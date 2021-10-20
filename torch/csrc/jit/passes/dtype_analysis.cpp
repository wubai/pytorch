#include <ATen/core/function_schema.h>
#include <ATen/core/interned_strings.h>
#include <ATen/core/jit_type.h>
#include <c10/core/ScalarType.h>
#include <c10/util/ArrayRef.h>
#include <c10/util/Optional.h>
#include <torch/csrc/jit/ir/alias_analysis.h>
#include <torch/csrc/jit/ir/ir.h>
#include <torch/csrc/jit/jit_log.h>
#include <torch/csrc/jit/passes/dtype_analysis.h>
#include <torch/csrc/jit/passes/utils/op_registry.h>
#include <torch/library.h>
#include <algorithm>
#include <memory>
#include <stdexcept>

namespace torch {
namespace jit {

namespace {

using Tensor = at::Tensor;
using ScalarType = at::ScalarType;

// ----------------------------------------------------------------------------------
// Metatensor Inference for Dtype
// ----------------------------------------------------------------------------------

std::unique_ptr<Stack> MTensorArgumentCreator(Node* n) {
  auto stack = std::make_unique<std::vector<IValue>>();
  for (Value* inp : n->inputs()) {
    if (auto tp = inp->type()->cast<TensorType>()) {
      // Zero-dim tensors have special type promotion behavoir, hence the need
      // for rank.
      auto rank = tp->symbolic_sizes().rank(); // Validity checked earlier
      auto tensor_size = std::vector<int64_t>(rank.value(), 1);
      stack->emplace_back(at::empty(
          tensor_size, at::TensorOptions(at::kMeta).dtype(*tp->scalarType())));
      continue;
    }
    // Someday Todo: Fill in concrete values that we know.
    if (inp->type() == FloatType::get()) {
      stack->emplace_back(1.);
    } else if (inp->type() == IntType::get()) {
      stack->emplace_back(1);
    } else if (inp->type() == BoolType::get()) {
      // For now just not support bool until we know it is safe.
      throw std::runtime_error("Unsupported input type for Tensor argument");
      stack->emplace_back(false);
    } else {
      // Arrays of values are specifically not handled due
      // to the fact that naive default vaules would likely be
      // incorrect anyways.
      throw std::runtime_error("Unsupported input type for Tensor argument");
    }
  }
  return stack;
};

bool MTensorNodeArgValid(Value* value) {
  auto tensor_type = value->type()->cast<TensorType>();
  if (!tensor_type) {
    return true;
  }
  if (!tensor_type->scalarType().has_value()) {
    GRAPH_DEBUG("Argument missing Dtype");
    return false;
  }
  auto rank = tensor_type->symbolic_sizes().rank();
  return rank.has_value();
}

static bool canBeInferredWithMetaTensor(Node* n) {
  // Not a guarantee that the metatensor will not error out
  // Do not have a allowlist for now and let things error out in execution.
  // Has Tensor output is checked in another place
  bool args_valid =
      std::all_of(n->inputs().begin(), n->inputs().end(), MTensorNodeArgValid);

  if (!args_valid) {
    return false;
  }
  if (n->outputs().size() != 1) {
    // Currently not supporting multiple outputs
    return false;
  }
  auto opt_op = n->maybeOperator();
  if (!opt_op) {
    GRAPH_DEBUG("not registered with Meta");
    return false;
  }
  return true;
}

c10::optional<Tensor> inferWithMetaTensor(Node* n) {
  GRAPH_DEBUG("inferWithMetaTensor", getHeader(n));
  if (!canBeInferredWithMetaTensor(n)) {
    return c10::nullopt;
  }
  Operation op = n->getOperation();
  try {
    auto stack = MTensorArgumentCreator(n);
    GRAPH_DEBUG("Running op for ", getHeader(n));
    op(*stack);
    GRAPH_DEBUG("op run successfully", getHeader(n));
    GRAPH_DEBUG("After receive!");
    return stack->back().toTensor();

  } catch (...) {
    GRAPH_DEBUG("caught exception with Metatensor run!");
  };
  return c10::nullopt;
}

bool setDtype(Value* value, ScalarType scalarType) {
  // returns if anything was changed
  auto tensor_type = value->type()->cast<TensorType>();
  TORCH_INTERNAL_ASSERT(tensor_type, "Expecting a tensor type");
  if (!tensor_type->scalarType().has_value()) {
    value->setType(tensor_type->withScalarType(scalarType));
    return true;
  }
  if (tensor_type->scalarType().value() != scalarType) {
    // Have to overwrite the dtype if it has been set to a diffferent value
    // in a previous run of dtype analysis (eg in testing with cached graphs)

    // Due to SSA, there shouldn't be a reason why this overwrite is not valid.
    value->setType(tensor_type->withScalarType(scalarType));
    return true;
  }
  return false;
}

bool tryApplyDtypeMetaTensor(Node* n) {
  // returns if anything was changed
  auto return_tensor = inferWithMetaTensor(n);
  if (!return_tensor) {
    return false;
  }
  GRAPH_DEBUG("Received ", toString(return_tensor->scalar_type()));
  return setDtype(n->output(), return_tensor->scalar_type());
}

// ----------------------------------------------------------------------------------
// Custom Rules for Dtype
// ----------------------------------------------------------------------------------
using DtypePropRule = std::function<bool(Node*)>;
// Function to propagate dtype information for a node
// Returns true if the dtype information was changed

bool setDtypeToFirstArg(Node* n) {
  // Sets all tesnsor outputs to the dtype of the first input
  auto first_arg = n->inputs().at(0);
  auto tensor_type = first_arg->type()->cast<TensorType>();
  TORCH_INTERNAL_ASSERT(tensor_type, "Expecting a tensor type");
  auto scalar_type = tensor_type->scalarType();
  if (!scalar_type.has_value()) {
    return false;
  }

  bool changed = false;
  for (auto output : n->outputs()) {
    if (output->type()->cast<TensorType>()) {
      changed |= setDtype(output, scalar_type.value());
    }
  }
  return changed;
}

// DtypePropagationPass is an analysis pass that walks through a graph in
// topological order and forward propagate Dtypes (ScalarTypes) from graph
// inputs (expressed in input_descriptors) to all output tensor nodes in the
// graph.
struct DtypePropagationPass {
  DtypePropagationPass(std::shared_ptr<Graph> graph)
      : graph_(std::move(graph)) {
    buildDtypeRuleRegistry();
  }

