/*!
 *  Copyright (c) 2016 by Contributors
 * \file graph.cc
 * \brief Utilities to get information about schedule graph.
 */
#include <tvm/ir.h>
#include <tvm/ir_visitor.h>
#include <tvm/operation.h>
#include <unordered_set>
#include <unordered_map>
#include "./graph.h"

namespace tvm {
namespace schedule {
// key to specific tensor dimension.
struct TensorDimKey {
  FunctionRef f;
  int value_index;
  int dim;
  TensorDimKey() {}
  TensorDimKey(const ir::Call* op, int dim)
      : f(op->func), value_index(op->value_index), dim(dim) {
  }
  TensorDimKey(const Tensor& t, int dim)
      : f(t->op), value_index(t->value_index), dim(dim) {
  }
  TensorDimKey(const Tensor& t, size_t dim)
      : f(t->op), value_index(t->value_index), dim(static_cast<int>(dim)) {
  }
  inline bool operator==(const TensorDimKey& other) const {
    return f == other.f &&
        value_index == other.value_index &&
        dim == other.dim;
  }
  inline bool operator!=(const TensorDimKey& other) const {
    return !operator==(other);
  }
};
}  // namespace schedule
}  // namespace tvm

namespace std {
template <>
struct hash<::tvm::schedule::TensorDimKey> {
  std::size_t operator()(const ::tvm::schedule::TensorDimKey& k) const {
    size_t lhs = k.f.hash();
    size_t rhs = static_cast<size_t>(k.value_index) << 16UL |
        static_cast<size_t>(k.dim);
    lhs ^= rhs + 0x9e3779b9 + (lhs << 6) + (lhs >> 2);
    return lhs;
  }
};
}  // namespace std


