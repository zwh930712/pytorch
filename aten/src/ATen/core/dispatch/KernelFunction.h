#pragma once

#include <ATen/core/stack.h>
#include <c10/util/TypeList.h>
#include <ATen/core/op_registration/kernel_functor.h>
#include <ATen/core/op_registration/kernel_function.h>
#include <ATen/core/op_registration/kernel_lambda.h>

namespace c10 {

namespace detail {
template<class Type>
constexpr uint64_t hashType() {
  return typeid(Type).hash_code()
      + 1000 * std::is_lvalue_reference<Type>::value
      + 5000 * std::is_rvalue_reference<Type>::value
      + 10000 * std::is_const<guts::remove_reference_t<Type>>::value
      + 15000 * std::is_volatile<guts::remove_reference_t<Type>>::value
      ;
}
template<class TypeList> struct hashTypeList_ final {};
template<class Head, class... Tail>
struct hashTypeList_<guts::typelist::typelist<Head, Tail...>> final {
  static constexpr uint64_t call(uint64_t index) {
    return 1000000 * index * hashType<Head>() + hashTypeList_<guts::typelist::typelist<Tail...>>::call(index + 1);
  }
};
template<>
struct hashTypeList_<guts::typelist::typelist<>> final {
  static constexpr uint64_t call(uint64_t index) {
    return 0;
  }
};

template<class TypeList>
constexpr uint64_t hashTypeList() {
  return hashTypeList_<TypeList>::call(1);
}

// Take a function signature and produce a hash value depending on its
// argument and return types. For the same function signature and while
// running the same executable, this will always produce the same hash.
// A different compiler/OS might generate different hashes, so don't use
// these for serialization, but they're good for error checking the casting
// of void* function pointers into actual typed function pointers.
// Note that there it is not perfect error checking, two different signatures
// might have the same hash, but it is probably good enough.
template<class FuncSignature>
constexpr uint64_t hashFunctionSignature() {
#ifdef __CUDACC__
  // Disabling because the CUDA compiler complains.
  // TODO Fix this
  return 0;
#else
  using func_traits = guts::infer_function_traits_t<FuncSignature>;
  return hashTypeList<
    guts::typelist::concat_t<
      guts::typelist::typelist<typename func_traits::return_type>,
      typename func_traits::parameter_types
    >>();
#endif
}

template<class Return, class... Args> struct boxAndCallBoxedFunc;
}

/**
 * KernelFunction is similar to std::function but stores a kernel function.
 * You can create a KernelFunction from a boxed or unboxed function/functor/lambda
 * and call it in a boxed or unboxed way. If the way it was created doesn't
 * match the way it was called, it will do boxing or unboxing as necessary.
 */
class CAFFE2_API KernelFunction final {
public:
  using BoxedKernelFunction = void(OperatorKernel*, Stack*);

  KernelFunction()
  : functorCreator_()
  , functor_(nullptr)
  , boxed_kernel_func_(nullptr)
  , unboxed_kernel_func_(nullptr)
  , signature_hash_(c10::nullopt)
  {}

  bool isValid() const {
    // TODO We want to introduce the invariant that all kernels must be callable in a boxed way, then this should only check boxed_kernel_func_.
    return boxed_kernel_func_ != nullptr || unboxed_kernel_func_ != nullptr;
  }

  /**
   * Call the function in a boxed way.
   * If the kernel function was created with an unboxed function,
   * this will call an unboxing wrapper which then calls into that
   * unboxed function.
   *
   * Example:
   *
   * > void boxed_func(OperatorKernel*, Stack* stack) {...}
   * > KernelFunction func = KernelFunction::makeFromBoxedFunction(&boxed_func);
   * > Tensor result = func.callBoxed(stack);
   *
   * Or, with an unboxed implementation:
   *
   * > KernelFunction func = KernelFunction::makeFromUnboxedLambda(
   * >      [] (Tensor a, bool b) -> Tensor {...});
   * > Tensor result = func.callBoxed(stack);
   */
  void callBoxed(Stack* stack) const {
    if (C10_UNLIKELY(boxed_kernel_func_ == nullptr)) {
      if (unboxed_kernel_func_ == nullptr) {
        TORCH_INTERNAL_ASSERT(false, "Tried to call KernelFunction::callBoxed() on an uninitizliaed KernelFunction.");
      } else {
        // TODO We want to introduce the invariant that all kernels must be callable in a boxed way, then this case should be impossible.
        TORCH_INTERNAL_ASSERT(false, "Tried to call KernelFunction::callBoxed() on a KernelFunction that can only be called with KernelFunction::callUnboxed().");
      }
    }

    (*boxed_kernel_func_)(getFunctor_(), stack);
  }

