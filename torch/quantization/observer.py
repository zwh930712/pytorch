from __future__ import absolute_import, division, print_function, unicode_literals

import warnings
from abc import ABCMeta, abstractmethod
from functools import partial

from torch._jit_internal import Optional, List
import torch
import torch.nn as nn


ABC = ABCMeta(str("ABC"), (object,), {})  # compatible with Python 2 *and* 3:


class ObserverBase(ABC, nn.Module):
    r"""Observer base Module
    Any concrete observer implementation should derive from this class.

    Concrete observers should follow the same API. In forward, they will update
    the statistics of the observed Tensor. And they should provide a
    `calculate_qparams` function that computes the quantization parameters given
    the collected statistics.
    """

    def __init__(
        self, dtype=torch.quint8, qscheme=torch.per_tensor_affine, reduce_range=False
    ):
        super(ObserverBase, self).__init__()
        self.dtype = dtype
        self.qscheme = qscheme
        self.reduce_range = reduce_range

        self.eps = torch.finfo(torch.float32).eps
        assert self.qscheme in (
            torch.per_tensor_affine,
            torch.per_tensor_symmetric,
            torch.per_channel_affine,
            torch.per_channel_symmetric,
        ), "Default Observer only works for per_tensor_affine, \
                per_tensor_symmetric, per_channel_affine and \
                per_channel_symmetric quantization scheme"
        assert self.dtype in (
            torch.qint8,
            torch.quint8,
        ), "Default Observer only works for qint8 and quint8 data type"

    @abstractmethod
    def forward(self, x):
        pass

    @abstractmethod
    def calculate_qparams(self, **kwargs):
        pass

    def _calculate_per_channel_qparams(self, min_vals, max_vals):
        # type: (Optional[Tensor], Optional[Tensor]) -> Tuple[Tensor, Tensor]
        """
        Given min and max value tensors, this function calculates per channel
        quantization parameters
        """
        if min_vals is None or max_vals is None:
            warnings.warn(
                "must run observer before calling calculate_qparams.\
                                    Returning default scale and zero point "
            )
            return torch.tensor([1.0]), torch.tensor([0])

        for i in range(len(min_vals)):
            assert (
                min_vals[i] <= max_vals[i]
            ), "min {} should be less than max {}".format(min_vals[i], max_vals[i])

        scales = torch.ones(min_vals.size())
        zero_points = torch.ones(min_vals.size())
        for i in range(len(scales)):
            qparam = self._calculate_qparams(
                min_vals[i], max_vals[i]
            )
            scales[i] = float(qparam[0])
            zero_points[i] = int(qparam[1])

        return scales, zero_points

    def _calculate_qparams(self, min_val, max_val):
        # type: (Optional[Tensor], Optional[Tensor]) -> Tuple[Tensor, Tensor]
        """
        Given min and max values, this function calculates quantization parameters
        """

        if max_val is None or min_val is None:
            warnings.warn(
                "must run observer before calling calculate_qparams.\
                                    Returning default scale and zero point "
            )
            return torch.tensor([1.0]), torch.tensor([0])

        assert min_val <= max_val, "min {} should be less than max {}".format(
            min_val, max_val
        )

        if self.dtype == torch.qint8:
            if self.reduce_range:
                qmin, qmax = -64, 63
            else:
                qmin, qmax = -128, 127
        else:
            if self.reduce_range:
                qmin, qmax = 0, 127
            else:
                qmin, qmax = 0, 255

        max_val, min_val = float(max_val), float(min_val)
        min_val = min(0.0, min_val)
        max_val = max(0.0, max_val)
        if max_val == min_val:
            scale = 1.0
            zero_point = 0
        else:
            if self.qscheme == torch.per_tensor_symmetric or self.qscheme == torch.per_channel_symmetric:
                max_val = max(-min_val, max_val)
                scale = max_val / ((qmax - qmin) / 2)
                scale = max(scale, self.eps)
                zero_point = 0 if self.dtype == torch.qint8 else 128
            else:
                scale = (max_val - min_val) / float(qmax - qmin)
                scale = max(scale, self.eps)
                zero_point = qmin - round(min_val / scale)
                zero_point = max(qmin, zero_point)
                zero_point = min(qmax, zero_point)
                zero_point = int(zero_point)

        return torch.tensor([scale]), torch.tensor([zero_point])


