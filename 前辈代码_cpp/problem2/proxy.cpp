#include "proxy.hpp"
namespace problem2 {
using namespace std;
using i64 = Id;
// Dense immutable access index: built once per search, reused by all candidates.
// Own lightweight model, ported from dag_chain_solver_v2; no official module calls.
struct ProxyIndex {
    struct Op {
        vector<int> spill_uses, outputs;
    };
    struct Tensor {
        Id size;
        int pos;
    };
    vector<Op> ops;
    vector<Tensor> tensors;
    unordered_map<Id, int> op_index;
    explicit ProxyIndex(const GraphInfo &g) {
        unordered_map<Id, int> tensor_index;
        for (const auto &[id, tensor] : g.tensors) {
            tensor_index[id] = int(tensors.size());
            tensors.push_back({field_id(tensor, "size"),
                               field_string(tensor, "pos") == "L1" ? 0 : 1});
        }
        for (Id id : g.ids) {
            op_index[id] = int(ops.size());
            Op op;
            for (Id t : g.op_inputs.at(id)) {
                op.spill_uses.push_back(tensor_index.at(t));
            }
            for (Id t : g.op_outputs.at(id)) {
                op.outputs.push_back(tensor_index.at(t));
                op.spill_uses.push_back(tensor_index.at(t));
            }
            sort(op.spill_uses.begin(), op.spill_uses.end());
            op.spill_uses.erase(unique(op.spill_uses.begin(), op.spill_uses.end()), op.spill_uses.end());
            ops.push_back(move(op));
        }
    }
};
namespace {
struct ProxyConfig {
    std::array<Id, 2> capacity;
};
struct ProxyPlan {
    vector<int> core, order;
};
struct SpillEstimate {
    double bytes = 0;
    bool infeasible = false;
};
SpillEstimate estimate_spills(const ProxyIndex &g, const ProxyConfig &cfg,
                              const ProxyPlan &s, int k) {
    const int nt = int(g.tensors.size()), NEVER = numeric_limits<int>::max();
    SpillEstimate ans;
    vector<vector<int>> order(k);
    for (int v : s.order)
        order[s.core[v]].push_back(v);
    for (int c = 0; c < k; ++c) {
        poll_search_deadline();
        if (order[c].empty())
            continue;
        int m = int(order[c].size());
        vector<size_t> offsets(size_t(m) + 1);
        for (int i = 0; i < m; ++i)
            offsets[i + 1] = offsets[i] + g.ops[order[c][i]].spill_uses.size();
        vector<pair<int, int>> uses(offsets.back());
        vector<int> next(nt, NEVER);
        vector<unsigned char> local_writer(nt);
        for (int i = 0; i < m; ++i)
            for (int t : g.ops[order[c][i]].outputs)
                local_writer[t] = 1;
        for (int i = m - 1; i >= 0; --i) {
            size_t at = offsets[i];
            for (int t : g.ops[order[c][i]].spill_uses) {
                uses[at++] = {t, next[t]};
                next[t] = i;
            }
        }
        vector<unsigned char> resident(nt), backed(nt);
        vector<int> nextuse(nt, NEVER), protect(nt, -1), insertion(nt);
        // Scheduled reloads precede new allocations, in eviction order. Linked
        // buckets avoid allocating a separate vector for every compute step.
        vector<int> reload_head(m, -1), reload_tail(m, -1), reload_next(nt, -1);
        int stamp = 0;
        using Item = tuple<int, int, int>; // next use, negative insertion stamp, tensor
        std::array<priority_queue<Item>, 2> heap;
        std::array<i64, 2> used{};
        vector<Item> held;
        auto allocate = [&](int t) {
            resident[t] = 1;
            insertion[t] = ++stamp;
            used[g.tensors[t].pos] += g.tensors[t].size;
        };
        for (int i = 0; i < m; ++i) {
            if (i % 512 == 0)
                poll_search_deadline();
            const auto begin = uses.begin() + offsets[i], end = uses.begin() + offsets[i + 1];
            for (int t = reload_head[i]; t >= 0; t = reload_next[t]) {
                allocate(t);
                ans.bytes += double(g.tensors[t].size);
            }
            // Allocate every current input AND output before capacity checks;
            // last-use inputs are freed only after execution.
            for (auto it = begin; it != end; ++it) {
                auto [t, nxt] = *it;
                protect[t] = i;
                if (!resident[t]) {
                    allocate(t);
                    backed[t] = !local_writer[t]; // boundary COPY_IN backing
                }
                nextuse[t] = nxt;
                if (nxt != NEVER)
                    heap[g.tensors[t].pos].push({nxt, -insertion[t], t});
            }
            for (int mem = 0; mem < 2; ++mem) {
                // Avoid overflow in capacity * 90 even on oversized input config.
                const i64 limit = cfg.capacity[mem] / 10 * 9 + cfg.capacity[mem] % 10 * 9 / 10;
                held.clear();
                while (used[mem] > limit) {
                    while (!heap[mem].empty()) {
                        auto [n, neg_stamp, t] = heap[mem].top();
                        if (!resident[t] || nextuse[t] != n || insertion[t] != -neg_stamp) {
                            heap[mem].pop();
                            continue;
                        }
                        if (protect[t] == i) {
                            held.push_back(heap[mem].top());
                            heap[mem].pop();
                            continue;
                        }
                        break;
                    }
                    if (heap[mem].empty()) {
                        // A current op may exceed the soft trigger, never hard capacity.
                        ans.infeasible = ans.infeasible || used[mem] > cfg.capacity[mem];
                        break;
                    }
                    auto [n, neg_stamp, t] = heap[mem].top();
                    (void)neg_stamp;
                    heap[mem].pop();
                    resident[t] = 0;
                    used[mem] -= g.tensors[t].size;
                    if (!backed[t]) {
                        ans.bytes += double(g.tensors[t].size);
                        backed[t] = 1;
                    }
                    reload_next[t] = -1;
                    if (reload_tail[n] < 0)
                        reload_head[n] = t;
                    else
                        reload_next[reload_tail[n]] = t;
                    reload_tail[n] = t;
                }
                for (const auto &item : held)
                    heap[mem].push(item);
            }
            // Later writers modify the reloaded incarnation and invalidate DDR.
            for (int t : g.ops[order[c][i]].outputs)
                backed[t] = 0;
            for (auto it = begin; it != end; ++it) {
                auto [t, nxt] = *it;
                if (nxt == NEVER) {
                    resident[t] = 0;
                    used[g.tensors[t].pos] -= g.tensors[t].size;
                }
            }
        }
    }
    return ans;
}
} // namespace

double ProxyMetrics::score(double bandwidth) const {
    // T 是流水线草图，D 是总 DDR 流量时间；换页额外惩罚保留已验证的权重。
    const double d = (route + spill) / bandwidth;
    return max(sketch, d) + spill / bandwidth + .03 * d +
           (memory_warning ? 1e18 : 0);
}
ScreeningProxy::ScreeningProxy(const GraphInfo &graph)
    : graph_(&graph), index_(make_shared<ProxyIndex>(graph)) {}
ProxyMetrics ScreeningProxy::evaluate(const Plan &plan, const Settings &settings) const {
    const auto &g = *graph_;
    const auto &idx = *index_;
    ProxyPlan s;
    s.core.resize(idx.ops.size(), -1);
    s.order.reserve(idx.ops.size());
    for (Id block : plan.order) {
        for (Id v : plan.blocks.at(block)) {
            int op = idx.op_index.at(v);
            s.core[op] = int(plan.core_of.at(block));
            s.order.push_back(op);
        }
    }
    ProxyConfig config{{settings.capacity.at("L1"), settings.capacity.at("UB")}};
    const auto spill = estimate_spills(idx, config, s, int(plan.ncores));
    ProxyMetrics result;
    result.spill = spill.bytes;
    result.memory_warning = spill.infeasible;
    result.route = double(route_bytes(g, plan));
    result.sketch = replay_sketch(g, plan, settings);
    return result;
}
} // namespace problem2
