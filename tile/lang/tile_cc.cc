#include "tile/lang/tile_cc.h"

#include <algorithm>
#include <iterator>

#include <boost/format.hpp>

#include "base/util/logging.h"
#include "base/util/lookup.h"
#include "base/util/stream_container.h"
#include "tile/lang/ast.h"
#include "tile/lang/gen_special.h"
#include "tile/lang/ops.h"
#include "tile/math/polynomial.h"

namespace vertexai {
namespace tile {
namespace lang {

using Polynomial = math::Polynomial<math::Rational>;
using AstVector = std::vector<std::shared_ptr<Expr>>;

TensorShape EvaluateShape(const std::shared_ptr<Expr>& expr);

std::ostream& operator<<(std::ostream& os, const Expr* expr) {
  os << expr->str() << ": " << static_cast<const void*>(expr);
  return os;
}

std::string to_string(const Expr* expr) {
  std::stringstream ss;
  ss << expr;
  return ss.str();
}

struct Index::Impl {
  std::shared_ptr<PolyExpr> expr;
  std::vector<std::shared_ptr<ConstraintExpr>> constraints;
  Index MakePolyOp(const std::string& op, const std::vector<Index>& args);
};

struct Access::Impl {
  std::shared_ptr<Expr> expr;
  Tensor::Impl* src = nullptr;
  void MakeContraction(AggregationOp agg_op, const Access& rhs);
  Access MakeCall(const std::string& fn, const Access& rhs);
};

struct Tensor::Impl {
  std::shared_ptr<Expr> expr;
};

Index::Index() : impl_(std::make_shared<Impl>()) { impl_->expr = std::make_shared<PolyIndex>(impl_.get()); }

Index::Index(size_t value) : impl_(std::make_shared<Impl>()) { impl_->expr = std::make_shared<PolyLiteral>(value); }

Index::Index(const std::string& name) : impl_(std::make_shared<Impl>()) {
  impl_->expr = std::make_shared<PolyIndex>(impl_.get(), name);
}

Index::Index(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Index::~Index() = default;

Index Index::Impl::MakePolyOp(const std::string& op, const std::vector<Index>& args) {
  auto impl = std::make_unique<Impl>();
  std::vector<std::shared_ptr<PolyExpr>> operands;
  for (const auto& arg : args) {
    operands.push_back(arg.impl_->expr);
  }
  impl->expr = std::make_shared<PolyOp>(op, operands);
  return Index{std::move(impl)};
}

Index Index::operator-() const { return impl_->MakePolyOp("neg", {*this}); }
Index Index::operator+(const Index& rhs) const { return impl_->MakePolyOp("add", {*this, rhs}); }
Index Index::operator-(const Index& rhs) const { return impl_->MakePolyOp("sub", {*this, rhs}); }
Index Index::operator*(const Index& rhs) const { return impl_->MakePolyOp("mul", {*this, rhs}); }
Index Index::operator/(const Index& rhs) const { return impl_->MakePolyOp("div", {*this, rhs}); }

Constraint Index::operator<(size_t rhs) const {
  auto constraint = std::make_shared<ConstraintExpr>(impl_->expr, rhs);
  impl_->constraints.emplace_back(constraint);
  return Constraint();
}

Tensor::Tensor(const std::string& name) : impl_(new Impl) {
  impl_->expr = std::make_shared<ParamExpr>(TensorShape{}, name);
}

Tensor::Tensor(const TensorShape& shape, const std::string& name) : impl_(new Impl) {
  impl_->expr = std::make_shared<ParamExpr>(shape, name);
}

Tensor::Tensor(int value) : impl_(new Impl) { impl_->expr = std::make_shared<IntConst>(value); }

Tensor::Tensor(int64_t value) : impl_(new Impl) { impl_->expr = std::make_shared<IntConst>(value); }

Tensor::Tensor(double value) : impl_(new Impl) { impl_->expr = std::make_shared<FloatConst>(value); }

Tensor::~Tensor() = default;

// Impl Constructor
Tensor::Tensor(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

// Copy Constructor
Tensor::Tensor(const Tensor& rhs) { *this = rhs; }

// Copy Assignment
Tensor& Tensor::operator=(const Tensor& rhs) {
  if (this != &rhs) {
    impl_.reset(new Impl(*rhs.impl_));
  }
  return *this;
}

Access Tensor::operator()(const std::vector<Index>& idxs, const std::vector<size_t>& sizes) {
  if (idxs.size() != sizes.size()) {
    throw std::runtime_error(
        str(boost::format("Dimensions and sizes mismatch in contraction output. Indexes: %1%, Sizes: %2%") %
            idxs.size() % sizes.size()));
  }
  std::vector<std::shared_ptr<PolyExpr>> idx_exprs;
  for (const auto& idx : idxs) {
    idx_exprs.push_back(idx.impl_->expr);
  }
  auto impl = std::make_unique<Access::Impl>();
  impl->src = impl_.get();
  impl->expr = std::make_shared<TensorSpecExpr>(impl_->expr, idx_exprs, sizes);
  return Access{std::move(impl)};
}

Access Tensor::operator()(const std::vector<Index>& idxs) const {
  auto this_shape = shape();
  if (idxs.size() != this_shape.dims.size()) {
    throw std::runtime_error(
        str(boost::format("Unexpected number of dimensions in contraction input. Expected: %1%, Actual: %2%") %
            this_shape.dims.size() % idxs.size()));
  }
  std::vector<std::shared_ptr<PolyExpr>> idx_exprs;
  for (const auto& idx : idxs) {
    idx_exprs.push_back(idx.impl_->expr);
  }
  std::vector<size_t> sizes;
  auto impl = std::make_unique<Access::Impl>();
  impl->src = impl_.get();
  impl->expr = std::make_shared<TensorSpecExpr>(impl_->expr, idx_exprs, sizes);
  return Access{std::move(impl)};
}

size_t Tensor::dims(const size_t dim) const {
  auto this_shape = shape();
  if (this_shape.dims.size() <= dim) {
    throw std::runtime_error("Requested dimension number higher than number of tensor dimensions");
  }
  return this_shape.dims[dim].size;
}

Tensor Tensor::operator-() const { return Call("neg", {*this}); }
Tensor Tensor::operator~() const { return Call("bit_not", {*this}); }
Tensor Tensor::operator+(const Tensor& rhs) const { return Call("add", {*this, rhs}); }
Tensor Tensor::operator-(const Tensor& rhs) const { return Call("sub", {*this, rhs}); }
Tensor Tensor::operator*(const Tensor& rhs) const { return Call("mul", {*this, rhs}); }
Tensor Tensor::operator/(const Tensor& rhs) const { return Call("div", {*this, rhs}); }
Tensor Tensor::operator==(const Tensor& rhs) const { return Call("cmp_eq", {*this, rhs}); }
Tensor Tensor::operator!=(const Tensor& rhs) const { return Call("cmp_ne", {*this, rhs}); }
Tensor Tensor::operator<(const Tensor& rhs) const { return Call("cmp_lt", {*this, rhs}); }
Tensor Tensor::operator>(const Tensor& rhs) const { return Call("cmp_gt", {*this, rhs}); }
Tensor Tensor::operator<=(const Tensor& rhs) const { return Call("cmp_le", {*this, rhs}); }
Tensor Tensor::operator>=(const Tensor& rhs) const { return Call("cmp_ge", {*this, rhs}); }
Tensor Tensor::operator<<(const Tensor& rhs) const { return Call("bit_left", {*this, rhs}); }
Tensor Tensor::operator>>(const Tensor& rhs) const { return Call("bit_right", {*this, rhs}); }
Tensor Tensor::operator&(const Tensor& rhs) const { return Call("bit_and", {*this, rhs}); }
Tensor Tensor::operator|(const Tensor& rhs) const { return Call("bit_or", {*this, rhs}); }
Tensor Tensor::operator^(const Tensor& rhs) const { return Call("bit_xor", {*this, rhs}); }

Tensor& Tensor::no_defract() {
  auto cion_expr = std::dynamic_pointer_cast<ContractionExpr>(impl_->expr);
  if (!cion_expr) {
    throw std::runtime_error("no_defract can only be specified on a contraction");
  }
  cion_expr->no_defract = true;
  return *this;
}

Tensor& Tensor::use_default(const Tensor& rhs) {
  auto cion_expr = std::dynamic_pointer_cast<ContractionExpr>(impl_->expr);
  if (!cion_expr) {
    throw std::runtime_error("use_default can only be specified on a contraction");
  }
  cion_expr->use_default = rhs.impl_->expr;
  return *this;
}

TensorShape Tensor::shape() const { return EvaluateShape(impl_->expr); }

Access::~Access() = default;

Access::Access(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Access::Access(Access&& rhs) noexcept : impl_(std::move(rhs.impl_)) {}

struct ConstraintCollector : public PolyVisitor {
  Polynomial Visit(const PolyIndex& expr) {
    auto impl = static_cast<const Index::Impl*>(expr.ptr);
    std::copy(impl->constraints.begin(), impl->constraints.end(), std::back_inserter(constraints));
    return Polynomial();
  }

  Polynomial Visit(const PolyLiteral& expr) { return Polynomial(); }

  Polynomial Visit(const PolyOp& expr) {
    for (const auto& op : expr.operands) {
      op->Accept(this);
    }
    return Polynomial();
  }

  std::vector<std::shared_ptr<ConstraintExpr>> constraints;
};

void Access::Impl::MakeContraction(AggregationOp agg_op, const Access& rhs) {
  auto output_spec = std::dynamic_pointer_cast<TensorSpecExpr>(expr);
  if (!output_spec) {
    throw std::runtime_error("oops: out_spec");
  }

  auto cion_expr = std::make_shared<ContractionExpr>();
  cion_expr->agg_op = agg_op;
  cion_expr->output = output_spec;

  auto input_spec = std::dynamic_pointer_cast<TensorSpecExpr>(rhs.impl_->expr);
  if (input_spec) {
    cion_expr->inputs = {input_spec};
  } else {
    auto call_expr = std::dynamic_pointer_cast<CallExpr>(rhs.impl_->expr);
    if (!call_expr) {
      throw std::runtime_error("oops: call_expr");
    }
    if (call_expr->fn == "add") {
      cion_expr->combo_op = CombinationOp::PLUS;
    } else if (call_expr->fn == "mul") {
      cion_expr->combo_op = CombinationOp::MULTIPLY;
    } else if (call_expr->fn == "eq") {
      cion_expr->combo_op = CombinationOp::EQ;
    } else if (call_expr->fn == "cond") {
      cion_expr->combo_op = CombinationOp::COND;
    }
    for (const auto& arg : call_expr->args) {
      auto spec = std::dynamic_pointer_cast<TensorSpecExpr>(arg);
      cion_expr->inputs.push_back(spec);
    }
  }

  ConstraintCollector cc;
  for (const auto& idx : output_spec->index_spec) {
    idx->Accept(&cc);
  }

  for (const auto& tensor : cion_expr->inputs) {
    for (const auto& idx : tensor->index_spec) {
      idx->Accept(&cc);
    }
  }
  cion_expr->constraints = cc.constraints;

  // If the lhs has been optionally named, use it
  auto param = std::dynamic_pointer_cast<ParamExpr>(src->expr);
  std::string name;
  if (param) {
    cion_expr->name = param->name;
  }
  src->expr = cion_expr;
}

Access& Access::operator+=(const Access& rhs) {
  impl_->MakeContraction(AggregationOp::SUM, rhs);
  return *this;
}

Access& Access::operator*=(const Access& rhs) {
  impl_->MakeContraction(AggregationOp::PROD, rhs);
  return *this;
}

Access& Access::operator>=(const Access& rhs) {
  impl_->MakeContraction(AggregationOp::MAX, rhs);
  return *this;
}

Access& Access::operator<=(const Access& rhs) {
  impl_->MakeContraction(AggregationOp::MIN, rhs);
  return *this;
}

Access& Access::operator=(const Access& rhs) {
  impl_->MakeContraction(AggregationOp::ASSIGN, rhs);
  return *this;
}

Access Access::Impl::MakeCall(const std::string& fn, const Access& rhs) {
  auto impl = std::make_unique<Impl>();
  std::vector<std::shared_ptr<Expr>> args = {expr, rhs.impl_->expr};
  impl->expr = std::make_shared<CallExpr>(fn, args);
  return Access{std::move(impl)};
}

Access Access::operator+(const Access& rhs) const { return impl_->MakeCall("add", rhs); }
Access Access::operator*(const Access& rhs) const { return impl_->MakeCall("mul", rhs); }
Access Access::operator==(const Access& rhs) const { return impl_->MakeCall("eq", rhs); }

Access cond(const Access& lhs, const Access& rhs, const Access& true_case) {
  auto impl = std::make_unique<Access::Impl>();
  std::vector<std::shared_ptr<Expr>> args = {lhs.impl_->expr, rhs.impl_->expr, true_case.impl_->expr};
  impl->expr = std::make_shared<CallExpr>("cond", args);
  return Access{std::move(impl)};
}

class PolyEvaluator : public PolyVisitor {
 private:
  Polynomial Visit(const PolyIndex& expr) {
    auto it = seen_.find(expr.ptr);
    if (it == seen_.end()) {
      auto name = expr.name;
      if (name.empty()) {
        name = NewIdx();
      }
      std::tie(it, std::ignore) = seen_.emplace(expr.ptr, name);
    }
    return Polynomial(it->second);
  }

  Polynomial Visit(const PolyLiteral& expr) { return Polynomial(expr.value); }

  Polynomial Visit(const PolyOp& expr) {
    if (expr.op == "neg") {
      return -expr.operands[0]->Accept(this);
    }
    if (expr.operands.size() != 2) {
      throw std::runtime_error("Invalid number of operands in PolyOp");
    }
    auto lhs = expr.operands[0]->Accept(this);
    auto rhs = expr.operands[1]->Accept(this);
    if (expr.op == "add") {
      return lhs + rhs;
    }
    if (expr.op == "sub") {
      return lhs - rhs;
    }
    if (expr.op == "mul") {
      if (lhs.isConstant()) {
        return rhs * lhs.constant();
      }
      if (rhs.isConstant()) {
        return lhs * rhs.constant();
      }
      throw std::runtime_error("Non-linear polynomial");
    }
    if (expr.op == "div") {
      if (!rhs.isConstant()) {
        throw std::runtime_error("Divisor of polynomials must be a constant");
      }
      return lhs / rhs.constant();
    }
    throw std::runtime_error("Unknown PolyOp");
  }

 private:
  std::string NewIdx() { return str(boost::format("x%1%") % next_++); }

 private:
  std::unordered_map<const void*, std::string> seen_;
  std::vector<Polynomial> constraints_;
  size_t next_ = 0;
};

class AstTraversal : public AstVisitor {
 public:
  explicit AstTraversal(const std::vector<std::shared_ptr<Expr>>& exprs) {
    for (const auto& expr : exprs) {
      Push(expr);
    }
    while (stack_.size()) {
      auto entry = stack_.top();
      stack_.pop();
      if (entry.second) {
        flat_.push_back(entry.first);
      } else if (!seen_.count(entry.first.get())) {
        seen_.insert(entry.first.get());
        stack_.push(std::make_pair(entry.first, true));
        entry.first->Accept(this);
      }
    }
    IVLOG(4, "AstTraversal: " << StreamContainer(flat_));
  }

  const AstVector& flat() const { return flat_; }

 private:
  void Visit(const CallExpr& expr) {
    // push arguments from right-to-left so they eventually get processed in left-to-right order
    for (auto it = expr.args.rbegin(); it != expr.args.rend(); ++it) {
      Push(*it);
    }
  }

  void Visit(const ConstraintExpr& expr) { throw std::runtime_error("Not implemented"); }

  void Visit(const ContractionExpr& expr) {
    // push inputs from right-to-left so they eventually get processed in left-to-right order
    for (auto it = expr.inputs.rbegin(); it != expr.inputs.rend(); ++it) {
      Push((*it)->ref);
    }
    if (expr.use_default) {
      Push(expr.use_default);
    }
  }

  void Visit(const FloatConst& expr) {}

  void Visit(const IntConst& expr) {}

  void Visit(const ParamExpr& expr) {}

  void Visit(const TensorSpecExpr& expr) { throw std::runtime_error("Not implemented"); }

 private:
  void Push(const std::shared_ptr<Expr>& expr) {
    IVLOG(4, "AstTraversal::Push> " << expr.get());
    stack_.push(std::make_pair(expr, false));
  }

 private:
  std::stack<std::pair<std::shared_ptr<Expr>, bool>> stack_;
  AstVector flat_;
  std::unordered_set<const Expr*> seen_;
};

struct SpecialOp {
  virtual ~SpecialOp() = default;
  virtual TensorShape ComputeShape(const std::vector<Binding>& args) const = 0;
};

class SpecialOpRegistry {
 public:
  static SpecialOpRegistry* Instance() {
    static SpecialOpRegistry registry;
    return &registry;
  }

  void Register(const std::string& name, std::unique_ptr<SpecialOp> op) {  //
    registry_[name] = std::move(op);
  }

  const SpecialOp* Resolve(const std::string& name) {
    auto it = registry_.find(name);
    if (it == registry_.end()) {
      return nullptr;
    }
    return it->second.get();
  }

 private:
  std::unordered_map<std::string, std::unique_ptr<SpecialOp>> registry_;
};

bool MergeShapes(TensorShape* into, const TensorShape& shape) {
  IVLOG(4, "MergeShapes: " << *into << ", " << shape);
  if (shape.dims.size()) {
    if (into->dims.empty()) {
      into->dims = shape.dims;
      return false;
    } else if (into->dims != shape.dims) {
      IVLOG(4, "Checking compatibility between " << into->dims << " and " << shape.dims);
      auto dst = into->dims.rbegin();
      auto src = shape.dims.rbegin();
      for (;; ++dst, ++src) {
        IVLOG(4, "Top of check loop");
        if (src == shape.dims.rend()) {
          IVLOG(4, "src broadcasts to dst");
          break;
        }
        if (dst == into->dims.rend()) {
          // Anything that was used to produce 'into' can be broadcast
          // to 'src'; we just need to augment 'into' with the remaining
          // elements of 'src'.
          into->dims.insert(into->dims.begin(), shape.dims.begin(),
                            shape.dims.begin() + (shape.dims.size() - (src - shape.dims.rbegin())));
          IVLOG(4, "dst broadcasts to src; dims = " << into->dims);
          break;
        }
        IVLOG(4, "Considering " << dst->size << " vs. " << src->size);
        if (src->size == dst->size) {
          IVLOG(4, "No broadcasting needed (here)");
          continue;
        }
        if (src->size == 1) {
          // This dimension of src can be broadcast to whatever is in src.
          IVLOG(4, "dst broadcasts to src");
          continue;
        }
        if (dst->size == 1) {
          // This dimension of dst can be broadcast to whatever is in dst.
          dst->size = src->size;
          IVLOG(4, "src broadcasts to dst");
          continue;
        }
        // Otherwise, broadcasting cannot be done.
        throw std::runtime_error(
            str(boost::format("Mismatched tensor shapes in elementwise operation: %1% can't match %2%")  //
                % StreamContainer(into->dims) % StreamContainer(shape.dims)));
      }
      IVLOG(4, "Broadcast possible; LCM dims=" << into->dims);
      return true;
    }
  }
  return false;
}

DataType ComputeOutputType(const std::vector<TensorShape>& shapes) {
  DataType ret = DataType::INVALID;
  for (const auto& shape : shapes) {
    DataType cur = shape.type;
    if (is_float(cur) != is_float(ret)) {
      if (is_float(cur)) {
        ret = cur;
      }
    } else {
      // TODO: This is a bit primitive; for example, it will pick
      // the first of "int32" or "float32".  We may want to make it
      // a bit more sophisticated.
      if (bit_width(cur) > bit_width(ret)) {
        ret = cur;
      }
    }
  }
  return ret;
}

TensorShape ComputeOutputShape(const std::vector<Binding>& inputs) {
  TensorShape ret;
  bool did_broadcast = false;
  std::vector<TensorShape> shapes;
  for (const auto& input : inputs) {
    TensorShape shape;
    switch (input.tag) {
      case Binding::TENSOR:
        shape = input.shape;
        break;
      case Binding::ICONST:
        shape = TensorShape(DataType::INT32, {});
        break;
      case Binding::FCONST:
        shape = TensorShape(DataType::FLOAT32, {});
        break;
      default:
        throw std::runtime_error("Unknown binding tag");
    }
    did_broadcast = MergeShapes(&ret, shape) || did_broadcast;
    shapes.emplace_back(shape);
  }
  if (did_broadcast) {
    // Recompute strides in dims.
    size_t stride = 1;
    for (auto it = ret.dims.rbegin(); it != ret.dims.rend(); ++it) {
      it->stride = stride;
      stride *= it->size;
    }
  }
  ret.type = ComputeOutputType(shapes);
  return ret;
}

class ShapeEvaluator : AstVisitor {
 public:
  ShapeEvaluator(const AstVector& flat, std::unordered_map<const Expr*, Binding>* bindings)
      : bindings_by_expr_(bindings) {
    for (const auto& expr : flat) {
      expr->Accept(this);
    }
  }

 private:
  void Visit(const ParamExpr& expr) {
    IVLOG(4, "ShapeEvaluator::Visit> " << to_string(&expr));
    bindings_by_expr_->emplace(&expr, Binding{expr.shape});
  }

  void Visit(const CallExpr& expr) {
    IVLOG(4, "ShapeEvaluator::Visit> " << to_string(&expr));
    std::vector<Binding> args;
    for (const auto& arg : expr.args) {
      args.emplace_back(safe_at(bindings_by_expr_, arg.get()));
    }
    auto op = SpecialOpRegistry::Instance()->Resolve(expr.fn);
    if (op) {
      bindings_by_expr_->emplace(&expr, Binding(op->ComputeShape(args)));
    } else {
      bindings_by_expr_->emplace(&expr, Binding(ComputeOutputShape(args)));
    }
  }

  void Visit(const ConstraintExpr&) { throw std::runtime_error("Not implemented"); }

  void Visit(const ContractionExpr& expr) {
    IVLOG(4, "ShapeEvaluator::Visit> " << to_string(&expr));
    DataType type;
    if (expr.combo_op == CombinationOp::COND) {
      type = DataType::BOOLEAN;
    } else {
      std::vector<TensorShape> shapes;
      for (const auto& input : expr.inputs) {
        auto binding = safe_at(bindings_by_expr_, input->ref.get());
        if (binding.tag != Binding::TENSOR) {
          throw std::runtime_error("Unexpected TensorSpecExpr in ContractionExpr.");
        }
        shapes.emplace_back(binding.shape);
      }
      type = ComputeOutputType(shapes);
    }
    bindings_by_expr_->emplace(&expr, Binding(tile::SimpleShape(type, expr.output->output_sizes)));
  }

  void Visit(const FloatConst& expr) {
    IVLOG(4, "ShapeEvaluator::Visit> " << to_string(&expr));
    bindings_by_expr_->emplace(&expr, Binding(expr.value, DataType::FLOAT32));
  }

  void Visit(const IntConst& expr) {
    IVLOG(4, "ShapeEvaluator::Visit> " << to_string(&expr));
    bindings_by_expr_->emplace(&expr, Binding{expr.value});
  }

  void Visit(const TensorSpecExpr& expr) { throw std::runtime_error("Not implemented"); }

 private:
  std::unordered_map<const Expr*, Binding>* bindings_by_expr_;
};

class Evaluator : public AstVisitor {
 public:
  explicit Evaluator(const std::string& name) { runinfo_.program_name = name; }

  RunInfo Evaluate(const std::vector<Tensor>& vars) {
    std::vector<std::shared_ptr<Expr>> exprs;
    for (const auto& var : vars) {
      exprs.push_back(var.impl_->expr);
    }
    AstTraversal traversal(exprs);
    // Traverse the entire graph in least-dependent to most-dependent order.
    ShapeEvaluator evaluator(traversal.flat(), &bindings_by_expr_);
    for (const auto& expr : traversal.flat()) {
      expr->Accept(this);
    }
    for (const auto& expr : exprs) {
      // At this point, it should be gauranteed that the output expressions have been visited.
      auto name = safe_at(&names_by_expr_, expr.get());
      auto shape = safe_at(&bindings_by_expr_, expr.get()).shape;
      IVLOG(2, "Output> " << name << ": " << shape);
      runinfo_.output_shapes.emplace(name, shape);
      runinfo_.program.outputs.push_back(name);
    }
    for (const auto& kvp : names_by_expr_) {
      auto name = kvp.second;
      auto binding = safe_at(&bindings_by_expr_, kvp.first);
      runinfo_.vars.emplace(name, binding);
    }
    runinfo_.code = to_string(runinfo_.program);
    runinfo_.from_edsl = true;
    IVLOG(2, "Evaluator::Evaluate> " << runinfo_.code);
    return runinfo_;
  }

 private:
  void Visit(const ParamExpr& expr) {
    IVLOG(4, "Evaluator::Visit> " << to_string(&expr));
    auto name = NewTmp(expr);
    Input input{Input::FIXED, name};
    for (size_t i = 0; i < expr.shape.dims.size(); i++) {
      auto dim_name = str(boost::format("%1%_%2%") % name % i);
      input.dims.emplace_back(dim_name);
    }
    runinfo_.program.inputs.push_back(input);
    runinfo_.input_shapes.emplace(name, expr.shape);
    names_by_expr_.emplace(&expr, name);
  }

  void Visit(const FloatConst& expr) {
    IVLOG(4, "Evaluator::Visit> " << to_string(&expr));
    auto name = NewTmp(expr);
    Op op{
        Op::CONSTANT,                  // tag
        name,                          // output
        {std::to_string(expr.value)},  // inputs
        {},                            // Contraction
        {"fconst"},                    // Function
    };
    runinfo_.program.ops.emplace_back(op);
    names_by_expr_.emplace(&expr, name);
  }

  void Visit(const IntConst& expr) {
    IVLOG(4, "Evaluator::Visit> " << to_string(&expr));
    auto name = NewTmp(expr);
    Op op{
        Op::CONSTANT,                  // tag
        name,                          // output
        {std::to_string(expr.value)},  // inputs
        {},                            // Contraction
        {"iconst"},                    // Function
    };
    runinfo_.program.ops.emplace_back(op);
    names_by_expr_.emplace(&expr, name);
  }

  void Visit(const CallExpr& expr) {
    IVLOG(4, "Evaluator::Visit> " << to_string(&expr));
    std::vector<std::string> args;
    for (const auto& arg : expr.args) {
      args.emplace_back(safe_at(&names_by_expr_, arg.get()));
    }
    auto name = NewTmp(expr);
    Op op{
        Op::FUNCTION,  // tag
        name,          // output
        args,          // inputs
        {},            // Contraction
        {expr.fn},     // Function
    };
    runinfo_.program.ops.emplace_back(op);
    names_by_expr_.emplace(&expr, name);
  }

  void Visit(const ConstraintExpr& expr) { throw std::runtime_error("Not implemented"); }

  void Visit(const ContractionExpr& expr) {
    IVLOG(4, "Evaluator::Visit> " << to_string(&expr));
    PolyEvaluator poly_eval;
    Contraction cion;
    cion.agg_op = expr.agg_op;
    cion.comb_op = expr.combo_op;
    cion.no_defract = expr.no_defract;
    if (expr.use_default) {
      cion.use_default = safe_at(&names_by_expr_, expr.use_default.get());
    }
    cion.specs.emplace_back(TensorSpec{});
    std::vector<std::string> inputs;
    for (const auto& input : expr.inputs) {
      TensorSpec tensor_spec;
      tensor_spec.id = safe_at(&names_by_expr_, input->ref.get());
      inputs.push_back(tensor_spec.id);
      for (const auto& idx : input->index_spec) {
        auto poly = idx->Accept(&poly_eval);
        tensor_spec.spec.push_back(poly);
      }
      cion.specs.emplace_back(tensor_spec);
    }
    auto name = NewTmp(expr);
    cion.specs[0].id = name;
    for (const auto& idx : expr.output->index_spec) {
      auto poly = idx->Accept(&poly_eval);
      cion.specs[0].spec.push_back(poly);
    }
    for (const auto& size : expr.output->output_sizes) {
      cion.output_size.push_back(std::to_string(size));
    }
    for (const auto& constraint : expr.constraints) {
      auto poly = constraint->lhs->Accept(&poly_eval);
      auto range = constraint->rhs;
      tile::math::RangeConstraint bound(poly, range);
      cion.constraints.emplace_back(bound);
    }
    Op op{
        Op::CONTRACTION,  // tag
        name,             // output
        inputs,           // inputs
        cion,             // Contraction
    };
    runinfo_.program.ops.emplace_back(op);
    names_by_expr_.emplace(&expr, name);
  }

  void Visit(const TensorSpecExpr& expr) { throw std::runtime_error("Not implemented"); }

 private:
  // The current algorithm works by making all unnamed nodes automatically
  // generated so that they are unique but only if names that begin with
  // underscore ("_") are reserved by the system.
  std::string NewTmp(const Expr& expr) {
    if (expr.name.empty()) {
      return str(boost::format("_X%1%") % runinfo_.program.next_tmp++);
    }
    return MakeUniqueName(expr.name);
  }

  std::string MakeUniqueName(const std::string& prefix) {
    bool is_new;
    auto name = prefix;
    std::tie(std::ignore, is_new) = names_.insert(name);
    for (size_t i = 0; !is_new; i++) {
      name = str(boost::format("%1%%2%") % prefix % i);
      std::tie(std::ignore, is_new) = names_.insert(name);
    }
    return name;
  }

 private:
  std::set<std::string> names_;
  std::unordered_map<const Expr*, std::string> names_by_expr_;
  std::unordered_map<const Expr*, Binding> bindings_by_expr_;
  RunInfo runinfo_;
};

TensorShape EvaluateShape(const std::shared_ptr<Expr>& expr) {
  AstTraversal traversal({expr});
  std::unordered_map<const Expr*, Binding> bindings;
  ShapeEvaluator evaluator(traversal.flat(), &bindings);
  return safe_at(&bindings, expr.get()).shape;
}

Tensor Call(const std::string& fn, const std::vector<Tensor>& args) {
  auto impl = std::make_unique<Tensor::Impl>();
  std::vector<std::shared_ptr<Expr>> exprs;
  for (const auto& tensor : args) {
    exprs.push_back(tensor.impl_->expr);
  }
  impl->expr = std::make_shared<CallExpr>(fn, exprs);
  return Tensor{std::move(impl)};
}

RunInfo Evaluate(const std::string& name, const std::vector<Tensor>& vars) { return Evaluator(name).Evaluate(vars); }

struct ReshapeOp : SpecialOp {
  TensorShape ComputeShape(const std::vector<Binding>& args) const {
    if (args.size() < 1) {
      throw std::runtime_error("'reshape' requires at least one argument.");
    }
    if (args[0].tag != Binding::TENSOR) {
      throw std::runtime_error("'reshape' requires the first argument to be a tensor.");
    }
    std::vector<size_t> sizes;
    for (size_t i = 1; i < args.size(); i++) {
      if (args[i].tag != Binding::ICONST) {
        throw std::runtime_error("Additional parameters to 'reshape' must be integers.");
      }
      sizes.push_back(args[i].iconst);
    }
    return tile::SimpleShape(args[0].shape.type, sizes);
  }
};

struct BooleanOp : SpecialOp {
  TensorShape ComputeShape(const std::vector<Binding>& args) const {
    auto ret = ComputeOutputShape(args);
    ret.type = DataType::BOOLEAN;
    return ret;
  }
};

struct FloatCastOp : SpecialOp {
  TensorShape ComputeShape(const std::vector<Binding>& args) const {
    if (args.size() != 2) {
      throw std::runtime_error("'as_float' requires 2 arguments.");
    }
    if (args[0].tag != Binding::TENSOR) {
      throw std::runtime_error("'as_float' requires the first argument to be a tensor.");
    }
    if (args[1].tag != Binding::ICONST) {
      throw std::runtime_error("'as_float' requires the second argument to be a integer.");
    }
    TensorShape ret = args[0].shape;
    switch (args[1].iconst) {
      case 16:
        ret.type = DataType::FLOAT16;
        break;
      case 32:
        ret.type = DataType::FLOAT32;
        break;
      case 64:
        ret.type = DataType::FLOAT64;
        break;
      default:
        throw std::runtime_error("'as_float' requires the width to be one of: (16, 32, 64)");
    }
    return ret;
  }
};

struct IntCastOp : SpecialOp {
  TensorShape ComputeShape(const std::vector<Binding>& args) const {
    if (args.size() != 2) {
      throw std::runtime_error("'as_int' requires 2 arguments.");
    }
    if (args[0].tag != Binding::TENSOR) {
      throw std::runtime_error("'as_int' requires the first argument to be a tensor.");
    }
    if (args[1].tag != Binding::ICONST) {
      throw std::runtime_error("'as_int' requires the second argument to be a integer.");
    }
    TensorShape ret = args[0].shape;
    switch (args[1].iconst) {
      case 16:
        ret.type = DataType::INT16;
        break;
      case 32:
        ret.type = DataType::INT32;
        break;
      case 64:
        ret.type = DataType::INT64;
        break;
      default:
        throw std::runtime_error("'as_int' requires the width to be one of: (16, 32, 64)");
    }
    return ret;
  }
};

struct UintCastOp : SpecialOp {
  TensorShape ComputeShape(const std::vector<Binding>& args) const {
    if (args.size() != 2) {
      throw std::runtime_error("'as_uint' requires 2 arguments.");
    }
    if (args[0].tag != Binding::TENSOR) {
      throw std::runtime_error("'as_uint' requires the first argument to be a tensor.");
    }
    if (args[1].tag != Binding::ICONST) {
      throw std::runtime_error("'as_uint' requires the second argument to be a integer.");
    }
    TensorShape ret = args[0].shape;
    switch (args[1].iconst) {
      case 16:
        ret.type = DataType::UINT16;
        break;
      case 32:
        ret.type = DataType::UINT32;
        break;
      case 64:
        ret.type = DataType::UINT64;
        break;
      default:
        throw std::runtime_error("'as_uint' requires the width to be one of: (16, 32, 64)");
    }
    return ret;
  }
};

struct IndexOp : SpecialOp {
  TensorShape ComputeShape(const std::vector<Binding>& args) const {
    if (args.size() != 2) {
      throw std::runtime_error("'index' requires 2 arguments.");
    }
    if (args[0].tag != Binding::TENSOR) {
      throw std::runtime_error("'index' requires the first argument to be a tensor.");
    }
    if (args[1].tag != Binding::ICONST) {
      throw std::runtime_error("'index' requires the second argument to be an integer.");
    }
    TensorShape ret = args[0].shape;
    ret.type = DataType::INT32;
    return ret;
  }
};

struct ElementOp : SpecialOp {
  TensorShape ComputeShape(const std::vector<Binding>& args) const {
    if (args.size() != 2) {
      throw std::runtime_error("'element' requires 2 arguments.");
    }
    if (args[0].tag != Binding::TUPLE) {
      throw std::runtime_error("'element' requires the first argument to be a tuple.");
    }
    if (args[1].tag != Binding::ICONST) {
      throw std::runtime_error("'element' requires the second arguement to be an integer.");
    }
    auto elt = args[1].iconst;
    if (elt < 0 || elt >= static_cast<int64_t>(args[0].tuple.size())) {
      throw std::runtime_error(
          "'element' requires the second argument to be within the bounds of the specified tuple.");
    }
    if (args[0].tuple[elt].tag != Binding::TENSOR) {
      throw std::runtime_error("'element' requires the resulting binding to be a tensor.");
    }
    return args[0].tuple[elt].shape;
  }
};

struct GatherOp : SpecialOp {
  TensorShape ComputeShape(const std::vector<Binding>& args) const {
    if (args.size() != 2) {
      throw std::runtime_error("'gather' requires 2 arguments.");
    }
    auto data = args[0];
    auto index = args[1];
    if (data.tag != Binding::TENSOR || index.tag != Binding::TENSOR) {
      throw std::runtime_error("'gather' requires both arguments to be tensors.");
    }
    if (data.shape.dims.empty()) {
      throw std::runtime_error("'gather' requires first argument to have at least one dimension.");
    }
    if (index.shape.type != DataType::INT32) {
      // TODO: Handle other integer types?  Floor floats?
      throw std::runtime_error("'gather' requires the data type for the second argument to be INT32.");
    }
    std::vector<size_t> dims;
    for (size_t i = 0; i < index.shape.dims.size(); i++) {
      dims.push_back(index.shape.dims[i].size);
    }
    for (size_t i = 1; i < data.shape.dims.size(); i++) {
      dims.push_back(data.shape.dims[i].size);
    }
    return tile::SimpleShape(data.shape.type, dims);
  }
};

struct ScatterOp : SpecialOp {
  TensorShape ComputeShape(const std::vector<Binding>& args) const {
    if (args.size() != 3) {
      throw std::runtime_error("'scatter' requires 3 arguments.");
    }
    if (args[0].tag != Binding::TENSOR || args[1].tag != Binding::TENSOR || args[2].tag != Binding::TENSOR) {
      throw std::runtime_error("'scatter' requires all arguments to be tensors.");
    }
    if (args[0].shape.dims.empty()) {
      throw std::runtime_error("'scatter' requires first argument to have at least one dimension.");
    }
    if (args[1].shape.type != DataType::INT32) {
      // TODO: Handle other integer types?  Floor floats?
      throw std::runtime_error("'scatter' requires the data type for the second argument to be INT32.");
    }
    std::vector<size_t> dims = {args[2].shape.dims[0].size};
    for (size_t i = args[1].shape.dims.size(); i < args[0].shape.dims.size(); i++) {
      dims.push_back(args[0].shape.dims[i].size);
    }
    return tile::SimpleShape(args[0].shape.type, dims);
  }
};

struct ShapeOp : SpecialOp {
  TensorShape ComputeShape(const std::vector<Binding>& args) const {
    if (args.size() != 1) {
      throw std::runtime_error("'shape' requires exactly one argument.");
    }
    if (args[0].tag != Binding::TENSOR) {
      throw std::runtime_error("'shape' requires one argument that is a tensor.");
    }
    return tile::SimpleShape(DataType::INT32, {args[0].shape.dims.size()});
  }
};

struct PrngStateOp : SpecialOp {
  TensorShape ComputeShape(const std::vector<Binding>& args) const {
    if (args.size() != 1) {
      throw std::runtime_error("'prng_state' requires exactly one argument.");
    }
    if (args[0].tag != Binding::TENSOR) {
      throw std::runtime_error("'prng_state' requires one argument that is a tensor.");
    }
    auto shape = args[0].shape;
    if (shape.type != DataType::PRNG) {
      throw std::runtime_error("'prng_state' requires one argument that is the result of 'prng_step'");
    }
    return tile::SimpleShape(DataType::UINT32, {3, k_rng_size});
  }
};

struct PrngValueOp : SpecialOp {
  TensorShape ComputeShape(const std::vector<Binding>& args) const {
    if (args.size() != 1) {
      throw std::runtime_error("'prng_value' requires exactly one argument.");
    }
    if (args[0].tag != Binding::TENSOR) {
      throw std::runtime_error("'prng_value' requires one argument that is a tensor.");
    }
    auto shape = args[0].shape;
    if (shape.type != DataType::PRNG) {
      throw std::runtime_error("'prng_value' requires one argument that is the result of 'prng_step'");
    }
    return TensorShape(DataType::FLOAT32, shape.dims);
  }
};

struct PrngStepOp : SpecialOp {
  TensorShape ComputeShape(const std::vector<Binding>& args) const {
    if (args.size() < 1) {
      throw std::runtime_error("'prng_step' must have at least one argument.");
    }
    if (args[0].tag != Binding::TENSOR) {
      throw std::runtime_error("'prng_step' requires first argument to be a tensor.");
    }
    // Valididate PRNG state size
    auto shape = args[0].shape;
    if (!(shape == tile::SimpleShape(DataType::UINT32, {3, k_rng_size}))) {
      throw std::runtime_error("'prng_step' requires a valid PRNG state tensor.");
    }
    // Get the output shape sizes
    std::vector<size_t> dims;
    for (size_t i = 1; i < args.size(); i++) {
      if (args[i].tag != Binding::ICONST) {
        throw std::runtime_error("'prng_step' requires additional arguments to be integers.");
      }
      dims.push_back(args[i].iconst);
    }
    return tile::SimpleShape(DataType::PRNG, dims);
  }
};

[[gnu::unused]] auto init = []() {
  auto registry = SpecialOpRegistry::Instance();
  registry->Register("as_float", std::make_unique<FloatCastOp>());
  registry->Register("as_int", std::make_unique<IntCastOp>());
  registry->Register("as_uint", std::make_unique<UintCastOp>());
  registry->Register("cmp_eq", std::make_unique<BooleanOp>());
  registry->Register("cmp_ge", std::make_unique<BooleanOp>());
  registry->Register("cmp_gt", std::make_unique<BooleanOp>());
  registry->Register("cmp_le", std::make_unique<BooleanOp>());
  registry->Register("cmp_lt", std::make_unique<BooleanOp>());
  registry->Register("cmp_ne", std::make_unique<BooleanOp>());
  registry->Register("element", std::make_unique<ElementOp>());
  registry->Register("gather", std::make_unique<GatherOp>());
  registry->Register("index", std::make_unique<IndexOp>());
  registry->Register("prng_state", std::make_unique<PrngStateOp>());
  registry->Register("prng_step", std::make_unique<PrngStepOp>());
  registry->Register("prng_value", std::make_unique<PrngValueOp>());
  registry->Register("reshape", std::make_unique<ReshapeOp>());
  registry->Register("scatter", std::make_unique<ScatterOp>());
  registry->Register("shape", std::make_unique<ShapeOp>());
  return 0;
}();

}  // namespace lang
}  // namespace tile
}  // namespace vertexai