class MinMaxObserver(ObserverBase):
    r"""Default Observer Module
    A default implementation of the observer module, only works for
    `per_tensor_affine` quantization scheme.  The module will record the
    running average of max and min value of the observed Tensor and
    calculate_qparams will calculate scale and zero_point
    """

    __annotations__ = {
        "min_val": Optional[torch.Tensor],
        "max_val": Optional[torch.Tensor],
    }

    def __init__(self, **kwargs):
        #  For x86 quantized kernels, we need to ensure that the vpmaddubsw instruction
        #  does not overflow. We allow for a reduce_range argument to observers that
        #  reduces the quantized range to (0,127) or (-64, 63). For more details see
        #  aten/src/ATen/native/quantized/cpu/qconv.cpp
        #  This is not the optimal choice for non x86 backends as
        #  lose a bit of precision for activations.
        #
        super(MinMaxObserver, self).__init__(**kwargs)
        self.min_val = None
        self.max_val = None
        if (
            self.qscheme == torch.per_tensor_symmetric
            and self.reduce_range
            and self.dtype == torch.quint8
        ):
            raise NotImplementedError(
                "Cannot reduce range for symmetric quantization for quint8"
            )

    def forward(self, x):
        min_val = self.min_val
        max_val = self.max_val
        if min_val is None or max_val is None:
            min_val = torch.min(x)
            max_val = torch.max(x)
        else:
            min_val = torch.min(torch.min(x), min_val)
            max_val = torch.max(torch.max(x), max_val)
        self.min_val = min_val
        self.max_val = max_val
        return x

    @torch.jit.export
    def calculate_qparams(self):
        return self._calculate_qparams(self.min_val, self.max_val)

    @torch.jit.export
    def extra_repr(self):
        return "min_val={}, max_val={}".format(self.min_val, self.max_val)


class PerChannelMinMaxObserver(ObserverBase):
    r"""Per Channel Observer Module
    The module will record the running average of max and min value for each
    channel of the observed Tensor and calculate_qparams will calculate
    scales and zero_points for each channel
    """

    def __init__(self, ch_axis=0, **kwargs):
        super(PerChannelMinMaxObserver, self).__init__(**kwargs)
        self.ch_axis = ch_axis
        self.min_vals = None
        self.max_vals = None

    def forward(self, x):
        with torch.no_grad():
            min_vals = self.min_vals
            max_vals = self.max_vals
            x_dim = x.size()

            new_axis_list = list(range(len(x_dim)))
            new_axis_list[self.ch_axis] = 0
            new_axis_list[0] = self.ch_axis
            y = x.permute(tuple(new_axis_list))
            y = torch.flatten(y, start_dim=1)
            if min_vals is None or max_vals is None:
                min_vals = torch.min(y, 1)[0]
                max_vals = torch.max(y, 1)[0]
            else:
                min_vals = torch.min(torch.min(y, 1)[0], min_vals)
                max_vals = torch.max(torch.max(y, 1)[0], max_vals)
            self.min_vals = min_vals
            self.max_vals = max_vals
            return x

    def calculate_qparams(self):
        return self._calculate_per_channel_qparams(self.min_vals, self.max_vals)

    def extra_repr(self):
        return "min_val={}, max_val={}".format(self.min_vals, self.max_vals)



class TensorObserver(ObserverBase):
    r"""
    The module is mainly for debug and records the tensor values during runtime
    """
    __annotations__ = {
        "tensor_val": List[Optional[torch.Tensor]],
    }

    def __init__(self, **kwargs):
        super(TensorObserver, self).__init__(**kwargs)
        self.tensor_val = []

    def forward(self, x):
        self.tensor_val.append(x.clone())
        return x

    @torch.jit.export
    def calculate_qparams(self):
        raise Exception("calculate_qparams should not be called for TensorObserver")

    @torch.jit.export
    def get_tensor_value(self):
        return self.tensor_val


def observer(observer_cls, **kwargs):
    return partial(observer_cls, **kwargs)


def default_observer(**kwargs):
    # Restrict activations to be in the range (0,127)
    kwargs.setdefault("reduce_range", True)
    return observer(MinMaxObserver, **kwargs)

def default_debug_observer(**kwargs):
    return observer(TensorObserver, **kwargs)

def default_weight_observer(**kwargs):
    kwargs.setdefault("dtype", torch.qint8)
    kwargs.setdefault("qscheme", torch.per_tensor_symmetric)
    return observer(MinMaxObserver, **kwargs)
