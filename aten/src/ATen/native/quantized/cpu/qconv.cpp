#include <ATen/ATen.h>
#include <ATen/SmallVector.h>
#include <ATen/core/op_registration/op_registration.h>
#include <ATen/cpp_custom_type_hack.h>
#include <ATen/native/quantized/cpu/fbgemm_utils.h>
#include <cmath>

namespace at {
namespace native {
namespace {

SmallVector<int64_t, 4> convOutputShape(
    int N, // mini-batch
    int H, // input height
    int W, // input width
    int K, // output channels
    const std::vector<int64_t>& kernel,
    const torch::List<int64_t>& stride,
    const torch::List<int64_t>& padding,
    const torch::List<int64_t>& dilation) {
  SmallVector<int64_t, 4> out_shape;
  out_shape.push_back(N);

  int H_out = std::floor(
      (H + 2 * padding[0] - dilation[0] * (kernel[0] - 1) - 1) / stride[0] + 1);
  int W_out = std::floor(
      (W + 2 * padding[1] - dilation[1] * (kernel[1] - 1) - 1) / stride[1] + 1);
  out_shape.push_back(H_out);
  out_shape.push_back(W_out);
  out_shape.push_back(K);

  return out_shape;
}

/*
 * FBGEMM uses vpmaddubsw instruction to multiply activations (uint8_t) and
 * weights (int8_t).
 *
 * https://software.intel.com/sites/landingpage/IntrinsicsGuide/#text=_mm256_maddubs_epi16&expand=3284,3530
 *
 * vpmaddubsw operates on a vector of activations and a vector of
 * weights. If these vectors are
 *
 *    A (uint8_t) = a0, a1, a2, a3 ...
 *
 * and
 *
 *    B (int8_t)  = b0, b1, b2, b3 ...
 *
 * the result of this instruction is an int16_t vector with values
 *
 *    C (int16_t) = a0*b0 + a1*b1, a2*b2 + a3*b3 ...
 *
 * For large values of A and/or B the result (a0*b0 + a1*b1) might not fit into
 * an int16_t number. So the instruction saturates them to max (or min) possible
 * value of an int16_t number. Such behavior is expected for the
 * implementation below.
 *
 * For example, a0 = 255, a1 = 255, b0 = 127 and b1 = 127 the actual result
 * 64770 overflows for an int16_t number (-32768, 32767) so the returned result
 * is 32767.
 *
 */
template <bool ReluFused>
class QConv2dInt8 final : public c10::OperatorKernel {
 public:
#ifdef USE_FBGEMM
  Tensor operator()(
      Tensor act,
      Tensor packed_weight,
      torch::List<int64_t> stride,
      torch::List<int64_t> padding,
      torch::List<int64_t> dilation,
      int64_t groups,
      double output_scale,
      int64_t output_zero_point) {
    TORCH_CHECK(
        fbgemm::fbgemmSupportedCPU(), "Your CPU does not support FBGEMM.");
    TORCH_CHECK(
        act.ndimension() == 4,
        "Activations are supposed to have 4 dimensions.");
    TORCH_CHECK(stride.size() == 2, "2D convolution only");
    TORCH_CHECK(padding.size() == 2, "2D convolution only");
    TORCH_CHECK(dilation.size() == 2, "2D convolution only");
    // inputs are in NHWC format
    int N = act.size(0);
    int H = act.size(1);
    int W = act.size(2);
    int C = act.size(3);

    Tensor act_contig = act.contiguous();
    const uint8_t* act_ptr =
        reinterpret_cast<uint8_t*>(act_contig.data_ptr<c10::quint8>());

    PackedConvWeight& pack_ptr =
        cpp_custom_type_hack::cast<PackedConvWeight>(packed_weight);
    auto packB = pack_ptr.w.get();
    auto& col_offsets = pack_ptr.col_offsets;
    auto& kernel = pack_ptr.kernel;

    int K = packB->outputChannels();

    int pad_l = padding[0];
    int pad_t = padding[1];
    int stride_h = stride[0];
    int stride_w = stride[1];
    int kernel_h = kernel[0];
    int kernel_w = kernel[1];

    TORCH_CHECK(C == (packB->inputChannels()),
        "[QConv2D] Given groups=", groups, ", weight of size ",
        K, ", ",  kernel_h, ", ", kernel_w, ", ", packB->inputChannels(),
        ", expected input (NHWC) ", N, ", ", H, ", ", W, ", ", C,
        " to have ", (packB->inputChannels() * groups),
        " channels, but got ", C, " channels instead");

    fbgemm::conv_param_t<> conv_p(
        N, // Batch size
        C, // Number of input channels
        K, // Number of output channels
        {H, W},
        groups,
        {kernel_h, kernel_w},
        {stride_h, stride_w},
        {pad_l, pad_t, pad_l, pad_t},
        {static_cast<int>(dilation[0]), static_cast<int>(dilation[1])});

    fbgemm::DoNothing<> NoOpObj{};

    float act_scale = act.q_scale();
    int32_t act_zero_point = act.q_zero_point();

    const float* bias_ptr = nullptr;
    at::Tensor bias;
    if (pack_ptr.bias.has_value()) {
      bias = pack_ptr.bias.value();
      TORCH_CHECK(
          bias.dtype() == at::kFloat,
          "[QConv2D] The 'bias' tensor must have 'torch.float' dtype");
      bias = bias.contiguous();
      TORCH_CHECK(bias.dim() == 1, "bias should be a vector (1D Tensor)");
      TORCH_CHECK(
          bias.size(0) == K,
          "bias should have K elements: " + std::to_string(K));
      bias_ptr = bias.data_ptr<float>();
    }

    std::vector<float> output_multiplier_float(1, 0.0);
    std::vector<float> act_times_w_scale(1, 1.0);
    TORCH_CHECK(
        pack_ptr.w_scale.size() == pack_ptr.w_zp.size(),
        "Weight scales and zero points vectors should have the same size.");

    if (pack_ptr.q_scheme == kPerTensorAffine) {
      act_times_w_scale[0] = (act_scale * pack_ptr.w_scale[0]);
      output_multiplier_float[0] =
          act_times_w_scale[0] / static_cast<float>(output_scale);
    } else if (pack_ptr.q_scheme == kPerChannelAffine) {
      output_multiplier_float.resize(K, 0.0);
      act_times_w_scale.resize(K, 1.0);
      for (int i = 0; i < K; ++i) {
        act_times_w_scale[i] = (act_scale * pack_ptr.w_scale[i]);
        output_multiplier_float[i] =
            act_times_w_scale[i] / static_cast<float>(output_scale);
      }
    } else {
      TORCH_CHECK(false, "[QConv2D] Unknown quantization scheme");
    }

    auto outShape =
        convOutputShape(N, H, W, K, kernel, stride, padding, dilation);
    TORCH_CHECK(
        std::all_of(
            outShape.begin(), outShape.end(), [](int64_t i) { return i > 0; }),
        "[QConv2D] each dimension of output tensor should be greater than 0")

    Tensor output = _empty_affine_quantized(
        outShape, device(kCPU).dtype(kQUInt8), output_scale, output_zero_point);
    auto buffer = at::zeros_like(output, output.options().dtype(at::kInt));

    if (pack_ptr.q_scheme == kPerTensorAffine) {
      fbgemm::ReQuantizeOutput<
          ReluFused,
          fbgemm::QuantizationGranularity::TENSOR,
          float>
          outputProcObj(
              NoOpObj,
              output_multiplier_float.data(),
              output_zero_point,
              act_zero_point,
              pack_ptr.w_zp.data(),
              nullptr, /* row offset buffer */
              col_offsets.data(),
              bias_ptr,
              K,
              groups,
              act_times_w_scale.data());
      fbgemm::fbgemmConv(
          conv_p,
          act_ptr,
          *packB,
          reinterpret_cast<uint8_t*>(output.data_ptr<c10::quint8>()),
          buffer.data_ptr<int32_t>(),
          outputProcObj,
          0 /* thread_id*/,
          1 /* num_threads */);

    } else if (pack_ptr.q_scheme == kPerChannelAffine) {
      fbgemm::ReQuantizeOutput<
          ReluFused,
          fbgemm::QuantizationGranularity::OUT_CHANNEL,
          float>
          outputProcObj(
              NoOpObj,
              output_multiplier_float.data(),
              output_zero_point,
              act_zero_point,
              pack_ptr.w_zp.data(),
              nullptr, /* row offset buffer */
              col_offsets.data(),
              bias_ptr,
              K,
              groups,
              act_times_w_scale.data());

      fbgemm::fbgemmConv(
          conv_p,
          act_ptr,
          *packB,
          reinterpret_cast<uint8_t*>(output.data_ptr<c10::quint8>()),
          buffer.data_ptr<int32_t>(),
          outputProcObj,
          0 /* thread_id*/,
          1 /* num_threads */);
    }

    return output;
  }
#else // USE_FBGEMM
  Tensor operator()(
      Tensor /* activation */,
      Tensor /* packed_weight */,
      torch::List<int64_t> /* stride */,
      torch::List<int64_t> /* padding */,
      torch::List<int64_t> /* dilation */,
      torch::List<int64_t> /* output padding */,
      int64_t /* groups */,
      double /* output scale */,
      int64_t /* output_zero_point */) {
    TORCH_CHECK(
        false,
        "This PyTorch installation was not built "
        "with FBGEMM operators");
  }
#endif // USE_FBGEMM
};

static auto registry =
    c10::RegisterOperators()
        .op("quantized::conv2d",
            c10::RegisterOperators::options().kernel<QConv2dInt8<false>>(
                TensorTypeId::QuantizedCPUTensorId))
        .op("quantized::conv2d_relu",
            c10::RegisterOperators::options().kernel<QConv2dInt8<true>>(
                TensorTypeId::QuantizedCPUTensorId));

} // namespace
} // namespace native
} // namespace at