  // returns true if at least one node has its scalar type set on a tensor node
  bool run() {
    return processBlocks(graph_->block());
  }

 private:
  bool processBlocks(at::ArrayRef<Block*> blocks) {
    bool changed = false;
    for (auto block : blocks) {
      changed |= processBlock(block);
    }
    return changed;
  }

  bool processBlock(Block* block) {
    GRAPH_DEBUG("processBlock");
    bool changed = false;
    for (auto it = block->nodes().begin(); it != block->nodes().end(); it++) {
      changed |= processNode(*it);
    }
    return changed;
  }

  bool processNode(Node* n) {
    GRAPH_DEBUG("processNode");
    switch (n->kind()) {
      case prim::If:
        return processIf(n);
      case prim::Loop:
      case prim::CallMethod:
      case prim::CallFunction:
        TORCH_INTERNAL_ASSERT(false, "Loop/Call not handled now");
      default:
        break;
    }

    TORCH_INTERNAL_ASSERT(
        n->blocks().empty(), "Do not handle this block structure");

    // process non-block nodes
    bool has_tensor_output =
        std::any_of(n->outputs().begin(), n->outputs().end(), [](Value* v) {
          return (bool)v->type()->cast<TensorType>();
        });

    if (!has_tensor_output) {
      // if output contains no tensor, nothing to propagate
      return false;
    }

    bool changed = false;
    // Main dispatch loop for the abstract interpreter
    switch (n->kind()) {
      case prim::Constant:
        // This seems to already been propagated by something else in freezing
        return false;
      case prim::ListConstruct:
      case prim::ListUnpack:
        TORCH_INTERNAL_ASSERT(false, "not supported IR");
        break;
      default:
        if (n->kind().is_aten()) {
          changed = processAtenOps(n);
        } else {
          TORCH_INTERNAL_ASSERT(false, "not supported IR");
        }
    }
    return changed;
  }