  /**
   * Call the function in an unboxed way.
   * As the "Only" in the name suggests, this only works for KernelFunctions
   * that are backed by an unboxed kernel. If the KernelFunction was created
   * in a boxed way, this will fail (also see KernelFunction::callUnboxed()).
   *
   * KernelFunction::callUnboxed() is generally better, since it will allow
   * calling KernelFunctions that are backed by either boxed or unboxed
   * kernels, but that one will not work for all types.
   *
   * Example:
   *
   * > KernelFunction func = KernelFunction::makeFromUnboxedLambda(
   * >      [] (Tensor a, bool b) -> Tensor {...});
   * > Tensor result = func.callUnboxedOnly<Tensor, Tensor, bool>(tensor1, true);
   */
  template<class Return, class... Args>
  Return callUnboxedOnly(Args... args) const {
    // TODO Remove this function once all kernels support a boxed variant

    TORCH_INTERNAL_ASSERT(!signature_hash_.has_value() || (detail::hashFunctionSignature<Return (Args...)>() == *signature_hash_),
      "Called KernelFunction::callUnboxed with wrong argument types");

    if (unboxed_kernel_func_ != nullptr) {
      using ActualSignature = Return (OperatorKernel*, Args...);
      ActualSignature* func = reinterpret_cast<ActualSignature*>(unboxed_kernel_func_);
      return (*func)(getFunctor_(), std::forward<Args>(args)...);
    }

    TORCH_INTERNAL_ASSERT(false, "Tried to call KernelFunction::callUnboxedOnly() for a kernel that doesn't have an unboxed version.");
  }

  /**
   * Call the function in an unboxed way.
   * If the kernel function was created with a boxed function,
   * this will box all inputs and then call into that boxed function.
   *
   * Note that this doesn't work for all types yet.
   *
   * Example:
   *
   * > KernelFunction func = KernelFunction::makeFromUnboxedLambda(
   * >      [] (Tensor a, bool b) -> Tensor {...});
   * > Tensor result = func.callUnboxed<Tensor, Tensor, bool>(tensor1, true);
   *
   * Or, with a boxed implementation:
   *
   * > void boxed_func(OperatorKernel*, Stack* stack) {...}
   * > KernelFunction func = KernelFunction::makeFromBoxedFunction(&boxed_func);
   * > Tensor result = func.callUnboxed<Tensor, Tensor, bool>(tensor1, true);
   */
  template<class Return, class... Args>
  Return callUnboxed(Args... args) const {
    TORCH_INTERNAL_ASSERT(!signature_hash_.has_value() || (detail::hashFunctionSignature<Return (Args...)>() == *signature_hash_),
      "Called KernelFunction::callUnboxed with wrong argument types");

    if (unboxed_kernel_func_ != nullptr) {
      using ActualSignature = Return (OperatorKernel*, Args...);
      ActualSignature* func = reinterpret_cast<ActualSignature*>(unboxed_kernel_func_);
      return (*func)(getFunctor_(), std::forward<Args>(args)...);
    }

    TORCH_INTERNAL_ASSERT(boxed_kernel_func_ != nullptr, "Tried to call KernelFunction::callUnboxed() on an uninitialized KernelFunction.");
    return detail::boxAndCallBoxedFunc<Return, Args...>::call(boxed_kernel_func_, getFunctor_(), std::forward<Args>(args)...);
  }

  /**
   * Create a KernelFunction from a boxed function.
   *
   * Example:
   *
   * > void boxed_func(OperatorKernel*, Stack* stack) {...}
   * > KernelFunction func = KernelFunction::makeFromBoxedFunction(&boxed_func);
   */
  static KernelFunction makeFromBoxedFunction(BoxedKernelFunction* func) {
    return KernelFunction(
      nullptr,  // no functorCreator_, this can only be called in a boxed way.
      nullptr,  // no functor_ object either
      func,
      nullptr,  // no unboxed function pointer
      c10::nullopt  // signature is not known, we can't error check unboxed calls.
    );
  }

