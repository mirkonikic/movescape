#include "movescape/graph_analysis.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace movescape {

namespace {

using Adjacency = std::vector<std::vector<BlockId>>;
using BitMatrix = std::vector<std::vector<bool>>;

struct InternalDominators {
  std::vector<bool> reachable;
  BitMatrix sets;
  std::vector<std::optional<BlockId>> immediate;
};

[[nodiscard]] Adjacency adjacencyOf(const ControlFlowGraph &graph) {
  Adjacency result(graph.blocks.size());
  for (const auto &block : graph.blocks) {
    for (const auto &edge : block.successors) {
      result[block.id].push_back(edge.target);
    }
  }
  return result;
}

[[nodiscard]] Adjacency predecessorsOf(const Adjacency &adjacency) {
  Adjacency result(adjacency.size());
  for (BlockId source = 0; source < adjacency.size(); ++source) {
    for (const auto target : adjacency[source]) {
      result[target].push_back(source);
    }
  }
  return result;
}

[[nodiscard]] std::vector<bool> reachableFrom(const Adjacency &adjacency, BlockId root) {
  std::vector<bool> reachable(adjacency.size(), false);
  if (root >= adjacency.size()) {
    return reachable;
  }
  std::vector<BlockId> pending{root};
  reachable[root] = true;
  while (!pending.empty()) {
    const auto current = pending.back();
    pending.pop_back();
    for (const auto successor : adjacency[current]) {
      if (!reachable[successor]) {
        reachable[successor] = true;
        pending.push_back(successor);
      }
    }
  }
  return reachable;
}

[[nodiscard]] InternalDominators computeDominators(const Adjacency &adjacency, BlockId root) {
  InternalDominators result;
  const auto size = adjacency.size();
  result.reachable = reachableFrom(adjacency, root);
  result.sets.assign(size, std::vector<bool>(size, false));
  result.immediate.resize(size);
  if (root >= size) {
    return result;
  }

  const auto predecessors = predecessorsOf(adjacency);
  for (BlockId node = 0; node < size; ++node) {
    if (!result.reachable[node]) {
      result.sets[node][node] = true;
    } else if (node == root) {
      result.sets[node][node] = true;
    } else {
      for (BlockId candidate = 0; candidate < size; ++candidate) {
        result.sets[node][candidate] = result.reachable[candidate];
      }
    }
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (BlockId node = 0; node < size; ++node) {
      if (!result.reachable[node] || node == root) {
        continue;
      }
      std::vector<bool> next(size, true);
      bool has_predecessor = false;
      for (const auto predecessor : predecessors[node]) {
        if (!result.reachable[predecessor]) {
          continue;
        }
        if (!has_predecessor) {
          next = result.sets[predecessor];
          has_predecessor = true;
        } else {
          for (BlockId candidate = 0; candidate < size; ++candidate) {
            next[candidate] = next[candidate] && result.sets[predecessor][candidate];
          }
        }
      }
      if (!has_predecessor) {
        std::fill(next.begin(), next.end(), false);
      }
      next[node] = true;
      if (next != result.sets[node]) {
        result.sets[node] = std::move(next);
        changed = true;
      }
    }
  }

  for (BlockId node = 0; node < size; ++node) {
    if (!result.reachable[node] || node == root) {
      continue;
    }
    std::optional<BlockId> best;
    std::size_t best_depth = 0;
    for (BlockId candidate = 0; candidate < size; ++candidate) {
      if (candidate == node || !result.sets[node][candidate]) {
        continue;
      }
      const auto depth = static_cast<std::size_t>(std::count(result.sets[candidate].begin(), result.sets[candidate].end(), true));
      if (!best.has_value() || depth > best_depth) {
        best = candidate;
        best_depth = depth;
      }
    }
    result.immediate[node] = best;
  }
  return result;
}

[[nodiscard]] std::vector<BlockId> finishOrder(const Adjacency &adjacency) {
  struct Frame {
    BlockId node;
    std::size_t successor;
  };
  std::vector<std::uint8_t> state(adjacency.size(), 0);
  std::vector<BlockId> result;
  for (BlockId root = 0; root < adjacency.size(); ++root) {
    if (state[root] != 0) {
      continue;
    }
    std::vector<Frame> stack{{root, 0}};
    state[root] = 1;
    while (!stack.empty()) {
      auto &frame = stack.back();
      if (frame.successor < adjacency[frame.node].size()) {
        const auto successor = adjacency[frame.node][frame.successor++];
        if (state[successor] == 0) {
          state[successor] = 1;
          stack.push_back({successor, 0});
        }
      } else {
        state[frame.node] = 2;
        result.push_back(frame.node);
        stack.pop_back();
      }
    }
  }
  return result;
}

[[nodiscard]] StronglyConnectedComponents computeSccs(const Adjacency &adjacency) {
  StronglyConnectedComponents result;
  result.component_of.assign(adjacency.size(), std::numeric_limits<std::size_t>::max());
  const auto reverse = predecessorsOf(adjacency);
  const auto finish = finishOrder(adjacency);
  std::vector<bool> seen(adjacency.size(), false);

  for (auto iterator = finish.rbegin(); iterator != finish.rend(); ++iterator) {
    const auto root = *iterator;
    if (seen[root]) {
      continue;
    }
    std::vector<BlockId> component;
    std::vector<BlockId> pending{root};
    seen[root] = true;
    while (!pending.empty()) {
      const auto current = pending.back();
      pending.pop_back();
      component.push_back(current);
      for (const auto predecessor : reverse[current]) {
        if (!seen[predecessor]) {
          seen[predecessor] = true;
          pending.push_back(predecessor);
        }
      }
    }
    std::sort(component.begin(), component.end());
    result.components.push_back(std::move(component));
  }
  std::sort(result.components.begin(), result.components.end(), [](const auto &left, const auto &right) { return left.front() < right.front(); });
  for (std::size_t index = 0; index < result.components.size(); ++index) {
    for (const auto node : result.components[index]) {
      result.component_of[node] = index;
    }
  }
  return result;
}

[[nodiscard]] bool contains(const std::vector<BlockId> &values, BlockId value) { return std::binary_search(values.begin(), values.end(), value); }

[[nodiscard]] bool strictSubset(const std::vector<BlockId> &inner, const std::vector<BlockId> &outer) {
  return inner.size() < outer.size() && std::includes(outer.begin(), outer.end(), inner.begin(), inner.end());
}

} // namespace

