#include <blas.hpp>
#include <af/blas.h>
#include <Array.hpp>
#include <clBLAS.h>
#include <helper.hpp>
#include <cassert>
#include <string>
#include <iostream>
#include <functional>
#include <stdexcept>
#include <mutex>

namespace opencl
{

using std::is_floating_point;
using std::enable_if;
using std::once_flag;
using std::call_once;
using std::runtime_error;
using std::to_string;

clblasTranspose
toClblasTranspose(af_blas_transpose opt)
{
    clblasTranspose out;
    switch(opt) {
        case AF_NO_TRANSPOSE        : out = clblasNoTrans;   break;
        case AF_TRANSPOSE           : out = clblasTrans;     break;
        case AF_CONJUGATE_TRANSPOSE : out = clblasConjTrans; break;
        default                     : assert( "INVALID af_blas_transpose" && 1!=1);
    }
    return out;
}

#define BLAS_FUNC_DEF(NAME)                                             \
template<typename T>                                                    \
struct NAME##_func;

#define BLAS_FUNC(NAME, TYPE, PREFIX)                                   \
template<>                                                              \
struct NAME##_func<TYPE>                                                \
{                                                                       \
    template<typename... Args>                                          \
    clblasStatus                                                        \
    operator() (Args... args) { return clblas##PREFIX##NAME(args...); } \
};

BLAS_FUNC_DEF(gemm)
BLAS_FUNC(gemm, float,      S)
BLAS_FUNC(gemm, double,     D)
BLAS_FUNC(gemm, cfloat,     C)
BLAS_FUNC(gemm, cdouble,    Z)

BLAS_FUNC_DEF(gemv)
BLAS_FUNC(gemv, float,      S)
BLAS_FUNC(gemv, double,     D)
BLAS_FUNC(gemv, cfloat,     C)
BLAS_FUNC(gemv, cdouble,    Z)

BLAS_FUNC_DEF( dot )
BLAS_FUNC(dot, float,       S)
BLAS_FUNC(dot, double,      D)

#undef BLAS_FUNC_DEF
#undef BLAS_FUNC


template<typename T>
typename enable_if<is_floating_point<T>::value, T >::type
getValue(T val)         { return T(val); }

template<typename T>
typename enable_if<is_complex<T>::value, T>::type
getValue(double val)
{
    T tmp;
    tmp.s[0] = val;
    tmp.s[1] = 0;
    return tmp;
}


static void
initBlas() {
    static std::once_flag clblasSetupFlag;
    call_once(clblasSetupFlag, clblasSetup);
}

template<typename T>
Array<T>* matmul(const Array<T> &lhs, const Array<T> &rhs,
                    af_blas_transpose optLhs, af_blas_transpose optRhs)
{
    initBlas();
    clblasTranspose lOpts = toClblasTranspose(optLhs);
    clblasTranspose rOpts = toClblasTranspose(optRhs);

    int aRowDim = (lOpts == clblasNoTrans) ? 0 : 1;
    int aColDim = (lOpts == clblasNoTrans) ? 1 : 0;
    int bRowDim = (rOpts == clblasNoTrans) ? 0 : 1;
    int bColDim = (rOpts == clblasNoTrans) ? 1 : 0;

    dim4 lDims = lhs.dims();
    dim4 rDims = rhs.dims();
    int M = lDims[aRowDim];
    int N = rDims[bColDim];
    int K = lDims[aColDim];

    assert(lDims[aColDim] == rDims[bRowDim]);

    //FIXME: Leaks on errors.
    Array<T> *out = createValueArray<T>(af::dim4(M, N, 1, 1), getValue<T>(0));
    auto scale = getValue<T>(1);

    dim4 lStrides = lhs.strides();
    dim4 rStrides = rhs.strides();
    clblasStatus err;
    cl::Event event;
    if(rDims[bColDim] == 1) {
        N = lDims[aColDim];
        gemv_func<T> gemv;
        err = gemv(
            clblasColumnMajor, lOpts,
            M, N,
            scale,  lhs.get()(),    lhs.getOffset(),   lStrides[1],
                    rhs.get()(),    rhs.getOffset(),   rStrides[0],
            scale,  out->get()(),   out->getOffset(),             1,
            1, &getQueue(0)(), 0, nullptr, &event());
    } else {
        gemm_func<T> gemm;
        err = gemm(
                clblasColumnMajor, lOpts, rOpts,
                M, N, K,
                scale,  lhs.get()(),    lhs.getOffset(),   lStrides[1],
                        rhs.get()(),    rhs.getOffset(),   rStrides[1],
                scale,  out->get()(),   out->getOffset(),   out->dims()[0],
                1, &getQueue(0)(), 0, nullptr, &event());

    }
    if(err) {
        throw runtime_error(string("CLBLAS error: ") + to_string(err));
    }

    return out;
}

template<typename T>
Array<T>* dot(const Array<T> &lhs, const Array<T> &rhs,
                    af_blas_transpose optLhs, af_blas_transpose optRhs)
{
    initBlas();
    assert(lhs.dims()[0] == rhs.dims()[0]);
    int N = lhs.dims()[0];
    dot_func<T> dot;
    cl::Event event;
    auto out = createEmptyArray<T>(af::dim4(1));
    cl::Buffer scratch(getCtx(0), CL_MEM_READ_WRITE, sizeof(T) * N);
    clblasStatus err;
    err = dot(    N,
            out->get()(), out->getOffset(),
            lhs.get()(),  lhs.getOffset(), lhs.strides()[0],
            rhs.get()(),  rhs.getOffset(), rhs.strides()[0],
            scratch(),
            1, &getQueue(0)(), 0, nullptr, &event());

    if(err) {
        throw runtime_error(string("CLBLAS error: ") + to_string(err));
    }
    return out;
}

#define INSTANTIATE_BLAS(TYPE)                                                          \
    template Array<TYPE>* matmul<TYPE>(const Array<TYPE> &lhs, const Array<TYPE> &rhs,  \
                    af_blas_transpose optLhs, af_blas_transpose optRhs);

INSTANTIATE_BLAS(float)
INSTANTIATE_BLAS(cfloat)
INSTANTIATE_BLAS(double)
INSTANTIATE_BLAS(cdouble)

#define INSTANTIATE_DOT(TYPE)                                                       \
    template Array<TYPE>* dot<TYPE>(const Array<TYPE> &lhs, const Array<TYPE> &rhs, \
                    af_blas_transpose optLhs, af_blas_transpose optRhs);

template<typename T>
Array<T>* dot(const Array<T> &lhs, const Array<T> &rhs,
                    af_blas_transpose optLhs, af_blas_transpose optRhs);

INSTANTIATE_DOT(float)
INSTANTIATE_DOT(double)
}
