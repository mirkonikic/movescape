#include "movescape/borrow_analysis.hpp"

#include "movescape/disassembler.hpp"
#include "movescape/error.hpp"
#include "movescape/type_analysis.hpp"

#include <algorithm>
#include <compare>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace movescape {

namespace {

using RefId = std::size_t;

enum class LabelKind { Local, Global, Field };

struct Label {
  LabelKind kind = LabelKind::Local;
  std::size_t index = 0;

  auto operator<=>(const Label &) const = default;
};

using Path = std::vector<Label>;

struct Edge {
  RefId parent = 0;
  RefId child = 0;
  bool strong = false;
  Path path;

  auto operator<=>(const Edge &) const = default;
};

struct Reference {
  bool mutable_reference = false;

  friend bool operator==(const Reference &, const Reference &) = default;
};

[[noreturn]] void borrowError(const StacklessInstruction &instruction, std::string message) {
  std::ostringstream out;
  out << "instruction " << instruction.bytecode_index << " (" << opcodeInfo(instruction.opcode).name << "): " << message;
  throw Error(ErrorCode::InvalidBorrowState, Error::UnknownOffset, out.str());
}

[[noreturn]] void analysisError(std::string message) { throw Error(ErrorCode::InvalidBorrowState, Error::UnknownOffset, std::move(message)); }

[[nodiscard]] bool isReference(const Type &type) noexcept { return type.kind == TypeKind::Reference || type.kind == TypeKind::MutableReference; }

[[nodiscard]] bool isMutableReference(const Type &type) noexcept { return type.kind == TypeKind::MutableReference; }

[[nodiscard]] bool pathPrefix(const Path &prefix, const Path &path) {
  return prefix.size() <= path.size() && std::equal(prefix.begin(), prefix.end(), path.begin());
}

[[nodiscard]] bool edgeCovers(const Edge &cover, const Edge &candidate) {
  if (cover.parent != candidate.parent || cover.child != candidate.child) {
    return false;
  }
  return cover == candidate || (!cover.strong && pathPrefix(cover.path, candidate.path));
}

class BorrowGraph {
public:
  void newReference(RefId id, bool mutable_reference) {
    if (!references_.emplace(id, Reference{.mutable_reference = mutable_reference}).second) {
      analysisError("borrow analysis reused a reference identifier");
    }
  }

  [[nodiscard]] bool contains(RefId id) const { return references_.contains(id); }

  [[nodiscard]] bool isMutable(RefId id) const { return references_.at(id).mutable_reference; }

  void addStrongBorrow(RefId parent, RefId child) { factor(parent, {}, child); }

  void addStrongFieldBorrow(RefId parent, Label field, RefId child) { factor(parent, {field}, child); }

  void addWeakBorrow(RefId parent, RefId child) { addEdge({.parent = parent, .child = child, .strong = false, .path = {}}); }

  void addWeakFieldBorrow(RefId parent, Label field, RefId child) { addEdge({.parent = parent, .child = child, .strong = false, .path = {field}}); }

  void release(RefId id) {
    if (!contains(id)) {
      analysisError("borrow analysis released an unknown reference");
    }
    std::vector<Edge> incoming;
    std::vector<Edge> outgoing;
    for (const auto &edge : edges_) {
      if (edge.child == id) {
        incoming.push_back(edge);
      }
      if (edge.parent == id) {
        outgoing.push_back(edge);
      }
    }
    std::erase_if(edges_, [id](const Edge &edge) { return edge.parent == id || edge.child == id; });
    references_.erase(id);

    for (const auto &parent_edge : incoming) {
      for (const auto &child_edge : outgoing) {
        if (parent_edge.parent == child_edge.child) {
          continue;
        }
        Path path = parent_edge.path;
        if (parent_edge.strong) {
          path.insert(path.end(), child_edge.path.begin(), child_edge.path.end());
        }
        addEdge({.parent = parent_edge.parent, .child = child_edge.child, .strong = parent_edge.strong && child_edge.strong, .path = std::move(path)});
      }
    }
  }

  [[nodiscard]] bool hasFullBorrows(RefId id) const {
    return std::any_of(edges_.begin(), edges_.end(), [id](const Edge &edge) { return edge.parent == id && edge.path.empty(); });
  }