  /**
   * Create a KernelFunction from an unboxed functor.
   *
   * Example:
   *
   * > class MyFunctor final {
   * >   public:
   * >     Tensor operator()(Tensor a, Tensor b) {...}
   * > };
   * > KernelFunction func = KernelFunction::makeFromUnboxedFunctor(std::make_shared<MyFunctor>());
   */
  template<bool AllowLegacyTypes = false, class KernelFunctor>
  static KernelFunction makeFromUnboxedFunctor(std::shared_ptr<KernelFunctor> kernelFunctor) {
    static_assert(guts::is_functor<KernelFunctor>::value, "Tried to call KernelFunction::makeFromUnboxedFunctor<KernelFunctor> but the argument is not a functor.");
    static_assert(std::is_base_of<OperatorKernel, KernelFunctor>::value, "Tried to call KernelFunction::makeFromUnboxedFunctor<KernelFunctor>, but the functor doesn't inherit from c10::OperatorKernel. Please have the functor inherit from it.");

    return KernelFunction(
      nullptr, // no functorCreator_ because we already have the functor_
      std::move(kernelFunctor),
      &detail::wrap_kernel_functor_boxed<KernelFunctor, AllowLegacyTypes>::call,
      reinterpret_cast<void*>(&detail::wrap_kernel_functor_unboxed<KernelFunctor>::call),
      detail::hashFunctionSignature<KernelFunctor>()
    );
  }

  /**
   * Create a KernelFunction from an unboxed functor and delay functor creation
   * until the first call to the KernelFunction. This is useful for functors
   * that are registered at static initialization time but can't be created
   * there yet. For example, some operator functors store Tensor members
   * (we can't create Tensor objects at static initialization time because of SIOF)
   * but these functors are registered as kernels at static initialization time.
   * Using this method, we can delay functor instantiation until the operator
   * is called for the first time.
   *
   * Example:
   *
   * > class MyFunctor final {
   * >   public:
   * >     Tensor operator()(Tensor a, Tensor b) {...}
   * > };
   * > KernelFunction func = KernelFunction::makeFromUnboxedFunctor([] {
   * >   return std::make_shared<MyFunctor>();
   * > });
   */
  template<bool AllowLegacyTypes = false, class KernelFunctor>
  static KernelFunction makeFromUnboxedFunctor(std::function<std::shared_ptr<KernelFunctor>()> kernelFunctorCreator) {
    static_assert(guts::is_functor<KernelFunctor>::value, "Tried to call KernelFunction::makeFromUnboxedFunctor<KernelFunctor> but the argument is not a functor.");
    static_assert(std::is_base_of<OperatorKernel, KernelFunctor>::value, "Tried to call KernelFunction::makeFromUnboxedFunctor<KernelFunctor>, but the functor doesn't inherit from c10::OperatorKernel. Please have the functor inherit from it.");

    return KernelFunction(
      std::move(kernelFunctorCreator),
      nullptr, // delay creation of functor_ (it will be created by calling functorCreator_ later)
      &detail::wrap_kernel_functor_boxed<KernelFunctor, AllowLegacyTypes>::call,
      reinterpret_cast<void*>(&detail::wrap_kernel_functor_unboxed<KernelFunctor>::call),
      detail::hashFunctionSignature<KernelFunctor>()
    );
  }

  /**
   * Create a KernelFunction from an unboxed functor and prevent creation of an
   * unboxing-wrapper. This means that you can only call this KernelFunction
   * using KernelFunction::callUnboxedOnly(), not using KernelFunction::callBoxed()
   * or KernelFunction::callUnboxed().
   *
   * This is necessary because our unboxing wrappers don't work for all types
   * yet, so if you want to use one of these types as function arguments,
   * you need to use makeFromUnboxedOnlyFunctor.
   *
   * Example:
   *
   * > class MyFunctor final {
   * >   public:
   * >     Tensor operator()(Tensor a, Tensor b) {...}
   * > };
   * > KernelFunction func = KernelFunction::makeFromUnboxedOnlyFunctor(std::make_shared<MyFunctor>());
   */
  template<class KernelFunctor>
  static KernelFunction makeFromUnboxedOnlyFunctor(std::shared_ptr<KernelFunctor> kernelFunctor) {
    // TODO We want to get rid of kernels that have only an unboxed function pointer.
    //      All kernels should have a boxed pointer.

    static_assert(guts::is_functor<KernelFunctor>::value, "Tried to call KernelFunction::makeFromUnboxedFunctor<KernelFunctor> but the argument is not a functor.");
    static_assert(std::is_base_of<OperatorKernel, KernelFunctor>::value, "Tried to call KernelFunction::makeFromUnboxedFunctor<KernelFunctor>, but the functor doesn't inherit from c10::OperatorKernel. Please have the functor inherit from it.");

    return KernelFunction(
      nullptr, // no functorCreator_ because we already have the functor_
      std::move(kernelFunctor),
      nullptr, // Don't create a boxed kernel for this
      reinterpret_cast<void*>(&detail::wrap_kernel_functor_unboxed<KernelFunctor>::call),
      detail::hashFunctionSignature<KernelFunctor>()
    );
  }