GraphAnalysis analyzeControlFlowGraph(const ControlFlowGraph &graph) {
  GraphAnalysis analysis;
  const auto adjacency = adjacencyOf(graph);
  const auto node_count = adjacency.size();
  if (node_count == 0) {
    analysis.postdominators.synthetic_exit = 0;
    return analysis;
  }

  auto finish = finishOrder(adjacency);
  for (auto iterator = finish.rbegin(); iterator != finish.rend(); ++iterator) {
    if (graph.blocks[*iterator].reachable) {
      analysis.reverse_postorder.push_back(*iterator);
    }
  }
  analysis.sccs = computeSccs(adjacency);

  const auto dominators = computeDominators(adjacency, 0);
  analysis.dominators.dominators.resize(node_count);
  analysis.dominators.immediate_dominator = dominators.immediate;
  analysis.dominators.tree_children.resize(node_count);
  analysis.dominators.dominance_frontier.resize(node_count);
  for (BlockId node = 0; node < node_count; ++node) {
    for (BlockId candidate = 0; candidate < node_count; ++candidate) {
      if (dominators.sets[node][candidate]) {
        analysis.dominators.dominators[node].push_back(candidate);
      }
    }
    if (dominators.immediate[node].has_value()) {
      analysis.dominators.tree_children[*dominators.immediate[node]].push_back(node);
    }
  }

  for (const auto &block : graph.blocks) {
    if (!block.reachable || block.predecessors.size() < 2) {
      continue;
    }
    for (auto runner : block.predecessors) {
      if (!graph.blocks[runner].reachable) {
        continue;
      }
      while (runner != analysis.dominators.immediate_dominator[block.id].value_or(node_count)) {
        auto &frontier = analysis.dominators.dominance_frontier[runner];
        if (std::find(frontier.begin(), frontier.end(), block.id) == frontier.end()) {
          frontier.push_back(block.id);
        }
        const auto next = analysis.dominators.immediate_dominator[runner];
        if (!next.has_value()) {
          break;
        }
        runner = *next;
      }
    }
  }

  const auto synthetic_exit = node_count;
  analysis.postdominators.synthetic_exit = synthetic_exit;
  Adjacency reverse_with_exit(node_count + 1);
  for (BlockId source = 0; source < node_count; ++source) {
    for (const auto target : adjacency[source]) {
      reverse_with_exit[target].push_back(source);
    }
    if (graph.blocks[source].successors.empty()) {
      reverse_with_exit[synthetic_exit].push_back(source);
    }
  }
  const auto postdominators = computeDominators(reverse_with_exit, synthetic_exit);
  analysis.postdominators.can_reach_exit.assign(postdominators.reachable.begin(), postdominators.reachable.begin() + static_cast<std::ptrdiff_t>(node_count));
  analysis.postdominators.postdominators.resize(node_count);
  analysis.postdominators.immediate_postdominator.resize(node_count);
  for (BlockId node = 0; node < node_count; ++node) {
    for (BlockId candidate = 0; candidate <= node_count; ++candidate) {
      if (postdominators.sets[node][candidate]) {
        analysis.postdominators.postdominators[node].push_back(candidate);
      }
    }
    analysis.postdominators.immediate_postdominator[node] = postdominators.immediate[node];
  }

  analysis.control_dependents.resize(node_count);
  for (BlockId source = 0; source < node_count; ++source) {
    if (!analysis.postdominators.can_reach_exit[source]) {
      continue;
    }
    const auto stop = analysis.postdominators.immediate_postdominator[source];
    for (const auto target : adjacency[source]) {
      if (!analysis.postdominators.can_reach_exit[target] || postdominators.sets[source][target]) {
        continue;
      }
      auto runner = target;
      std::size_t steps = 0;
      while ((!stop.has_value() || runner != *stop) && runner < node_count && steps++ <= node_count) {
        auto &dependents = analysis.control_dependents[source];
        if (std::find(dependents.begin(), dependents.end(), runner) == dependents.end()) {
          dependents.push_back(runner);
        }
        const auto next = analysis.postdominators.immediate_postdominator[runner];
        if (!next.has_value()) {
          break;
        }
        runner = *next;
      }
    }
    std::sort(analysis.control_dependents[source].begin(), analysis.control_dependents[source].end());
  }

  for (BlockId source = 0; source < node_count; ++source) {
    if (!graph.blocks[source].reachable) {
      continue;
    }
    for (const auto target : adjacency[source]) {
      if (dominators.sets[source][target]) {
        analysis.back_edges.push_back({.source = source, .header = target});
      }
    }
  }

  for (const auto &edge : analysis.back_edges) {
    auto loop = std::find_if(analysis.natural_loops.begin(), analysis.natural_loops.end(),
                             [&](const NaturalLoop &candidate) { return candidate.header == edge.header; });
    if (loop == analysis.natural_loops.end()) {
      analysis.natural_loops.push_back({.header = edge.header, .latches = {}, .members = {edge.header}, .parent = std::nullopt});
      loop = std::prev(analysis.natural_loops.end());
    }
    loop->latches.push_back(edge.source);
    std::set<BlockId> members(loop->members.begin(), loop->members.end());
    std::vector<BlockId> pending{edge.source};
    members.insert(edge.source);
    while (!pending.empty()) {
      const auto current = pending.back();
      pending.pop_back();
      for (const auto predecessor : graph.blocks[current].predecessors) {
        if (graph.blocks[predecessor].reachable && dominators.sets[predecessor][edge.header] && members.insert(predecessor).second &&
            predecessor != edge.header) {
          pending.push_back(predecessor);
        }
      }
    }
    loop->members.assign(members.begin(), members.end());
  }
  std::sort(analysis.natural_loops.begin(), analysis.natural_loops.end(), [](const auto &left, const auto &right) { return left.header < right.header; });
  for (std::size_t inner = 0; inner < analysis.natural_loops.size(); ++inner) {
    std::optional<std::size_t> parent;
    for (std::size_t outer = 0; outer < analysis.natural_loops.size(); ++outer) {
      if (inner == outer || !strictSubset(analysis.natural_loops[inner].members, analysis.natural_loops[outer].members)) {
        continue;
      }
      if (!parent.has_value() || analysis.natural_loops[outer].members.size() < analysis.natural_loops[*parent].members.size()) {
        parent = outer;
      }
    }
    analysis.natural_loops[inner].parent = parent;
  }

  for (const auto &component : analysis.sccs.components) {
    const bool self_cycle = component.size() == 1 && std::find(adjacency[component.front()].begin(), adjacency[component.front()].end(), component.front()) !=
                                                         adjacency[component.front()].end();
    if (component.size() == 1 && !self_cycle) {
      continue;
    }
    bool any_reachable = false;
    std::vector<BlockId> entries;
    if (contains(component, 0)) {
      entries.push_back(0);
    }
    for (const auto member : component) {
      any_reachable = any_reachable || graph.blocks[member].reachable;
      for (const auto predecessor : graph.blocks[member].predecessors) {
        if (graph.blocks[predecessor].reachable && !contains(component, predecessor)) {
          entries.push_back(member);
        }
      }
    }
    if (!any_reachable) {
      continue;
    }
    std::sort(entries.begin(), entries.end());
    entries.erase(std::unique(entries.begin(), entries.end()), entries.end());
    if (entries.size() != 1) {
      analysis.irreducible_regions.push_back(component);
      continue;
    }
    const auto header = entries.front();
    const bool dominates_all = std::all_of(component.begin(), component.end(), [&](BlockId member) { return dominators.sets[member][header]; });
    if (!dominates_all) {
      analysis.irreducible_regions.push_back(component);
    }
  }

  return analysis;
}