  [[nodiscard]] bool hasConsistentBorrows(RefId id, std::optional<Label> label) const {
    return std::any_of(edges_.begin(), edges_.end(), [id, label](const Edge &edge) {
      if (edge.parent != id) {
        return false;
      }
      if (edge.path.empty() || !label.has_value()) {
        return true;
      }
      return edge.path.front() == *label;
    });
  }

  [[nodiscard]] bool hasConsistentMutableBorrows(RefId id, std::optional<Label> label) const {
    return std::any_of(edges_.begin(), edges_.end(), [&](const Edge &edge) {
      if (edge.parent != id || !isMutable(edge.child)) {
        return false;
      }
      if (edge.path.empty() || !label.has_value()) {
        return true;
      }
      return edge.path.front() == *label;
    });
  }

  [[nodiscard]] bool writable(RefId id) const { return isMutable(id) && !hasConsistentBorrows(id, std::nullopt); }

  [[nodiscard]] bool freezable(RefId id, std::optional<Label> field) const { return isMutable(id) && !hasConsistentMutableBorrows(id, field); }

  [[nodiscard]] bool readable(RefId id, std::optional<Label> field) const { return !isMutable(id) || freezable(id, field); }

  [[nodiscard]] BorrowGraph remap(const std::map<RefId, RefId> &ids) const {
    BorrowGraph result;
    for (const auto &[old_id, reference] : references_) {
      const auto found = ids.find(old_id);
      if (found == ids.end()) {
        analysisError("live reference is not held by a local at block exit");
      }
      result.newReference(found->second, reference.mutable_reference);
    }
    for (const auto &edge : edges_) {
      result.addEdge({.parent = ids.at(edge.parent), .child = ids.at(edge.child), .strong = edge.strong, .path = edge.path});
    }
    return result;
  }

  [[nodiscard]] static BorrowGraph join(const BorrowGraph &left, const BorrowGraph &right) {
    if (left.references_ != right.references_) {
      analysisError("borrow graph nodes disagree at control-flow join");
    }
    BorrowGraph result = left;
    for (const auto &edge : right.edges_) {
      result.addEdge(edge);
    }
    return result;
  }

  friend bool operator==(const BorrowGraph &, const BorrowGraph &) = default;

private:
  static constexpr std::size_t MaxEdgesPerPair = 10;
  static constexpr std::size_t MaxPathLength = 32;

  void addEdge(Edge edge) {
    if (edge.parent == edge.child) {
      return;
    }
    if (!contains(edge.parent) || !contains(edge.child)) {
      analysisError("borrow edge references an unknown node");
    }
    if (edge.path.size() > MaxPathLength) {
      edge.strong = false;
      edge.path.clear();
    }
    for (const auto &existing : edges_) {
      if (edgeCovers(existing, edge)) {
        return;
      }
    }
    std::erase_if(edges_, [&](const Edge &existing) { return edgeCovers(edge, existing); });
    const auto parent = edge.parent;
    const auto child = edge.child;
    edges_.insert(std::move(edge));

    std::size_t count = 0;
    for (const auto &existing : edges_) {
      if (existing.parent == parent && existing.child == child) {
        ++count;
      }
    }
    if (count <= MaxEdgesPerPair) {
      return;
    }
    std::erase_if(edges_, [&](const Edge &existing) { return existing.parent == parent && existing.child == child; });
    edges_.insert({.parent = parent, .child = child, .strong = false, .path = {}});
  }

  void factor(RefId parent, Path path, RefId intermediate) {
    std::vector<Edge> factored;
    for (const auto &edge : edges_) {
      if (edge.parent == parent && pathPrefix(path, edge.path)) {
        factored.push_back(edge);
      }
    }
    for (const auto &edge : factored) {
      edges_.erase(edge);
      Path suffix(edge.path.begin() + static_cast<std::ptrdiff_t>(path.size()), edge.path.end());
      addEdge({.parent = intermediate, .child = edge.child, .strong = edge.strong, .path = std::move(suffix)});
    }
    addEdge({.parent = parent, .child = intermediate, .strong = true, .path = std::move(path)});
  }