  bool mergeTensorProperties(
      const at::ArrayRef<Value*>& list1,
      const at::ArrayRef<Value*>& list2) {
    // This is currently a placeholder for MobileNet
    // After Month1: implement the merge function
    TORCH_INTERNAL_ASSERT(list1.size() == 0, "Not implemented yet");
    return false;
    /*
    TORCH_INTERNAL_ASSERT(oneList.size() == anotherList.size());

    for (auto i : c10::irange(oneList.size())) {
    }
    return false;
    */
  }

  bool processIf(Node* node) {
    GRAPH_DEBUG("processIf");
    bool changed = false;
    auto blocks = node->blocks();
    auto true_block = blocks.at(0);
    auto false_block = blocks.at(1);

    changed |= processBlock(true_block);
    changed |= processBlock(false_block);

    changed |=
        mergeTensorProperties(true_block->outputs(), false_block->outputs());

    return changed;
  }

  // for efficiency
  bool processAtenOps(Node* n) {
    GRAPH_DEBUG("processAtenOps");
    GRAPH_DEBUG("case = ", n->kind(), " ", *n);

    // Custom Rule Matching
    if (auto prop_fn = dtype_prop_registry_->find(n->getOperator())) {
      DtypePropRule rule = *prop_fn;
      return rule(n);
    }
    GRAPH_DEBUG("case = ", n->kind(), " ", *n);

    // Custom Rule Matching
    if (auto prop_fn = dtype_prop_registry_->find(n->getOperator())) {
      DtypePropRule rule = *prop_fn;
      return rule(n);
    }

    bool changed = tryApplyDtypeMetaTensor(n);

    switch (n->kind()) {
      case aten::append:
        // auto elementType =
        // n->input()->type()->expect<ListType>()->containedTypes()[0]; if
        // (auto itp  = elementType->cast<TensorType>()) {
        //   //itp->
        //   auto otp = elementType->cast<TensorType>()->containedTypes()[0];
        //   ListType::create(otp->withScalarType(i))
        // }
        break;
    }

    return changed;
  }

  void buildDtypeRuleRegistry() {
    // building a registry for all of the custom dtype rules
    dtype_prop_registry_ = std::make_unique<OperatorMap<DtypePropRule>>();

    dtype_prop_registry_->insert(
        *nn_ops_first_input_preserving(), setDtypeToFirstArg);
    dtype_prop_registry_->insert(
        *ops_one_tensor_in_shape_transform(), setDtypeToFirstArg);
  }
  /*
  AliasDb* getOrCreateAliasDb() {
    if (!aliasDb_) {
      aliasDb_ = std::make_unique<AliasDb>(graph_);
    }
    return aliasDb_.get();
  }
  */

  /*
  // This one is a special rule -- mean take the ScalarType if specified,
  otherwise Tensor type
  // This is an example where the dtype rule has to be extracted from the
  implementation
  "aten::mean(Tensor self, *, ScalarType? dtype) -> Tensor",
  */
  std::unique_ptr<OperatorMap<DtypePropRule>> dtype_prop_registry_;
  std::shared_ptr<Graph> graph_;
  // std::unique_ptr<AliasDb> aliasDb_ = nullptr;
};

} // anonymous namespace

// This analysis propagates input dtypes (if any) throughout the
// graph.
bool DtypePropagation(std::shared_ptr<Graph>& graph) {
  DtypePropagationPass tp = DtypePropagationPass(graph);
  bool changed = tp.run();
  if (changed) {
    GRAPH_DUMP("After TensorPropertyPropagation pass:", graph);
  }
  return changed;
}

} // namespace jit
} // namespace torch