std::string formatGraphAnalysis(const GraphAnalysis &analysis) {
  const auto block = [](BlockId id) { return "bb" + std::to_string(id); };
  const auto node = [&](BlockId id) { return id == analysis.postdominators.synthetic_exit ? std::string("exit") : block(id); };
  std::ostringstream out;
  out << "reverse-postorder:";
  for (const auto id : analysis.reverse_postorder) {
    out << ' ' << block(id);
  }
  out << "\nreducible: " << (analysis.reducible() ? "yes" : "no") << '\n';

  out << "\ndominators:\n";
  for (BlockId id = 0; id < analysis.dominators.dominators.size(); ++id) {
    out << "  " << block(id) << " idom=";
    if (analysis.dominators.immediate_dominator[id].has_value()) {
      out << block(*analysis.dominators.immediate_dominator[id]);
    } else {
      out << '-';
    }
    out << " set={";
    for (std::size_t index = 0; index < analysis.dominators.dominators[id].size(); ++index) {
      if (index != 0) {
        out << ", ";
      }
      out << block(analysis.dominators.dominators[id][index]);
    }
    out << "} frontier={";
    for (std::size_t index = 0; index < analysis.dominators.dominance_frontier[id].size(); ++index) {
      if (index != 0) {
        out << ", ";
      }
      out << block(analysis.dominators.dominance_frontier[id][index]);
    }
    out << "}\n";
  }

  out << "\npostdominators:\n";
  for (BlockId id = 0; id < analysis.postdominators.postdominators.size(); ++id) {
    out << "  " << block(id) << " reaches-exit=" << (analysis.postdominators.can_reach_exit[id] ? "yes" : "no") << " ipdom=";
    if (analysis.postdominators.immediate_postdominator[id].has_value()) {
      out << node(*analysis.postdominators.immediate_postdominator[id]);
    } else {
      out << '-';
    }
    out << " set={";
    for (std::size_t index = 0; index < analysis.postdominators.postdominators[id].size(); ++index) {
      if (index != 0) {
        out << ", ";
      }
      out << node(analysis.postdominators.postdominators[id][index]);
    }
    out << "}\n";
  }

  out << "\nstrongly-connected-components:\n";
  for (std::size_t index = 0; index < analysis.sccs.components.size(); ++index) {
    out << "  scc" << index << "={";
    for (std::size_t member = 0; member < analysis.sccs.components[index].size(); ++member) {
      if (member != 0) {
        out << ", ";
      }
      out << block(analysis.sccs.components[index][member]);
    }
    out << "}\n";
  }

  out << "\nback-edges:\n";
  for (const auto &edge : analysis.back_edges) {
    out << "  " << block(edge.source) << " -> " << block(edge.header) << '\n';
  }

  out << "\nnatural-loops:\n";
  for (std::size_t index = 0; index < analysis.natural_loops.size(); ++index) {
    const auto &loop = analysis.natural_loops[index];
    out << "  loop" << index << " header=" << block(loop.header) << " parent=";
    if (loop.parent.has_value()) {
      out << "loop" << *loop.parent;
    } else {
      out << '-';
    }
    out << " latches={";
    for (std::size_t latch = 0; latch < loop.latches.size(); ++latch) {
      if (latch != 0) {
        out << ", ";
      }
      out << block(loop.latches[latch]);
    }
    out << "} members={";
    for (std::size_t member = 0; member < loop.members.size(); ++member) {
      if (member != 0) {
        out << ", ";
      }
      out << block(loop.members[member]);
    }
    out << "}\n";
  }

  out << "\ncontrol-dependence:\n";
  for (BlockId controller = 0; controller < analysis.control_dependents.size(); ++controller) {
    if (analysis.control_dependents[controller].empty()) {
      continue;
    }
    out << "  " << block(controller) << " controls {";
    for (std::size_t index = 0; index < analysis.control_dependents[controller].size(); ++index) {
      if (index != 0) {
        out << ", ";
      }
      out << block(analysis.control_dependents[controller][index]);
    }
    out << "}\n";
  }

  if (!analysis.irreducible_regions.empty()) {
    out << "\nirreducible-regions:\n";
    for (const auto &region : analysis.irreducible_regions) {
      out << "  {";
      for (std::size_t index = 0; index < region.size(); ++index) {
        if (index != 0) {
          out << ", ";
        }
        out << block(region[index]);
      }
      out << "}\n";
    }
  }
  return out.str();
}

} // namespace movescape