  std::map<RefId, Reference> references_;
  std::set<Edge> edges_;
};

struct AbstractState {
  std::vector<std::optional<RefId>> locals;
  BorrowGraph graph;
  RefId next_id = 0;

  friend bool operator==(const AbstractState &, const AbstractState &) = default;
};

[[nodiscard]] RefId frameRoot(const AbstractState &state) { return state.locals.size(); }

[[nodiscard]] RefId newReference(AbstractState &state, bool mutable_reference) {
  const auto id = state.next_id++;
  state.graph.newReference(id, mutable_reference);
  return id;
}

void releaseValue(AbstractState &state, std::optional<RefId> value) {
  if (value.has_value()) {
    state.graph.release(*value);
  }
}

[[nodiscard]] Label localLabel(std::size_t local) { return {.kind = LabelKind::Local, .index = local}; }

[[nodiscard]] Label globalLabel(std::size_t resource) { return {.kind = LabelKind::Global, .index = resource}; }

[[nodiscard]] Label fieldLabel(std::size_t field) { return {.kind = LabelKind::Field, .index = field}; }

[[nodiscard]] bool localBorrowed(const AbstractState &state, std::size_t local) {
  return state.graph.hasConsistentBorrows(frameRoot(state), localLabel(local));
}

[[nodiscard]] bool localMutablyBorrowed(const AbstractState &state, std::size_t local) {
  return state.graph.hasConsistentMutableBorrows(frameRoot(state), localLabel(local));
}

[[nodiscard]] bool globalBorrowed(const AbstractState &state, std::size_t resource) {
  return state.graph.hasConsistentBorrows(frameRoot(state), globalLabel(resource));
}

[[nodiscard]] bool globalMutablyBorrowed(const AbstractState &state, std::size_t resource) {
  return state.graph.hasConsistentMutableBorrows(frameRoot(state), globalLabel(resource));
}

[[nodiscard]] AbstractState initialState(const Module &module, const FunctionDefinition &function) {
  const auto local_types = functionLocalTypes(module, function);
  AbstractState state;
  state.locals.resize(local_types.size());
  state.next_id = local_types.size() + 1;
  const auto &handle = module.function_handles.at(function.handle);
  const auto parameter_count = module.signatures.at(handle.parameters).size();
  for (std::size_t local = 0; local < parameter_count; ++local) {
    if (!isReference(local_types[local])) {
      continue;
    }
    state.graph.newReference(local, isMutableReference(local_types[local]));
    state.locals[local] = local;
  }
  state.graph.newReference(frameRoot(state), true);
  return state;
}

void canonicalize(AbstractState &state) {
  std::map<RefId, RefId> ids;
  ids.emplace(frameRoot(state), frameRoot(state));
  for (std::size_t local = 0; local < state.locals.size(); ++local) {
    if (!state.locals[local].has_value()) {
      continue;
    }
    if (!ids.emplace(*state.locals[local], local).second) {
      analysisError("one reference is stored in multiple locals");
    }
    state.locals[local] = local;
  }
  state.graph = state.graph.remap(ids);
  state.next_id = state.locals.size() + 1;
}

[[nodiscard]] AbstractState joinStates(const AbstractState &left, const AbstractState &right) {
  if (left.locals.size() != right.locals.size()) {
    analysisError("local counts disagree at borrow-state join");
  }
  AbstractState lhs = left;
  AbstractState rhs = right;
  for (std::size_t local = 0; local < lhs.locals.size(); ++local) {
    if (lhs.locals[local].has_value() == rhs.locals[local].has_value()) {
      continue;
    }
    if (lhs.locals[local].has_value()) {
      lhs.graph.release(*lhs.locals[local]);
    } else {
      rhs.graph.release(*rhs.locals[local]);
    }
    lhs.locals[local] = std::nullopt;
    rhs.locals[local] = std::nullopt;
  }
  AbstractState result;
  result.locals = lhs.locals;
  result.graph = BorrowGraph::join(lhs.graph, rhs.graph);
  result.next_id = lhs.next_id;
  return result;
}

[[nodiscard]] std::size_t immediateIndex(const StacklessInstruction &instruction) {
  if (instruction.immediate_operands.empty()) {
    borrowError(instruction, "borrow analysis expected an immediate operand");
  }
  return static_cast<std::size_t>(instruction.immediate_operands.front());
}

[[nodiscard]] std::optional<RefId> inputValue(const StacklessInstruction &instruction, const std::vector<std::optional<RefId>> &values, std::size_t input) {
  if (input >= instruction.inputs.size()) {
    borrowError(instruction, "borrow analysis expected a stack input");
  }
  return values.at(instruction.inputs[input]);
}

[[nodiscard]] RefId inputReference(const StacklessInstruction &instruction, const std::vector<std::optional<RefId>> &values, std::size_t input) {
  const auto value = inputValue(instruction, values, input);
  if (!value.has_value()) {
    borrowError(instruction, "borrow analysis expected a reference operand");
  }
  return *value;
}

void setOutput(const StacklessInstruction &instruction, std::vector<std::optional<RefId>> &values, std::size_t output, std::optional<RefId> value) {
  if (output >= instruction.outputs.size()) {
    borrowError(instruction, "borrow analysis expected a stack output");
  }
  values.at(instruction.outputs[output]) = value;
}

[[nodiscard]] std::size_t fieldIndex(const Module &module, const StacklessInstruction &instruction) {
  const auto index = immediateIndex(instruction);
  switch (instruction.opcode) {
  case Opcode::MutBorrowField:
  case Opcode::ImmBorrowField:
    return module.field_handles.at(index).field;
  case Opcode::MutBorrowFieldGeneric:
  case Opcode::ImmBorrowFieldGeneric:
    return module.field_handles.at(module.field_instantiations.at(index).handle).field;
  case Opcode::MutBorrowVariantField:
  case Opcode::ImmBorrowVariantField:
    return module.variant_field_handles.at(index).field;
  case Opcode::MutBorrowVariantFieldGeneric:
  case Opcode::ImmBorrowVariantFieldGeneric:
    return module.variant_field_handles.at(module.variant_field_instantiations.at(index).handle).field;
  default:
    borrowError(instruction, "opcode does not identify a borrowed field");
  }
}

[[nodiscard]] std::size_t resourceIndex(const Module &module, const StacklessInstruction &instruction) {
  const auto index = immediateIndex(instruction);
  switch (instruction.opcode) {
  case Opcode::MutBorrowGlobalGeneric:
  case Opcode::ImmBorrowGlobalGeneric:
  case Opcode::MoveFromGeneric:
    return module.struct_definition_instantiations.at(index).definition;
  default:
    return index;
  }
}

[[nodiscard]] const FunctionHandle &calledFunction(const Module &module, const StacklessInstruction &instruction) {
  const auto index = immediateIndex(instruction);
  if (instruction.opcode == Opcode::CallGeneric) {
    return module.function_handles.at(module.function_instantiations.at(index).handle);
  }
  return module.function_handles.at(index);
}

[[nodiscard]] std::optional<TableIndex> calledFunctionHandleIndex(const Module &module, const StacklessInstruction &instruction) {
  const auto index = immediateIndex(instruction);
  if (instruction.opcode == Opcode::CallGeneric) {
    return module.function_instantiations.at(index).handle;
  }
  if (instruction.opcode == Opcode::Call) {
    return static_cast<TableIndex>(index);
  }
  return std::nullopt;
}

[[nodiscard]] const FunctionDefinition *findDefinition(const Module &module, TableIndex handle) {
  const auto found = std::find_if(module.function_definitions.begin(), module.function_definitions.end(),
                                  [handle](const FunctionDefinition &definition) { return definition.handle == handle; });
  return found == module.function_definitions.end() ? nullptr : &*found;
}

void releaseReferences(AbstractState &state, const std::vector<RefId> &references) {
  std::set<RefId> unique(references.begin(), references.end());
  for (const auto reference : unique) {
    state.graph.release(reference);
  }
}

class Transfer {
public:
  Transfer(const Module &module, const StacklessFunction &stackless) : module_(module), stackless_(stackless) {}