  /**
   * Create a KernelFunction from an unboxed function.
   * This is usually better than KernelFunction::makeFromUnboxedRuntimeFunction
   * because knowing the function pointer as a template argument (i.e. at
   * compile time) allows the compiler to inline the function into its
   * unboxing wrapper and yields better performance when calling the function.
   *
   * Example:
   *
   * > Tensor unboxed_func(Tensor a, Tensor b) {...}
   * > KernelFunction func = KernelFunction::makeFromUnboxedFunction<decltype(unboxed_func), &unboxed_func>();
   */
  template<class FuncType, FuncType* func, bool AllowLegacyTypes = false>
  static KernelFunction makeFromUnboxedFunction() {
    static_assert(guts::is_function_type<FuncType>::value, "Tried to call KernelFunction::makeFromUnboxedFunction with invalid template parameters. They must be <FuncType, *func_ptr>.");
    static_assert(!std::is_same<FuncType, BoxedKernelFunction>::value, "Tried to call KernelFunction::makeFromUnboxedFunction with a boxed function pointer. Please use KernelFunction::makeFromBoxedFunction instead.");
    static_assert(func != nullptr, "Kernel function cannot be nullptr");

    return makeFromUnboxedFunctor<AllowLegacyTypes>(
      std::make_shared<typename detail::WrapKernelFunction<FuncType, func>::type>()
    );
  }

  /**
   * Create a KernelFunction from an unboxed function and prevent creation of an
   * unboxing-wrapper. This means that you can only call this KernelFunction
   * using KernelFunction::callUnboxedOnly(), not using KernelFunction::callBoxed()
   * or KernelFunction::callUnboxed().
   *
   * This is necessary because our unboxing wrappers don't work for all types
   * yet, so if you want to use one of these types as function arguments,
   * you need to use makeFromUnboxedOnlyFunctor.
   *
   * Example:
   *
   * > Tensor unboxed_func(Tensor a, Tensor b) {...}
   * > KernelFunction func = KernelFunction::makeFromUnboxedOnlyFunction<decltype(unboxed_func), &unboxed_func>();
   */
  template<class FuncType, FuncType* func>
  static KernelFunction makeFromUnboxedOnlyFunction() {
    // TODO We want to get rid of kernels that have only an unboxed function pointer.
    //      All kernels should have a boxed pointer.

    static_assert(guts::is_function_type<FuncType>::value, "Tried to call KernelFunction::makeFromUnboxedOnlyFunction with invalid template parameters. They must be <FuncType, *func_ptr>.");
    static_assert(!std::is_same<FuncType, BoxedKernelFunction>::value, "Tried to call KernelFunction::makeFromUnboxedOnlyFunction with a boxed function pointer. Please use KernelFunction::makeFromBoxedFunction instead.");
    static_assert(func != nullptr, "Kernel function cannot be nullptr");

    return makeFromUnboxedOnlyFunctor(
      std::make_shared<typename detail::WrapKernelFunction<FuncType, func>::type>()
    );
  }

  /**
   * Create a KernelFunction from an unboxed function.
   * KernelFunction::makeFromUnboxedFunction is usually a better choice than
   * this if you know the function pointer at compile time, see doc comment
   * there for an explanation.
   *
   * Example:
   *
   * > Tensor unboxed_func(Tensor a, Tensor b) {...}
   * > KernelFunction func = KernelFunction::makeFromUnboxedRuntimeFunction(&unboxed_func);
   */
  template<bool AllowLegacyTypes = false, class FuncType>
  static KernelFunction makeFromUnboxedRuntimeFunction(FuncType* func) {
    static_assert(guts::is_function_type<FuncType>::value, "Tried to call KernelFunction::makeFromUnboxedRuntimeFunction with a non-function type.");
    static_assert(!std::is_same<FuncType, BoxedKernelFunction>::value, "Tried to call KernelFunction::makeFromUnboxedRuntimeFunction with a boxed function pointer. Please use KernelFunction::makeFromBoxedFunction instead.");
    TORCH_INTERNAL_ASSERT(func != nullptr, "Kernel function cannot be nullptr");

    return makeFromUnboxedFunctor<AllowLegacyTypes>(
      std::make_shared<detail::WrapRuntimeKernelFunctor<guts::decay_t<FuncType>>>(func)
    );
  }

