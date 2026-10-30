#include "proxy.hpp"
namespace problem2 {
using namespace std;
using i64 = Id;
// Dense immutable access index: built once per search, reused by all candidates.
// Own lightweight model, ported from dag_chain_solver_v2; no official module calls.
struct ProxyIndex {
    struct Op { Id id; vector<int> spill_uses, outputs; };
    struct Tensor { Id size; int pos; vector<int> producers; };
    vector<Op> ops;
    vector<Tensor> tensors;
    unordered_map<Id, int> op_index;
    explicit ProxyIndex(const GraphInfo &g) {
        unordered_map<Id,int> tensor_index;
        for (const auto &[id, tensor] : g.tensors) {
            tensor_index[id] = int(tensors.size());
            tensors.push_back({field_id(tensor,"size"), field_string(tensor,"pos") == "L1" ? 0 : 1, {}});
        }
        for (Id id : g.ids) {
            op_index[id] = int(ops.size());
            Op op; op.id = id;
            for (Id t : g.op_inputs.at(id)) op.spill_uses.push_back(tensor_index.at(t));
            for (Id t : g.op_outputs.at(id)) {
                op.outputs.push_back(tensor_index.at(t));
                op.spill_uses.push_back(tensor_index.at(t));
            }
            sort(op.spill_uses.begin(), op.spill_uses.end());
            op.spill_uses.erase(unique(op.spill_uses.begin(), op.spill_uses.end()), op.spill_uses.end());
            ops.push_back(move(op));
        }
        for (const auto &[id, producers] : g.eligible_producers)
            for (Id v : producers) tensors[tensor_index.at(id)].producers.push_back(op_index.at(v));
    }
};
struct ProxyConfig { std::array<Id,2> capacity; };
struct ProxyPlan { vector<int> core, order; };
struct SpillEvent { int tensor, core, previous, next, trigger; bool writeback; };
struct SpillEstimate {
    vector<SpillEvent> events;
    double bytes = 0;
    std::array<double, 2> peak{};
    bool infeasible = false;
    // Legal under the new evaluator; only a hint for optional order improvement.
    vector<int> partial_write_victims;
};
inline SpillEstimate estimate_spills(const ProxyIndex &g, const ProxyConfig &cfg, const ProxyPlan &s, int k,
                                     bool collect_events) {
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
        vector<int> next(nt, NEVER), last_writer(nt, -1);
        for (int i = 0; i < m; ++i)
            for (int t : g.ops[order[c][i]].outputs)
                last_writer[t] = i;
        for (int i = m - 1; i >= 0; --i) {
            size_t at = offsets[i];
            for (int t : g.ops[order[c][i]].spill_uses) {
                uses[at++] = {t, next[t]};
                next[t] = i;
            }
        }
        vector<unsigned char> resident(nt), backed(nt);
        vector<int> nextuse(nt, NEVER), protect(nt, -1), insertion(nt), previous(nt, -1);
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
                    backed[t] = last_writer[t] < 0; // boundary COPY_IN backing
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
                    if (g.tensors[t].producers.size() > 1 && last_writer[t] >= i)
                        ans.partial_write_victims.push_back(t);
                    if (collect_events)
                        ans.events.push_back({t,c,previous[t],order[c][n],order[c][i],!backed[t]});
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
                ans.peak[mem] = max(ans.peak[mem], double(used[mem]));
            }
            // Later writers modify the reloaded incarnation and invalidate DDR.
            for (int t : g.ops[order[c][i]].outputs)
                backed[t] = 0;
            for (auto it = begin; it != end; ++it) {
                auto [t, nxt] = *it;
                previous[t] = order[c][i];
                if (nxt == NEVER) {
                    resident[t] = 0;
                    used[g.tensors[t].pos] -= g.tensors[t].size;
                }
            }
        }
    }
    sort(ans.partial_write_victims.begin(), ans.partial_write_victims.end());
    ans.partial_write_victims.erase(unique(ans.partial_write_victims.begin(), ans.partial_write_victims.end()), ans.partial_write_victims.end());
    return ans;
}