  void executeBlock(const StacklessBlock &block, AbstractState &state) const {
    std::vector<std::optional<RefId>> values(stackless_.value_count);
    for (const auto &instruction : block.instructions) {
      execute(instruction, state, values);
    }
    canonicalize(state);
  }

private:
  void createCallResults(const StacklessInstruction &instruction, AbstractState &state, std::vector<std::optional<RefId>> &values,
                         const std::vector<RefId> &all_references, const std::vector<RefId> &mutable_references) const {
    for (std::size_t output = 0; output < instruction.outputs.size(); ++output) {
      const auto value_id = instruction.outputs[output];
      const auto &type = stackless_.value_types.at(value_id);
      if (!isReference(type)) {
        values[value_id] = std::nullopt;
        continue;
      }
      const bool mutable_result = isMutableReference(type);
      const auto result = newReference(state, mutable_result);
      const auto &parents = mutable_result ? mutable_references : all_references;
      for (const auto parent : parents) {
        state.graph.addWeakBorrow(parent, result);
      }
      values[value_id] = result;
    }
  }

  void call(const StacklessInstruction &instruction, AbstractState &state, std::vector<std::optional<RefId>> &values, std::size_t argument_count) const {
    std::vector<RefId> all_references;
    std::vector<RefId> mutable_references;
    for (std::size_t index = 0; index < argument_count; ++index) {
      const auto value = inputValue(instruction, values, index);
      if (!value.has_value()) {
        continue;
      }
      all_references.push_back(*value);
      if (state.graph.isMutable(*value)) {
        if (!state.graph.writable(*value)) {
          borrowError(instruction, "call transfers a borrowed mutable reference");
        }
        mutable_references.push_back(*value);
      }
    }

    if (const auto handle = calledFunctionHandleIndex(module_, instruction); handle.has_value()) {
      if (const auto *definition = findDefinition(module_, *handle); definition != nullptr) {
        for (const auto resource : definition->acquires) {
          if (globalBorrowed(state, resource)) {
            borrowError(instruction, "call may acquire a resource which is still borrowed");
          }
        }
      }
    }

    createCallResults(instruction, state, values, all_references, mutable_references);
    releaseReferences(state, all_references);
  }