namespace tvm {
namespace schedule {

// construct a read graph that gives readers of each operation
// that the root depend on
ReadGraph CreateReadGraph(const Array<Operation>& roots) {
  ReadGraph rmap;
  std::vector<Operation> stack;
  std::unordered_set<const Node*> visited;
  // initialize the roots
  for (Operation op : roots) {
    stack.push_back(op);
    visited.insert(op.get());
  }

  while (!stack.empty()) {
    Operation op = stack.back();
    stack.pop_back();
    Array<Tensor> deps = op->InputTensors();
    rmap.Set(op, deps);
    for (Tensor t : deps) {
      if (t->op.defined() && visited.count(t->op.get()) == 0) {
        visited.insert(t->op.get());
        stack.push_back(t->op);
      }
    }
  }
  return rmap;
}

// Do DFS visit to get the subgraph.
// Return if op is inside the subgraph.
bool GetSubGraphByPostDFS_(
    const Operation& op,
    const std::unordered_set<const Node*>& boundary,
    bool include_bounary,
    std::unordered_map<const Node*, bool>* visited,
    Array<Operation>* result) {
  if (visited->count(op.get())) {
    return visited->at(op.get());
  }
  if (boundary.count(op.get())) {
    (*visited)[op.get()] = true;
    if (include_bounary) {
      result->push_back(op);
    }
    return true;
  }
  // mark to avoid loop
  // Not necessary for DAG.
  (*visited)[op.get()] = false;
  // check if we can reach boundary.
  bool reach_boundary = false;
  for (Tensor t : op->InputTensors()) {
    if (GetSubGraphByPostDFS_(t->op, boundary,
                              include_bounary,
                              visited, result)) {
      reach_boundary = true;
    }
  }
  (*visited)[op.get()] = reach_boundary;
  if (reach_boundary) {
    result->push_back(op);
  }
  return reach_boundary;
}

Array<Operation> GetSubGraph(const Array<Tensor>& outputs,
                             const Array<Tensor>& inputs,
                             bool include_inputs) {
  Array<Operation> result;
  std::unordered_set<const Node*> boundary;
  for (Tensor t : inputs) {
    boundary.insert(t->op.get());
  }
  std::unordered_map<const Node*, bool> visited;
  for (Tensor t : outputs) {
    GetSubGraphByPostDFS_(t->op, boundary, include_inputs,
                          &visited, &result);
  }
  return result;
}


void PostDFSOrder(const Operation& op,
                  const ReadGraph& g,
                  std::unordered_set<Operation>* visited,
                  Array<Operation>* post_order) {
  if (visited->count(op)) return;
  visited->insert(op);
  for (const auto& t : g.at(op)) {
    PostDFSOrder(t->op, g, visited, post_order);
  }
  post_order->push_back(op);
}

Array<Operation> PostDFSOrder(
    const Array<Operation>& roots,
    const ReadGraph& g) {
  std::unordered_set<Operation> visited;
  Array<Operation> post_order;
  for (Operation op : roots) {
    PostDFSOrder(op, g, &visited, &post_order);
  }
  return post_order;
}

FeedGraph CreateFeedGraph(const ReadGraph& g) {
  FeedGraph fg;
  for (auto kv : g) {
    for (Tensor t : kv.second) {
      fg[t].push_back(kv.first);
    }
  }
  return fg;
}

AttachPath CreateAttachPath(Schedule sch) {
  AttachPath ret;
  for (Stage stage : sch->stages) {
    std::unordered_set<const Node*> visited;
    Array<IterVar> path;
    for (Stage s = stage; s.defined();) {
      CHECK(!visited.count(s.get()))
          << "Find loop in compute_at attach group";
      visited.insert(s.get());
      Stage spec = s.GetAttachSpec();
      bool start_attach;
      IterVar attach_ivar;
      if (spec->attach_type == kScope) {
        attach_ivar = spec->attach_ivar;
        s = spec->attach_stage;
        start_attach = false;
        CHECK(attach_ivar.defined());
      } else if (spec->attach_type == kScanUpdate) {
        s = spec->attach_stage;
        start_attach = true;
      } else {
        break;
      }
      CHECK(s.defined());
      for (size_t i = s->leaf_iter_vars.size(); i != 0; --i) {
        IterVar iv = s->leaf_iter_vars[i - 1];
        if (!start_attach && iv.same_as(attach_ivar)) {
          start_attach = true;
        }
        if (start_attach) path.push_back(iv);
      }
      CHECK(start_attach)
          << "Invalid Schedule: cannot find attach point " << attach_ivar
          << " in the schedule of " << s->op;
    }
    if (!ret.count(stage->op)) {
      ret.Set(stage->op, path);
    }
  }
  return ret;
}

// graph of push reach relation of tensor dimensions
using ReachGraph = std::unordered_map<TensorDimKey, std::vector<TensorDimKey> >;

ReachGraph GetReachGraph(const Array<Operation>& ops) {
  ReachGraph reach;
  std::unordered_set<const Node*> bset;
  for (size_t i = 0; i < ops.size(); ++i) {
    bset.insert(ops[i].get());
  }

  for (Operation op : ops) {
    if (op.as<ScanOpNode>()) {
      const auto& update = op.as<ScanOpNode>()->update;
      const auto& init = op.as<ScanOpNode>()->init;
      for (size_t i = 0; i < update.size(); ++i) {
        Tensor t = op.output(i);
        for (int k = 1; k < static_cast<int>(update[i]->shape.size()); ++k) {
          reach[TensorDimKey(t, k)].emplace_back(
              TensorDimKey(update[i], k));
          reach[TensorDimKey(t, k)].emplace_back(
              TensorDimKey(init[i], k));
        }
      }
    } else if (op.as<ComputeOpNode>()) {
      std::unordered_map<const Node*, TensorDimKey> vmap;
      const auto& axis = op.as<ComputeOpNode>()->axis;
      Tensor t = op.output(0);
      for (size_t i = 0; i < axis.size(); ++i) {
        vmap[axis[i]->var.get()] = TensorDimKey(t, i);
        reach[TensorDimKey(t, i)] = {};
      }
      auto fvisit = [&vmap, &reach, &bset](const NodeRef& n) {
        const ir::Call *call = n.as<ir::Call>();
        if (call != nullptr && call->func.defined()) {
          if (!bset.count(call->func.get())) return;
          for (size_t i = 0; i < call->args.size(); ++i) {
            TensorDimKey dkey(call, static_cast<int>(i));
            auto fpush = [&dkey, &vmap, &reach](const NodeRef& node) {
              const Variable *v = node.as<Variable>();
              auto it = vmap.find(v);
              if (it != vmap.end()) {
                reach[it->second].push_back(dkey);
              }
            };
            ir::PostOrderVisit(call->args[i], fpush);
          }
        }
      };
      ir::PostOrderVisit(op.as<ComputeOpNode>()->body, fvisit);
    }
  }
  return reach;
}

Array<Operation> ScanGetBody(const Operation& scan_op) {
  const ScanOpNode* scan = scan_op.as<ScanOpNode>();
  // Get the body.
  Array<Tensor> inputs;
  for (Tensor t : scan->state_placeholder) {
    inputs.push_back(t);
  }
  for (Tensor t : scan->inputs) {
    inputs.push_back(t);
  }
  return GetSubGraph(scan->update, inputs, false);
}

Map<IterVar, Expr> ScanFixPointAnalysis(const Operation& scan_op) {
  const ScanOpNode* scan = scan_op.as<ScanOpNode>();
  Array<Operation> body = ScanGetBody(scan_op);

  std::unordered_map<TensorDimKey, const Node*> exact_reach;
  std::unordered_set<const Node*> fail_set;

  for (size_t i = 0, sp_idx = 0; i < scan->update.size(); ++i) {
    for (size_t k = 1; k < scan->update[i]->shape.size(); ++k, ++sp_idx) {
      TensorDimKey key(scan->state_placeholder[i], k);
      exact_reach[key] = scan->spatial_axis_[sp_idx].get();
    }
  }
  // merge exact reach
  auto f_merge_key = [&exact_reach, &fail_set](
      const TensorDimKey& dst, const TensorDimKey& src) {
    auto sit = exact_reach.find(src);
    if (sit == exact_reach.end()) return;
    auto dit = exact_reach.find(dst);
    if (dit == exact_reach.end()) {
      exact_reach[dst] = sit->second;
    } else {
      if (dit->second != sit->second) {
        fail_set.insert(dit->second);
        fail_set.insert(sit->second);
      }
    }
  };
  // prop exact reach back.
  for (size_t i = 0; i < body.size(); ++i) {
    const Operation& op = body[i];
    if (op.as<ScanOpNode>()) {
      const auto& update = op.as<ScanOpNode>()->update;
      const auto& init = op.as<ScanOpNode>()->init;
      for (size_t i = 0; i < update.size(); ++i) {
        Tensor t = op.output(i);
        for (size_t k = 1; i < update[i]->shape.size(); ++k) {
          f_merge_key(TensorDimKey(t, k), TensorDimKey(update[i], k));
          f_merge_key(TensorDimKey(t, k), TensorDimKey(init[i], k));
        }
      }
    } else if (op.as<ComputeOpNode>()) {
      std::unordered_map<const Node*, TensorDimKey> vmap;
      const auto& axis = op.as<ComputeOpNode>()->axis;
      Tensor t = op.output(0);
      for (size_t i = 0; i < axis.size(); ++i) {
        vmap[axis[i]->var.get()] = TensorDimKey(t, i);
      }
      auto fvisit = [&vmap, &f_merge_key, &exact_reach, &fail_set](
          const NodeRef& n) {
        const ir::Call *call = n.as<ir::Call>();
        if (call != nullptr && call->func.defined()) {
          for (size_t i = 0; i < call->args.size(); ++i) {
            auto it = vmap.find(call->args[i].get());
            TensorDimKey src(call, static_cast<int>(i));
            if (it != vmap.end()) {
              f_merge_key(it->second, src);
            } else {
              if (exact_reach.count(src)) {
                fail_set.insert(exact_reach.at(src));
              }
            }
          }
        }
      };
      ir::PostOrderVisit(op.as<ComputeOpNode>()->body, fvisit);
    }
  }
  ReachGraph reach;
  Map<IterVar, Expr> ret;
  std::unordered_set<TensorDimKey> place_holder_ref;
  for (size_t i = 0; i < scan->state_placeholder.size(); ++i) {
    for (size_t k = 0; k < scan->state_placeholder[i]->shape.size(); ++k) {
      place_holder_ref.insert(TensorDimKey(scan->state_placeholder[i], k));
    }
  }

  for (size_t i = 0, sp_idx = 0; i < scan->update.size(); ++i) {
    for (size_t k = 1; k < scan->update[i]->shape.size(); ++k, ++sp_idx) {
      TensorDimKey key(scan->update[i], k);
      TensorDimKey target(scan->state_placeholder[i], k);
      IterVar sp_iv = scan->spatial_axis_[sp_idx];
      if (fail_set.count(sp_iv.get()) ||
          !exact_reach.count(key) ||
          exact_reach.at(key) != sp_iv.get()) {
        ret.Set(sp_iv, make_const(Int(32), 0));
      } else {
        // now we proved exact match, need to prove no interference with other graph.
        if (reach.size() == 0) reach = GetReachGraph(body);
        // do a DFS
        std::unordered_set<TensorDimKey> visited;
        std::vector<TensorDimKey> stack{key};
        visited.insert(key);
        while (!stack.empty()) {
          TensorDimKey k = stack.back();
          if (k != target && place_holder_ref.count(k)) break;
          stack.pop_back();
          if (!reach.count(k)) {
            LOG(FATAL) << "cannot find reach of " << k.f << "-" << k.dim;
          }

          for (TensorDimKey kk : reach.at(k)) {
            if (visited.count(kk)) {
              continue;
            }
            visited.insert(kk);
            stack.push_back(kk);
          }
        }
        if (!stack.empty()) {
          // failed the prove.
          ret.Set(sp_iv, make_const(Int(32), 0));
        } else {
          ret.Set(sp_iv, make_const(Int(32), 1));
        }
      }
    }
  }
  return ret;
}

}  // namespace schedule
}  // namespace tvm
