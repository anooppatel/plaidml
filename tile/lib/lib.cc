#include "tile/lib/lib.h"

#include <boost/format.hpp>

#include "base/util/stream_container.h"
#include "tile/lang/tile_cc.h"
#include "tile/util/tile_file.h"

namespace vertexai {
namespace tile {
namespace lib {

using namespace lang;  // NOLINT

namespace {

std::shared_ptr<BufferBase> MakeBuffer(const TensorShape& shape) {
  auto buffer = std::make_shared<util::SimpleBuffer>();
  buffer->bytes.resize(shape.byte_size());
  return buffer;
}

Tensor MatMul(const Tensor& A, const Tensor& B) {
  auto M = A.dims(0), N = B.dims(1);
  Index k("k"), m("m"), n("n");
  Tensor C("C");
  C({m, n}, {M, N}) += A({m, k}) * B({k, n});
  return C;
}

Tensor DilatedConvolution2(const Tensor& I, const Tensor& K) {
  auto N = I.dims(0), Lx = I.dims(1), Ly = I.dims(2), LKx = K.dims(0), LKy = K.dims(1), CO = K.dims(3);
  Tensor O("O");
  Index n, x, y, kx, ky, ci, co;
  O({n, x, y, co}, {N, Lx - 2 * (LKx - 1), Ly - 3 * (LKy - 1), CO}) +=
      I({n, x + 2 * kx, y + 3 * ky, ci}) * K({kx, ky, ci, co});
  return O;
}

Tensor Relu(const Tensor& X) { return Call("relu", {X}); }

Tensor Sin(const Tensor& X) { return Call("sin", {X}); }

Tensor Tanh(const Tensor& X) { return Call("tanh", {X}); }

}  // namespace

Tensor Convolution(const Tensor& I,                    //
                   const Tensor& K,                    //
                   const std::vector<size_t>& O_dims,  //
                   std::vector<size_t> strides,        //
                   ConvolutionFormat I_format,         //
                   ConvolutionFormat K_format) {
  auto I_shape = I.shape();
  auto K_shape = K.shape();
  auto rank = I_shape.dims.size() - 2;
  if (strides.empty()) {
    for (size_t i = 0; i < rank; i++) {
      strides.push_back(1);
    }
  } else if (strides.size() != rank) {
    throw std::runtime_error(
        str(boost::format("Convolution strides length inconsistent with input shape: %1% (rank %2%) v %3% (rank %4%)") %
            StreamContainer(strides) % strides.size() % I_shape % rank));
  }
  // auto N = I_shape.dims[0].size;
  Index n("n"), co("co"), ci("ci");
  Tensor O("O");
  std::vector<Index> K_idxs;
  std::vector<Index> I_idxs = {n};
  std::vector<Index> O_idxs = {n};
  // std::vector<size_t> O_sizes = {N};
  size_t K_spatial_dims_offset = 0;
  // size_t I_spatial_dims_offset = 1;
  // size_t CO;
  if (K_format == ConvolutionFormat::ChannelsFirst) {
    K_spatial_dims_offset = 2;
    K_idxs.push_back(co);
    K_idxs.push_back(ci);
    // CO = K_shape.dims[0].size;
  } else {
    // CO = K_shape.dims[rank + 1].size;
  }
  if (I_format == ConvolutionFormat::ChannelsFirst) {
    // I_spatial_dims_offset = 2;
    I_idxs.push_back(ci);
    O_idxs.push_back(co);
    // O_sizes.push_back(CO);
  }
  for (size_t i = 0; i < rank; i++) {
    Index x(str(boost::format("x%1%") % i));
    Index k(str(boost::format("k%1%") % i));
    // auto I_dim = I_shape.dims[I_spatial_dims_offset + i].size;
    auto K_dim = K_shape.dims[K_spatial_dims_offset + i].size;
    I_idxs.emplace_back(strides[i] * x + k - K_dim / 2);
    K_idxs.push_back(k);
    O_idxs.push_back(x);
    // O_sizes.push_back(I_dim - (K_dim - 1));
  }
  if (I_format == ConvolutionFormat::ChannelsLast) {
    I_idxs.push_back(ci);
    O_idxs.push_back(co);
    // O_sizes.push_back(CO);
  }
  if (K_format == ConvolutionFormat::ChannelsLast) {
    K_idxs.push_back(ci);
    K_idxs.push_back(co);
  }
  O(O_idxs, O_dims) += I(I_idxs) * K(K_idxs);
  return O;
}

RunInfo LoadMatMul(const std::string& name, const TensorShape& i1, const TensorShape& i2) {
  Tensor A(i1, "A");
  Tensor B(i2, "B");
  return Evaluate(name, {MatMul(A, B)});
}

RunInfo LoadMatMulIntermediate(const std::string& name, const TensorShape& i1, const TensorShape& i2,
                               const TensorShape& i3) {
  Tensor A(i1, "A");
  Tensor B(i2, "B");
  Tensor C(i2, "C");
  Tensor D = MatMul(A, B);
  Tensor E = D + C;
  return Evaluate(name, {D, E});
}

RunInfo LoadEltwiseMulFlip(const std::string& name, const TensorShape& i1, const TensorShape& i2) {
  Tensor A{i1, "A"}, B{i2, "B"};
  return Evaluate(name, {~(A * B)});
}

RunInfo LoadMatMulAmongEltwise(const std::string& name, const TensorShape& i1, const TensorShape& i2,
                               const TensorShape& i3) {
  Tensor A(i1, "A");
  Tensor B(i2, "B");
  Tensor C(i3, "C");
  Tensor NegA = -A;
  Tensor NegB = -B;
  Tensor P = MatMul(NegA, NegB);
  return Evaluate(name, {P + C});
}

RunInfo LoadEltwiseAdd(const std::string& name, const TensorShape& i1, const TensorShape& i2) {
  Tensor A(i1, "A");
  Tensor B(i2, "B");
  return Evaluate(name, {A + B});
}

RunInfo LoadEltwiseMultiAdd(const std::string& name, const TensorShape& i1, const TensorShape& i2,
                            const TensorShape& i3, const TensorShape& i4) {
  Tensor A(i1, "A");
  Tensor B(i2, "B");
  Tensor C(i3, "C");
  Tensor D(i4, "D");
  return Evaluate(name, {A + B + C + D});
}

RunInfo LoadEltwiseDiv(const std::string& name, const TensorShape& i1, const TensorShape& i2) {
  Tensor A(i1, "A");
  Tensor B(i2, "B");
  return Evaluate(name, {A / B});
}

RunInfo LoadEltwiseMul(const std::string& name, const TensorShape& i1, const TensorShape& i2) {
  Tensor A(i1, "A");
  Tensor B(i2, "B");
  return Evaluate(name, {A * B});
}

RunInfo LoadEltwiseMultiMul(const std::string& name, const TensorShape& i1, const TensorShape& i2,
                            const TensorShape& i3, const TensorShape& i4) {
  Tensor A(i1, "A");
  Tensor B(i2, "B");
  Tensor C(i3, "C");
  Tensor D(i4, "D");
  return Evaluate(name, {A * B * C * D});
}

RunInfo LoadSin(const std::string& name, const TensorShape& i1) {
  Tensor A(i1, "A");
  return Evaluate(name, {Sin(A)});
}

RunInfo LoadTanh(const std::string& name, const TensorShape& i1) {
  Tensor A(i1, "A");
  return Evaluate(name, {Tanh(A)});
}

RunInfo LoadMulThenNeg(const std::string& name, const TensorShape& i1, const TensorShape& i2) {
  Tensor A(i1, "A");
  Tensor B(i2, "B");
  Tensor C = A * B;
  return Evaluate(name, {-C});
}

RunInfo LoadNegThenMul(const std::string& name, const TensorShape& i1, const TensorShape& i2) {
  Tensor A(i1, "A");
  Tensor B(i2, "B");
  Tensor NegA = -A;
  Tensor NegB = -B;
  return Evaluate(name, {NegA * NegB});
}

RunInfo LoadConstCalc(const std::string& name) {
  Tensor N(1);
  Tensor F(0.0);
  Tensor F2(3.7);
  Index i;
  Tensor Simple;
  Simple({i}, {1}) = F({});
  Tensor DoubleN;
  DoubleN({i}, {1}) = N({});
  Tensor Partial = Simple + DoubleN;
  Tensor O = Partial + F2;
  return Evaluate(name, {O});
}

RunInfo LoadConv1d(const std::string& name,    //
                   const TensorShape& input,   //
                   const TensorShape& kernel,  //
                   const std::vector<size_t>& output) {
  Tensor I(input, "I");
  Tensor K(kernel, "K");
  auto runinfo = Evaluate(name, {Convolution(I, K, output)});
  runinfo.const_inputs = {"K"};
  runinfo.input_buffers = {{"K", MakeBuffer(kernel)}};
  return runinfo;
}

RunInfo LoadConv2d(const std::string& name,    //
                   const TensorShape& input,   //
                   const TensorShape& kernel,  //
                   const std::vector<size_t>& output) {
  Tensor I(input, "I");
  Tensor K(kernel, "K");
  auto runinfo = Evaluate(name, {Convolution(I, K, output)});
  runinfo.const_inputs = {"K"};
  runinfo.input_buffers = {{"K", MakeBuffer(kernel)}};
  return runinfo;
}

RunInfo LoadConv2dRelu(const std::string& name,    //
                       const TensorShape& input,   //
                       const TensorShape& kernel,  //
                       const std::vector<size_t>& output) {
  Tensor I(input, "I");
  Tensor K(kernel, "K");
  auto runinfo = Evaluate(name, {Relu(Convolution(I, K, output))});
  runinfo.const_inputs = {"K"};
  runinfo.input_buffers = {{"K", MakeBuffer(kernel)}};
  return runinfo;
}

RunInfo LoadConv2dBnRelu(const std::string& name,      //
                         const TensorShape& input,     //
                         const TensorShape& kernel,    //
                         const TensorShape& channels,  //
                         const std::vector<size_t>& output) {
  Tensor I(input, "I");
  Tensor K(kernel, "K");
  Tensor B(channels, "B");
  Tensor S(channels, "S");
  auto O = Convolution(I, K, output);
  auto R = Relu((O + B) * S);
  auto runinfo = Evaluate(name, {R});
  runinfo.const_inputs = {"K"};
  runinfo.input_buffers = {
      {"K", MakeBuffer(kernel)},
      {"B", MakeBuffer(channels)},
      {"S", MakeBuffer(channels)},
  };
  return runinfo;
}

RunInfo LoadConv2d3Deep(const std::string& name,     //
                        const TensorShape& input,    //
                        const TensorShape& kernel1,  //
                        const TensorShape& kernel2,  //
                        const TensorShape& kernel3) {
  Tensor I(input, "I");
  Tensor K1(input, "K1");
  Tensor K2(input, "K2");
  Tensor K3(input, "K3");
  auto I_dims = input.sizes();
  auto O1 = Convolution(I, K1, {I_dims[0], I_dims[1], I_dims[2], kernel1.dims[3].size});
  auto O2 = Convolution(O1, K2, {I_dims[0], I_dims[1], I_dims[2], kernel2.dims[3].size});
  auto O3 = Convolution(O2, K3, {I_dims[0], I_dims[1], I_dims[2], kernel3.dims[3].size});
  auto runinfo = Evaluate(name, {O3});
  runinfo.const_inputs = {"K1", "K2", "K3"};
  runinfo.input_buffers = {
      {"K1", MakeBuffer(kernel1)},
      {"K2", MakeBuffer(kernel2)},
      {"K3", MakeBuffer(kernel3)},
  };
  return runinfo;
}

RunInfo LoadDilatedConv2d(const std::string& name,   //
                          const TensorShape& input,  //
                          const TensorShape& kernel) {
  Tensor I(input);
  Tensor K(kernel);
  return Evaluate(name, {DilatedConvolution2(I, K)});
}

Tensor Normalize(const Tensor& X) {
  auto XSqr = X * X;
  Tensor X_MS;
  {
    std::vector<Index> idxs(X.shape().dims.size());
    X_MS({}) += XSqr(idxs);
  }
  return sqrt(X_MS);
}

std::tuple<Tensor, Tensor> LarsMomentum(const Tensor& X,           //
                                        const Tensor& Grad,        //
                                        const Tensor& Veloc,       //
                                        const Tensor& LR,          //
                                        double lars_coeff,         //
                                        double lars_weight_decay,  //
                                        double momentum) {
  auto XNorm = Normalize(X);
  auto GradNorm = Normalize(Grad);
  auto LocLR = LR * lars_coeff * XNorm / (GradNorm + lars_weight_decay * XNorm);
  auto NewVeloc = momentum * Veloc + LocLR * (Grad + lars_weight_decay * X);
  return std::make_tuple(X - NewVeloc, NewVeloc);
}

RunInfo LoadLarsMomentum4d(const std::string& name,     //
                           const TensorShape& x_shape,  //
                           const TensorShape& lr_shape) {
  // Note: X/Grad/Veloc/NewX/NewVeloc should all have the same shape for the
  // semantics of this operation to be correct, so we only pass in 1 shape for
  // all of them.
  double lars_coeff = 1. / 1024.;
  double lars_weight_decay = 1. / 2048.;
  double momentum = 1. / 8.;
  Tensor X(x_shape);
  Tensor Grad(x_shape);
  Tensor Veloc(x_shape);
  Tensor LR(lr_shape);
  auto R = LarsMomentum(X, Grad, Veloc, LR, lars_coeff, lars_weight_decay, momentum);
  return Evaluate("lars_momentum4d", {std::get<0>(R), std::get<1>(R)});
}

RunInfo LoadPow(const std::string& name,  //
                const TensorShape& i1,    //
                const TensorShape& i2) {
  Tensor X(i1, "X");
  Tensor Y(i2, "Y");
  auto runinfo = Evaluate(name, {pow(X, Y)});
  runinfo.input_buffers = {
      {"X", MakeBuffer(i1)},
      {"Y", MakeBuffer(i2)},
  };
  return runinfo;
}

Tensor Norm4dAx2(const Tensor& I, const Tensor& G, const Tensor& B, const Tensor& Epsilon) {
  int64_t H = I.dims(2) * I.dims(3);
  Tensor Sum;
  Index i0, i1, i2, i3;
  Sum({i0, i1, 0, 0}, {I.dims(0), I.dims(1), 1, 1}) += I({i0, i1, i2, i3});
  auto Mu = Sum / H;
  auto Diff = I - Mu;
  auto SqDiff = Diff * Diff;
  Tensor SumSqDiff;
  SumSqDiff({i0, i1, 0, 0}, {I.dims(0), I.dims(1), 1, 1}) += SqDiff({i0, i1, i2, i3});
  auto Stdev = sqrt(SumSqDiff + Epsilon) / H;
  return (G / Stdev) * (I - Mu) + B;
}

RunInfo LoadLayerNorm4dAx2(const std::string& name,  //
                           const TensorShape& input) {
  // Note: I/G/B/O should all have the same shape, so pass in one shape to share
  Tensor I(input);
  Tensor G(input);
  Tensor B(input);
  Tensor Epsilon(SimpleShape(DataType::FLOAT32, {}));
  return Evaluate(name, {Norm4dAx2(I, G, B, Epsilon)});
}

Tensor PolygonBoxTransform(const Tensor& I) {
  Tensor TEpartial;
  Tensor TOpartial;
  auto N = I.dims(0), C = I.dims(1), H = I.dims(2), W = I.dims(3);
  Index n, c, h, w;
  auto Widx = index(I, 3);
  TEpartial({2 * n, c, h, w}, {N, C, H, W}) = I({2 * n, c, h, w});
  auto TE = 4 * Widx - TEpartial;
  TOpartial({2 * n + 1, c, h, w}, {N, C, H, W}) = I({2 * n + 1, c, h, w});
  auto Hidx = index(I, 2);
  auto TO = 4 * Hidx - TOpartial;
  return TE + TO;
}

RunInfo LoadPolygonBoxTransform(const std::string& name,  //
                                const TensorShape& input) {
  // Note: I and O have the same shape
  Tensor I(input);
  return Evaluate(name, {PolygonBoxTransform(I)});
}

}  // namespace lib
}  // namespace tile
}  // namespace vertexai