  void execute(const StacklessInstruction &instruction, AbstractState &state, std::vector<std::optional<RefId>> &values) const {
    const auto local = [&]() { return immediateIndex(instruction); };
    const auto outputReference = [&](bool mutable_reference) {
      const auto id = newReference(state, mutable_reference);
      setOutput(instruction, values, 0, id);
      return id;
    };

    switch (instruction.opcode) {
    case Opcode::Pop:
      releaseValue(state, inputValue(instruction, values, 0));
      return;

    case Opcode::CopyLoc: {
      const auto stored = state.locals.at(local());
      if (stored.has_value()) {
        const auto copy = outputReference(state.graph.isMutable(*stored));
        state.graph.addStrongBorrow(*stored, copy);
      } else {
        if (localMutablyBorrowed(state, local())) {
          borrowError(instruction, "copying a local with an outstanding mutable borrow");
        }
        setOutput(instruction, values, 0, std::nullopt);
      }
      return;
    }

    case Opcode::MoveLoc: {
      const auto stored = state.locals.at(local());
      if (!stored.has_value() && localBorrowed(state, local())) {
        borrowError(instruction, "moving a local which is still borrowed");
      }
      setOutput(instruction, values, 0, stored);
      state.locals[local()] = std::nullopt;
      return;
    }

    case Opcode::StLoc: {
      const auto replacement = inputValue(instruction, values, 0);
      const auto old = state.locals.at(local());
      if (old.has_value()) {
        state.graph.release(*old);
      } else if (localBorrowed(state, local())) {
        borrowError(instruction, "overwriting a local which is still borrowed");
      }
      state.locals[local()] = replacement;
      return;
    }

    case Opcode::FreezeRef: {
      const auto parent = inputReference(instruction, values, 0);
      if (!state.graph.freezable(parent, std::nullopt)) {
        borrowError(instruction, "freezing a reference with a mutable borrower");
      }
      const auto frozen = outputReference(false);
      state.graph.addStrongBorrow(parent, frozen);
      state.graph.release(parent);
      return;
    }

    case Opcode::Eq:
    case Opcode::Neq:
      for (std::size_t index = 0; index < 2; ++index) {
        const auto value = inputValue(instruction, values, index);
        if (value.has_value()) {
          if (!state.graph.readable(*value, std::nullopt)) {
            borrowError(instruction, "comparing a reference with a mutable borrower");
          }
          state.graph.release(*value);
        }
      }
      setOutput(instruction, values, 0, std::nullopt);
      return;

    case Opcode::ReadRef: {
      const auto reference = inputReference(instruction, values, 0);
      if (!state.graph.readable(reference, std::nullopt)) {
        borrowError(instruction, "reading a reference with a mutable borrower");
      }
      state.graph.release(reference);
      setOutput(instruction, values, 0, std::nullopt);
      return;
    }

    case Opcode::WriteRef: {
      const auto reference = inputReference(instruction, values, 1);
      if (!state.graph.writable(reference)) {
        borrowError(instruction, "writing through a reference which is still borrowed");
      }
      state.graph.release(reference);
      return;
    }

    case Opcode::MutBorrowLoc:
    case Opcode::ImmBorrowLoc: {
      const bool mutable_borrow = instruction.opcode == Opcode::MutBorrowLoc;
      if (!mutable_borrow && localMutablyBorrowed(state, local())) {
        borrowError(instruction, "immutable local borrow conflicts with a mutable borrow");
      }
      if (mutable_borrow && state.graph.hasFullBorrows(frameRoot(state))) {
        borrowError(instruction, "mutable local borrow conflicts with an imprecise borrow");
      }
      const auto reference = outputReference(mutable_borrow);
      state.graph.addStrongFieldBorrow(frameRoot(state), localLabel(local()), reference);
      return;
    }

    case Opcode::MutBorrowField:
    case Opcode::MutBorrowFieldGeneric:
    case Opcode::ImmBorrowField:
    case Opcode::ImmBorrowFieldGeneric:
    case Opcode::MutBorrowVariantField:
    case Opcode::MutBorrowVariantFieldGeneric:
    case Opcode::ImmBorrowVariantField:
    case Opcode::ImmBorrowVariantFieldGeneric: {
      const bool mutable_borrow = instruction.opcode == Opcode::MutBorrowField || instruction.opcode == Opcode::MutBorrowFieldGeneric ||
                                  instruction.opcode == Opcode::MutBorrowVariantField || instruction.opcode == Opcode::MutBorrowVariantFieldGeneric;
      const auto parent = inputReference(instruction, values, 0);
      const auto field = fieldLabel(fieldIndex(module_, instruction));
      if ((mutable_borrow && state.graph.hasFullBorrows(parent)) || (!mutable_borrow && !state.graph.readable(parent, field))) {
        borrowError(instruction, "field borrow conflicts with an outstanding borrower");
      }
      const auto reference = outputReference(mutable_borrow);
      state.graph.addStrongFieldBorrow(parent, field, reference);
      state.graph.release(parent);
      return;
    }

    case Opcode::MutBorrowGlobal:
    case Opcode::MutBorrowGlobalGeneric:
    case Opcode::ImmBorrowGlobal:
    case Opcode::ImmBorrowGlobalGeneric: {
      const bool mutable_borrow = instruction.opcode == Opcode::MutBorrowGlobal || instruction.opcode == Opcode::MutBorrowGlobalGeneric;
      const auto resource = resourceIndex(module_, instruction);
      if ((mutable_borrow && globalBorrowed(state, resource)) || globalMutablyBorrowed(state, resource)) {
        borrowError(instruction, "global resource already has a conflicting borrow");
      }
      const auto reference = outputReference(mutable_borrow);
      state.graph.addWeakFieldBorrow(frameRoot(state), globalLabel(resource), reference);
      return;
    }

    case Opcode::MoveFrom:
    case Opcode::MoveFromGeneric:
      if (globalBorrowed(state, resourceIndex(module_, instruction))) {
        borrowError(instruction, "moving a global resource which is still borrowed");
      }
      setOutput(instruction, values, 0, std::nullopt);
      return;

    case Opcode::Call:
    case Opcode::CallGeneric: {
      const auto &handle = calledFunction(module_, instruction);
      const auto special = std::find_if(handle.attributes.begin(), handle.attributes.end(),
                                        [](const FunctionAttribute &attribute) { return attribute.kind == FunctionAttributeKind::BorrowFieldMutable; });
      if (special != handle.attributes.end() && special->value.has_value()) {
        const auto parent = inputReference(instruction, values, 0);
        if (state.graph.hasFullBorrows(parent)) {
          borrowError(instruction, "mutable field API call receives a borrowed reference");
        }
        const auto result = outputReference(true);
        state.graph.addStrongFieldBorrow(parent, fieldLabel(*special->value), result);
        state.graph.release(parent);
        return;
      }
      call(instruction, state, values, instruction.inputs.size());
      return;
    }

    case Opcode::CallClosure:
      if (instruction.inputs.empty()) {
        borrowError(instruction, "closure call has no closure operand");
      }
      call(instruction, state, values, instruction.inputs.size() - 1);
      return;

    case Opcode::Ret: {
      std::set<RefId> released;
      for (auto &stored : state.locals) {
        if (stored.has_value()) {
          released.insert(*stored);
          stored = std::nullopt;
        }
      }
      for (const auto reference : released) {
        state.graph.release(reference);
      }
      if (state.graph.hasConsistentBorrows(frameRoot(state), std::nullopt)) {
        borrowError(instruction, "return leaves a local or global resource borrowed");
      }
      std::vector<RefId> returned_references;
      for (std::size_t index = 0; index < instruction.inputs.size(); ++index) {
        const auto value = inputValue(instruction, values, index);
        if (value.has_value()) {
          if (state.graph.isMutable(*value) && !state.graph.writable(*value)) {
            borrowError(instruction, "return transfers a borrowed mutable reference");
          }
          returned_references.push_back(*value);
        }
      }
      releaseReferences(state, returned_references);
      return;
    }

    case Opcode::MoveTo:
    case Opcode::MoveToGeneric:
      releaseValue(state, inputValue(instruction, values, 0));
      return;

    case Opcode::TestVariant:
    case Opcode::TestVariantGeneric: {
      const auto reference = inputReference(instruction, values, 0);
      if (!state.graph.readable(reference, std::nullopt)) {
        borrowError(instruction, "testing a variant through a mutably borrowed reference");
      }
      state.graph.release(reference);
      setOutput(instruction, values, 0, std::nullopt);
      return;
    }

    case Opcode::VecLen: {
      const auto reference = inputReference(instruction, values, 0);
      state.graph.release(reference);
      setOutput(instruction, values, 0, std::nullopt);
      return;
    }

    case Opcode::VecImmBorrow:
    case Opcode::VecMutBorrow: {
      const bool mutable_borrow = instruction.opcode == Opcode::VecMutBorrow;
      const auto parent = inputReference(instruction, values, 0);
      if ((!mutable_borrow && !state.graph.readable(parent, std::nullopt)) || (mutable_borrow && !state.graph.writable(parent))) {
        borrowError(instruction, "vector element borrow conflicts with another borrow");
      }
      const auto result = outputReference(mutable_borrow);
      state.graph.addWeakBorrow(parent, result);
      state.graph.release(parent);
      return;
    }

    case Opcode::VecPushBack:
    case Opcode::VecPopBack:
    case Opcode::VecSwap: {
      const auto reference = inputReference(instruction, values, 0);
      if (!state.graph.writable(reference)) {
        borrowError(instruction, "vector mutation uses a borrowed reference");
      }
      state.graph.release(reference);
      if (!instruction.outputs.empty()) {
        setOutput(instruction, values, 0, std::nullopt);
      }
      return;
    }

    case Opcode::PackClosure:
    case Opcode::PackClosureGeneric:
      for (std::size_t input = 0; input < instruction.inputs.size(); ++input) {
        if (inputValue(instruction, values, input).has_value()) {
          borrowError(instruction, "closure captures a reference");
        }
      }
      setOutput(instruction, values, 0, std::nullopt);
      return;

    case Opcode::LdU8:
    case Opcode::LdU16:
    case Opcode::LdU32:
    case Opcode::LdU64:
    case Opcode::LdU128:
    case Opcode::LdU256:
    case Opcode::LdI8:
    case Opcode::LdI16:
    case Opcode::LdI32:
    case Opcode::LdI64:
    case Opcode::LdI128:
    case Opcode::LdI256:
    case Opcode::LdTrue:
    case Opcode::LdFalse:
    case Opcode::LdConst:
    case Opcode::Add:
    case Opcode::Sub:
    case Opcode::Mul:
    case Opcode::Mod:
    case Opcode::Div:
    case Opcode::BitOr:
    case Opcode::BitAnd:
    case Opcode::Xor:
    case Opcode::Shl:
    case Opcode::Shr:
    case Opcode::Or:
    case Opcode::And:
    case Opcode::Lt:
    case Opcode::Gt:
    case Opcode::Le:
    case Opcode::Ge:
    case Opcode::Not:
    case Opcode::Negate:
    case Opcode::CastU8:
    case Opcode::CastU16:
    case Opcode::CastU32:
    case Opcode::CastU64:
    case Opcode::CastU128:
    case Opcode::CastU256:
    case Opcode::CastI8:
    case Opcode::CastI16:
    case Opcode::CastI32:
    case Opcode::CastI64:
    case Opcode::CastI128:
    case Opcode::CastI256:
    case Opcode::Exists:
    case Opcode::ExistsGeneric:
    case Opcode::Pack:
    case Opcode::PackGeneric:
    case Opcode::Unpack:
    case Opcode::UnpackGeneric:
    case Opcode::PackVariant:
    case Opcode::PackVariantGeneric:
    case Opcode::UnpackVariant:
    case Opcode::UnpackVariantGeneric:
    case Opcode::VecPack:
    case Opcode::VecUnpack:
      for (std::size_t input = 0; input < instruction.inputs.size(); ++input) {
        if (inputValue(instruction, values, input).has_value()) {
          borrowError(instruction, "value-only opcode consumes a reference");
        }
      }
      for (std::size_t output = 0; output < instruction.outputs.size(); ++output) {
        if (isReference(stackless_.value_types.at(instruction.outputs[output]))) {
          borrowError(instruction, "value-only opcode produces a reference");
        }
        setOutput(instruction, values, output, std::nullopt);
      }
      return;

    case Opcode::BrTrue:
    case Opcode::BrFalse:
    case Opcode::Branch:
    case Opcode::Abort:
    case Opcode::AbortMsg:
    case Opcode::Nop:
      return;
    }
    borrowError(instruction, "borrow transfer is not implemented for opcode");
  }