bool valid_screening_proxy(const string &mode) {
    return mode == "legacy" || mode == "new90" || mode == "half" ||
           mode == "slack" || mode == "events";
}
double ProxyMetrics::score(const string &mode, double bandwidth) const {
    if (!valid_screening_proxy(mode)) throw invalid_argument("unknown screening proxy: " + mode);
    const double d = (route + spill) / bandwidth, q = spill / bandwidth;
    if (mode == "legacy") return max(sketch, route / bandwidth);
    double value = 0;
    if (mode == "new90") value = max(sketch,d) + q;
    if (mode == "half") value = max(sketch + .5*q,d);
    if (mode == "slack") value = max({sketch,d,pipe_bound+q});
    if (mode == "events") value = max(event_sketch,d);
    return value + .03*d + (memory_warning ? 1e18 : 0);
}
Json ProxyMetrics::to_json() const {
    return {{"sketch",sketch},{"route_bytes",route},{"spill_bytes",spill},
            {"pipe_bound",pipe_bound},{"event_sketch",event_sketch},{"memory_warning",memory_warning}};
}
ScreeningProxy::ScreeningProxy(const GraphInfo &graph)
    : graph_(&graph), index_(make_shared<ProxyIndex>(graph)) {}
ProxyMetrics ScreeningProxy::evaluate(const Plan &plan, const Settings &settings, bool events) const {
    const auto &g = *graph_;
    const auto &idx = *index_;
    ProxyPlan s;
    s.core.resize(idx.ops.size(),-1);
    for (Id block : plan.order)
        for (Id v : plan.blocks.at(block)) {
            int op = idx.op_index.at(v);
            s.core[op] = int(plan.core_of.at(block));
            s.order.push_back(op);
        }
    ProxyConfig config{{settings.capacity.at("L1"),settings.capacity.at("UB")}};
    auto spill = estimate_spills(idx,config,s,int(plan.ncores),events);
    ProxyMetrics result;
    result.spill = spill.bytes;
    result.memory_warning = spill.infeasible;
    result.route = double(route_bytes(g,plan));
    result.sketch = replay_sketch(g,plan,settings);
    for (const auto &row : pipe_loads(g,plan))
        for (const auto &[pipe,load] : row) result.pipe_bound = max(result.pipe_bound,load);
    result.event_sketch = result.sketch;
    if (!events || spill.events.empty()) return result;
    vector<vector<int>> after(idx.ops.size()), before(idx.ops.size()), release(idx.ops.size());
    for (int i = 0; i < int(spill.events.size()); ++i) {
        const auto &e = spill.events[i];
        after[e.previous].push_back(i);
        before[e.next].push_back(i);
        release[e.trigger].push_back(i);
    }
    vector<double> completed(spill.events.size());
    Sketch timeline(g,plan.ncores,settings);
    for (int op : s.order) {
        poll_search_deadline();
        Id v = idx.ops[op].id, core = s.core[op];
        double ready = 0;
        for (int i : release[op]) ready = max(ready,completed[i]);
        for (int i : before[op]) {
            const auto &e = spill.events[i];
            double &pipe = timeline.available[e.core]["PIPE_MTE2"];
            pipe = max(pipe,completed[i]) + copy_duration(idx.tensors[e.tensor].size,settings.bandwidth);
            ready = max(ready,pipe);
        }
        const auto pipe_name = field_string(g.ops.at(v),"pipe");
        double &compute_pipe = timeline.available[core][pipe_name];
        compute_pipe = max(compute_pipe,ready);
        timeline.append({v},core);
        for (int i : after[op]) {
            const auto &e = spill.events[i];
            double end = timeline.finish.at(v);
            if (e.writeback) {
                double &pipe = timeline.available[e.core]["PIPE_MTE3"];
                pipe = max(pipe,end) + copy_duration(idx.tensors[e.tensor].size,settings.bandwidth);
                end = pipe;
            }
            completed[i] = end;
        }
    }
    result.event_sketch = timeline.makespan();
    return result;
}
} // namespace problem2