  /**
   * Create a KernelFunction from an unboxed lambda.
   *
   * Example:
   *
   * > KernelFunction func = KernelFunction::makeFromUnboxedLambda(
   * >      [] (Tensor a, bool b) -> Tensor {...});
   */
  template<bool AllowLegacyTypes = false, class Lambda>
  static KernelFunction makeFromUnboxedLambda(Lambda&& lambda) {
    static_assert(guts::is_functor<guts::decay_t<Lambda>>::value, "Tried to call KernelFunction::makeFromUnboxedLambda with a non-lambda type.");

    return makeFromUnboxedFunctor<AllowLegacyTypes>(
      std::make_shared<detail::WrapRuntimeKernelFunctor<guts::decay_t<Lambda>>>(std::forward<Lambda>(lambda))
    );
  }

private:

  explicit KernelFunction(std::function<std::shared_ptr<OperatorKernel>()> functorCreator, std::shared_ptr<OperatorKernel> functor, BoxedKernelFunction* boxed_kernel_func, void* unboxed_kernel_func, c10::optional<uint64_t> signature_hash)
  : functorCreator_(std::move(functorCreator))
  , functor_(std::move(functor))
  , boxed_kernel_func_(boxed_kernel_func)
  , unboxed_kernel_func_(unboxed_kernel_func)
  , signature_hash_(signature_hash)
  {}

  OperatorKernel* getFunctor_() const {
    if (functor_.get() == nullptr) {
      if (!functorCreator_) {
        return nullptr;
      }
      functor_ = functorCreator_();
    }
    return functor_.get();
  }

  // If the operator has an unboxed_kernel_func, then either
  // functorCreator_ or functor_ must be set, possibly both.
  // If functor_ is not set but functorCreator_ is, we will create
  // functor_ by calling functorCreator_ the first time it is needed.
  // We use this indirection because many KernelFunctions are created
  // at static initialization time but are created with functors that
  // store Tensor and we can't call the Tensor() constructor at static
  // initialization time yet (SIOF). So these register with a
  // functorCreator_ instead of a functor_ and will be initialized
  // on the first call to the KernelFunction.
  std::function<std::shared_ptr<OperatorKernel>()> functorCreator_;
  mutable std::shared_ptr<OperatorKernel> functor_;

  BoxedKernelFunction* boxed_kernel_func_;
  void* unboxed_kernel_func_;

  // signature_hash_ is set to the hash of the function signature if the
  // KernelFunction was created in a way that allowed us to know the function
  // signature. If this is set, it will be used in unboxed function calls
  // to verify their arguments against the known function signature.
  c10::optional<uint64_t> signature_hash_;
};

namespace detail {
template<class Return, class... Args>
struct boxAndCallBoxedFunc final {
  static Return call(KernelFunction::BoxedKernelFunction* boxed_kernel_func, OperatorKernel* functor, Args... args) {
    // TODO Reuse stack vector instead of allocating?
    std::vector<IValue> stack {std::forward<Args>(args)...};

    (*boxed_kernel_func)(functor, &stack);

    TORCH_INTERNAL_ASSERT(stack.size() == 1, "A boxed kernel should only push one return to the stack");
    return std::move(stack[0]).to<Return>();
  }
};
template<class... Args>
struct boxAndCallBoxedFunc<void, Args...> final {
  static void call(KernelFunction::BoxedKernelFunction* boxed_kernel_func, OperatorKernel* functor, Args... args) {
    // TODO Reuse stack vector instead of allocating?
    std::vector<IValue> stack {std::forward<Args>(args)...};

    (*boxed_kernel_func)(functor, &stack);

    TORCH_INTERNAL_ASSERT(stack.size() == 0, "A boxed kernel returned a value but when we called it with KernelFunction::callUnboxed, we expected it to return void.");
  }
};
}

}

// TODO Test all KernelFunction::makeFromXXX() functions, each with callBoxed, callUnboxed and callUnboxedOnly. Make sure to test both, regular and void returns.
// TODO Also test different variants of calling unboxed with wrong signatures