  const Module &module_;
  const StacklessFunction &stackless_;
};

} // namespace

void validateBorrowSafety(const Module &module, const FunctionDefinition &function, const ControlFlowGraph &graph, const StacklessFunction &stackless) {
  if (!function.code.has_value() || graph.blocks.empty()) {
    return;
  }
  if (stackless.blocks.size() != graph.blocks.size()) {
    analysisError("CFG and stackless block counts disagree in borrow analysis");
  }

  const auto entry = initialState(module, function);
  std::vector<std::optional<AbstractState>> incoming(graph.blocks.size());
  std::vector<std::optional<AbstractState>> outgoing(graph.blocks.size());
  const Transfer transfer(module, stackless);

  bool changed = true;
  std::size_t iterations = 0;
  while (changed) {
    changed = false;
    if (++iterations > 10'000) {
      throw Error(ErrorCode::ResourceLimit, Error::UnknownOffset, "borrow analysis did not converge within 10000 iterations");
    }
    for (const auto &block : graph.blocks) {
      if (!block.reachable) {
        continue;
      }
      std::optional<AbstractState> next_in;
      if (block.id == 0) {
        next_in = entry;
      }
      for (const auto predecessor : block.predecessors) {
        if (!graph.blocks.at(predecessor).reachable || !outgoing.at(predecessor).has_value()) {
          continue;
        }
        next_in = next_in.has_value() ? joinStates(*next_in, *outgoing[predecessor]) : outgoing[predecessor];
      }
      if (!next_in.has_value()) {
        continue;
      }
      auto next_out = *next_in;
      transfer.executeBlock(stackless.blocks.at(block.id), next_out);
      if (incoming[block.id] != next_in || outgoing[block.id] != next_out) {
        incoming[block.id] = std::move(next_in);
        outgoing[block.id] = std::move(next_out);
        changed = true;
      }
    }
  }
}

} // namespace movescape
