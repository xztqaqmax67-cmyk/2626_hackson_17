// 链剖分初解：复用 GraphInfo 的解析结果，使用紧凑索引生成确定性的候选。
// 时间/内存草图改编自本仓库 dag_chain_solver；不是场景 B 完整评估器。
#include "chain.hpp"
#include "structure.hpp"
#include <unordered_set>

namespace problem2 {
namespace {
using namespace std;
using i64 = Id;
using Row = std::array<double, 5>;
constexpr double INF = 1e100;
struct Config {
    std::array<i64, 2> capacity;
    double bandwidth, delay;
    explicit Config(const Settings &s)
        : capacity{{s.capacity.at("L1"), s.capacity.at("UB")}},
          bandwidth(s.bandwidth), delay(s.cross_core_copy_delay) {}
};
struct Deadline {
    double limit;
    mutable size_t visits = 0;
    void check() const { check_deadline(limit); poll_search_deadline(); }
    void tick() const {
        if ((++visits & 255U) == 0) check();
    }
};
struct Op {
    i64 id = 0, cycles = 0;
    int pipe = 0;
    string name;
    vector<int> inputs, outputs, pred, succ, direct_in, direct_inc;
};
struct Tensor {
    i64 id = 0, size = 0;
    int pos = 0;
    vector<int> producers, consumers;
    bool final_output = false;
};
struct Direct {
    int a, b;
    i64 size;
};
struct DSU {
    vector<int> p, sz;
    explicit DSU(int n) : p(n), sz(n, 1) { iota(p.begin(), p.end(), 0); }
    int get(int x) {
        while (x != p[x])
            x = p[x] = p[p[x]];
        return x;
    }
    void join(int a, int b) {
        a = get(a);
        b = get(b);
        if (a == b)
            return;
        if (sz[a] < sz[b])
            std::swap(a, b);
        p[b] = a;
        sz[a] += sz[b];
    }
};
// 每次初解族只建立一次索引；保留 GraphInfo 全部依赖和输入张量顺序。
struct Graph {
    vector<Op> ops;
    vector<Tensor> tensors;
    vector<Direct> direct;
    vector<int> topo, dfs_order, dfs_pos, writer_group;
    vector<double> rank, depth;
    map<pair<int, int>, double> edge_bytes;
    size_t edge_count = 0;
    static void unique_vec(vector<int> &v) {
        sort(v.begin(), v.end());
        v.erase(unique(v.begin(), v.end()), v.end());
    }
    explicit Graph(const GraphInfo &g, Deadline &deadline) {
        deadline.check();
        if (g.ids.size() > size_t(numeric_limits<int>::max()) ||
            g.tensors.size() > size_t(numeric_limits<int>::max()))
            throw length_error("chain index exceeds supported size");
        map<Id, int> oid, tid;
        for (Id id : g.ids) {
            deadline.tick();
            oid[id] = int(ops.size());
            Op op;
            op.id = id;
            op.cycles = g.weight.at(id);
            const auto name = field_string(g.ops.at(id), "pipe");
            op.pipe = int(find(PIPES.begin(), PIPES.end(), name) - PIPES.begin());
            if (op.pipe == int(PIPES.size())) throw invalid_argument("unknown pipe");
            ops.push_back(move(op));
        }
        for (const auto &[id, record] : g.tensors) {
            deadline.tick();
            tid[id] = int(tensors.size());
            Tensor tensor;
            tensor.id = id;
            tensor.size = field_id(record, "size");
            tensor.pos = field_string(record, "pos") == "L1" ? 0 : 1;
            tensor.final_output = g.original_output_relations.count(id) != 0;
            for (Id v : g.eligible_producers.at(id)) {
                deadline.tick();
                tensor.producers.push_back(oid.at(v));
            }
            for (Id v : g.eligible_consumers.at(id)) {
                deadline.tick();
                tensor.consumers.push_back(oid.at(v));
            }
            tensors.push_back(move(tensor));
        }
        rank.resize(ops.size());
        depth.assign(ops.size(), 0);
        for (int v = 0; v < int(ops.size()); ++v) {
            if (v % 256 == 0) deadline.check();
            Id id = ops[v].id;
            for (Id t : g.op_inputs.at(id)) ops[v].inputs.push_back(tid.at(t));
            for (Id t : g.op_outputs.at(id)) ops[v].outputs.push_back(tid.at(t));
            for (Id p : g.preds.at(id)) ops[v].pred.push_back(oid.at(p));
            for (Id w : g.succs.at(id)) ops[v].succ.push_back(oid.at(w));
            edge_count += ops[v].succ.size();
            rank[v] = double(g.rank.at(id));
            if (g.direct_inputs.count(id)) for (auto [unused, p, size] : g.direct_inputs.at(id)) {
                (void)unused;
                int a = oid.at(p), d = int(direct.size());
                direct.push_back({a, v, size});
                ops[v].direct_in.push_back(d);
                ops[a].direct_inc.push_back(d);
                ops[v].direct_inc.push_back(d);
            }
        }
        for (auto [edge, bytes] : g.edge_bytes) {
            deadline.tick();
            edge_bytes[{oid.at(edge.first), oid.at(edge.second)}] = double(bytes);
        }
        for (Id v : g.id_order) {
            deadline.tick();
            topo.push_back(oid.at(v));
        }
        for (int v : topo) for (int w : ops[v].succ) {
            deadline.tick();
            depth[w] = max(depth[w], depth[v] + weight(v));
        }
        deadline.check();
        for (Id v : reverse_dfs_order(g)) dfs_order.push_back(oid.at(v));
        deadline.check();
        dfs_pos.resize(ops.size());
        for (int i = 0; i < int(dfs_order.size()); ++i) dfs_pos[dfs_order[i]] = i;
        DSU writers(int(ops.size()));
        for (const auto &tensor : tensors)
            for (size_t j = 1; j < tensor.producers.size(); ++j) {
                deadline.tick();
                writers.join(tensor.producers[0], tensor.producers[j]);
            }
        for (int v = 0; v < int(ops.size()); ++v) {
            deadline.tick();
            writer_group.push_back(writers.get(v));
        }
        deadline.check();
    }
    int size() const { return int(ops.size()); }
    double weight(int v) const { return max(1.0, double(ops[v].cycles)); }
    vector<int> normalize_writers(vector<int> a) const {
        vector<int> chosen(size(), -1);
        for (int v : topo) {
            int group = writer_group[v];
            if (chosen[group] < 0) chosen[group] = a[v];
            a[v] = chosen[group];
        }
        return a;
    }
};
struct Solution {
    vector<int> core, order;
    string label;
};
inline void validate(const Graph &g, const Solution &s, int k) {
    int n = g.size();
    if ((int)s.core.size() != n || (int)s.order.size() != n)
        throw runtime_error("internal incomplete solution");
    vector<int> pos(n, -1);
    for (int i = 0; i < n; ++i) {
        int v = s.order[i];
        if (v < 0 || v >= n || pos[v] >= 0)
            throw runtime_error("internal repeated op");
        pos[v] = i;
    }
    for (int v = 0; v < n; ++v) {
        if (s.core[v] < 0 || s.core[v] >= k)
            throw runtime_error("internal bad core");
        for (int w : g.ops[v].succ)
            if (pos[v] >= pos[w])
                throw runtime_error("internal non-topological order");
    }
}
struct RouteKey {
    int type, t, src, dst;
    bool operator==(const RouteKey &o) const {
        return tie(type, t, src, dst) == tie(o.type, o.t, o.src, o.dst);
    }
};
struct RouteHash {
    size_t operator()(const RouteKey &x) const {
        uint64_t h = uint32_t(x.t);
        h = (h * 1000003) ^ uint32_t(x.src + 1);
        h = (h * 1000003) ^ uint32_t(x.dst + 1);
        return size_t((h * 17) ^ x.type);
    }
};
struct Proposal {
    int v = -1, core = -1;
    double start = 0, end = 0, bytes = 0;
    vector<pair<int, Row>> rows;
    vector<pair<RouteKey, double>> routes;
};
struct Placement {
    const Graph &g;
    const Config &cfg;
    int k;
    vector<Row> available, fixed;
    vector<double> finish;
    vector<int> core;
    unordered_map<RouteKey, double, RouteHash> route_end;
    double bytes = 0;
    Placement(const Graph &gg, int kk, const Config &c)
        : g(gg), cfg(c), k(kk), available(kk), fixed(kk), finish(g.size(), -1), core(g.size(), -1) {
        route_end.reserve(g.tensors.size() * 2 + 1);
    }
    Proposal trial(int v, int target) const {
        Proposal out;
        out.v = v;
        out.core = target;
        out.rows.reserve(4);
        out.routes.reserve(8);
        auto row = [&](int c) -> Row & {
            for (auto &z : out.rows)
                if (z.first == c)
                    return z.second;
            out.rows.emplace_back(c, available[c]);
            return out.rows.back().second;
        };
        auto copy = [&](RouteKey key, int c, int pipe, i64 size, double release) {
            auto it = route_end.find(key);
            if (it != route_end.end())
                return it->second;
            for (const auto &z : out.routes)
                if (z.first == key)
                    return z.second;
            Row &r = row(c);
            double end = max(r[pipe], release) + max(1.0, ceil(double(size) / cfg.bandwidth));
            r[pipe] = end;
            out.routes.emplace_back(key, end);
            out.bytes += double(size);
            return end;
        };
        auto transfer = [&](int type, int tid, int source, i64 size, double release) {
            double a = copy({type, tid, source, target}, source, 1, size, release);
            return copy({type + 1, tid, source, target}, target, 0, size, a + cfg.delay);
        };
        double ready = 0;
        for (int p : g.ops[v].pred)
            ready = max(ready, finish[p]);
        for (int t : g.ops[v].inputs) {
            const Tensor &x = g.tensors[t];
            if (x.producers.empty()) {
                ready = max(ready, copy({0, t, -1, target}, target, 0, x.size, 0));
                continue;
            }
            vector<pair<int, double>> release;
            for (int p : x.producers) {
                int src = core[p];
                if (src < 0)
                    throw runtime_error("internal unscheduled producer");
                auto it =
                    find_if(release.begin(), release.end(), [&](auto z) { return z.first == src; });
                if (it == release.end())
                    release.emplace_back(src, finish[p]);
                else
                    it->second = max(it->second, finish[p]);
            }
            for (auto [src, r] : release)
                ready = max(ready, src == target ? r : transfer(1, t, src, x.size, r));
        }
        for (int d : g.ops[v].direct_in) {
            const auto &e = g.direct[d];
            if (core[e.a] != target)
                ready = max(ready, transfer(4, d, core[e.a], e.size, finish[e.a]));
        }
        int pipe = g.ops[v].pipe;
        out.start = max(ready, row(target)[pipe]);
        out.end = out.start + g.weight(v);
        row(target)[pipe] = out.end;
        for (int t : g.ops[v].outputs) {
            const auto &x = g.tensors[t];
            if (!x.final_output)
                continue;
            bool complete = true;
            for (int p : x.producers)
                if (p != v && finish[p] < 0)
                    complete = false;
            if (!complete)
                continue;
            vector<pair<int, double>> release;
            for (int p : x.producers) {
                int src = p == v ? target : core[p];
                double r = p == v ? out.end : finish[p];
                auto it =
                    find_if(release.begin(), release.end(), [&](auto z) { return z.first == src; });
                if (it == release.end())
                    release.emplace_back(src, r);
                else
                    it->second = max(it->second, r);
            }
            for (auto [src, r] : release)
                copy({1, t, src, -1}, src, 1, x.size, r);
        }
        return out;
    }
    void commit(const Proposal &p) {
        for (const auto &z : p.rows)
            available[z.first] = z.second;
        for (const auto &z : p.routes)
            route_end[z.first] = z.second;
        finish[p.v] = p.end;
        core[p.v] = p.core;
        fixed[p.core][g.ops[p.v].pipe] += double(g.ops[p.v].cycles);
        bytes += p.bytes;
    }
    double makespan() const {
        double ans = 0;
        for (const Row &r : available)
            for (double x : r)
                ans = max(ans, x);
        return ans;
    }
};

// Serial tensor-lifetime sketch for ORDERING ONLY (not official spill simulation).
struct MemoryOrder {
    const Graph &g;
    const Config &cfg;
    int k;
    vector<int> remaining;
    vector<unsigned char> live;
    vector<std::array<double, 2>> used;
    MemoryOrder(const Graph &gg, const Config &cc, int kk, const vector<int> &a)
        : g(gg), cfg(cc), k(kk), remaining(g.tensors.size() * size_t(k)), live(remaining.size()),
          used(k) {
        for (int t = 0; t < (int)g.tensors.size(); ++t) {
            vector<int> uses = g.tensors[t].consumers;
            uses.insert(uses.end(), g.tensors[t].producers.begin(), g.tensors[t].producers.end());
            Graph::unique_vec(uses);
            for (int v : uses)
                ++remaining[size_t(t) * k + a[v]];
        }
    }
    double price(int v, int c) const {
        std::array<double, 2> add{}, release{};
        vector<int> all = g.ops[v].inputs;
        for (int t : g.ops[v].outputs)
            if (find(all.begin(), all.end(), t) == all.end())
                all.push_back(t);
        for (int t : all) {
            size_t key = size_t(t) * k + c;
            if (!live[key])
                add[g.tensors[t].pos] += double(g.tensors[t].size);
        }
        for (int t : all)
            if (remaining[size_t(t) * k + c] == 1)
                release[g.tensors[t].pos] += double(g.tensors[t].size);
        double value = 0;
        for (int m = 0; m < 2; ++m) {
            double cap = double(cfg.capacity[m]);
            value += add[m] - release[m] + 4 * max(0.0, used[c][m] + add[m] - .90 * cap);
        }
        return value / cfg.bandwidth;
    }
    void commit(int v, int c) {
        vector<int> all = g.ops[v].inputs;
        for (int t : g.ops[v].outputs)
            if (find(all.begin(), all.end(), t) == all.end())
                all.push_back(t);
        for (int t : all) {
            size_t key = size_t(t) * k + c;
            if (!live[key]) {
                live[key] = 1;
                used[c][g.tensors[t].pos] += double(g.tensors[t].size);
            }
        }
        for (int t : all)
            --remaining[size_t(t) * k + c];
        for (int t : all) {
            size_t key = size_t(t) * k + c;
            if (!remaining[key] && live[key]) {
                live[key] = 0;
                used[c][g.tensors[t].pos] -= double(g.tensors[t].size);
            }
        }
    }
};
struct SeedOptions {
    string mode = "critical";
    double balance = 0, communication = 0, memory = 0;
    int window = 1;
};
inline Solution operation_plan(const Graph &g, int k, const Config &cfg, const SeedOptions &opt,
                               const vector<int> *assignment, Deadline *deadline,
                               const vector<int> *groups = nullptr) {
    vector<int> normalized;
    if (assignment) {
        normalized = g.normalize_writers(*assignment);
        assignment = &normalized;
    }
    Placement sketch(g, k, cfg);
    int n = g.size();
    const auto &group_of = groups ? *groups : g.writer_group;
    vector<int> group_core(n, -1);
    vector<pair<double, double>> priority(n);
    for (int v = 0; v < n; ++v) {
        if (opt.mode == "breadth")
            priority[v] = {g.depth[v], -g.rank[v]};
        else if (opt.mode == "id")
            priority[v] = {double(v), 0};
        else if (opt.mode == "dfs")
            priority[v] = {double(g.dfs_pos[v]), 0};
        else
            priority[v] = {-g.rank[v], 0};
    }
    auto cmp = [&](int a, int b) {
        return make_tuple(priority[a], a) > make_tuple(priority[b], b);
    };
    priority_queue<int, vector<int>, decltype(cmp)> heap(cmp);
    vector<int> degree(n);
    for (int v = 0; v < n; ++v) {
        degree[v] = int(g.ops[v].pred.size());
        if (!degree[v])
            heap.push(v);
    }
    // Avoid constructing a memory model when ownership is not fixed.
    unique_ptr<MemoryOrder> mem;
    if (assignment && opt.memory > 0)
        mem = make_unique<MemoryOrder>(g, cfg, k, *assignment);
    Solution sol;
    sol.core.assign(n, -1);
    sol.order.reserve(n);
    vector<int> candidates;
    while (!heap.empty()) {
        if (deadline && (sol.order.size() % 128 == 0))
            deadline->check();
        candidates.clear();
        for (int i = 0; i < opt.window && !heap.empty(); ++i) {
            candidates.push_back(heap.top());
            heap.pop();
        }
        Proposal chosen;
        tuple<double, pair<double, double>, int> chosen_key{INF, {INF, INF}, 0};
        for (int v : candidates) {
            Proposal best;
            tuple<double, double, double, int> bestkey{INF, INF, INF, 0};
            int p = g.ops[v].pipe;
            for (int c = 0; c < k; ++c) {
                if (assignment && c != (*assignment)[v])
                    continue;
                if (group_core[group_of[v]] >= 0 && c != group_core[group_of[v]])
                    continue;
                Proposal t = sketch.trial(v, c);
                double load = 0;
                for (int j = 0; j < 5; ++j)
                    load = max(load, sketch.fixed[c][j] + (j == p ? g.weight(v) : 0));
                auto key = make_tuple(t.end + opt.balance * load +
                                          opt.communication * t.bytes / cfg.bandwidth,
                                      t.bytes, load, c);
                if (key < bestkey) {
                    bestkey = key;
                    best = move(t);
                }
            }
            double order_score = best.start;
            if (mem)
                order_score += opt.memory * mem->price(v, best.core);
            auto key = make_tuple(order_score, priority[v], v);
            if (key < chosen_key) {
                chosen_key = key;
                chosen = move(best);
            }
        }
        int v = chosen.v;
        for (int x : candidates)
            if (x != v)
                heap.push(x);
        sketch.commit(chosen);
        if (mem)
            mem->commit(v, chosen.core);
        sol.core[v] = chosen.core;
        group_core[group_of[v]] = chosen.core;
        sol.order.push_back(v);
        for (int w : g.ops[v].succ)
            if (!--degree[w])
                heap.push(w);
    }
    validate(g, sol, k);
    return sol;
}

struct TrafficState {
    const Graph &g;
    const Config &cfg;
    int k;
    vector<int> core, pc, cc;
    vector<Row> load;
    double bytes = 0;
    TrafficState(const Graph &gg, const Config &cf, int kk, const vector<int> &a)
        : g(gg), cfg(cf), k(kk), core(a), pc(g.tensors.size() * size_t(k)), cc(pc.size()), load(k) {
        for (int v = 0; v < g.size(); ++v) {
            for (int t : g.ops[v].inputs)
                ++cc[size_t(t) * k + core[v]];
            for (int t : g.ops[v].outputs)
                ++pc[size_t(t) * k + core[v]];
            load[core[v]][g.ops[v].pipe] += double(g.ops[v].cycles);
        }
        for (int t = 0; t < (int)g.tensors.size(); ++t)
            bytes += tensor_bytes(t);
        for (const auto &e : g.direct)
            if (core[e.a] != core[e.b])
                bytes += 2 * double(e.size);
    }
    double tensor_bytes(int t) const {
        int np = 0, nc = 0, intersection = 0;
        for (int c = 0; c < k; ++c) {
            bool p = pc[size_t(t) * k + c] > 0, q = cc[size_t(t) * k + c] > 0;
            np += p;
            nc += q;
            intersection += p && q;
        }
        double copies = np ? 2.0 * (double(np) * nc - intersection) : double(nc);
        if (g.tensors[t].final_output)
            copies += np;
        return copies * double(g.tensors[t].size);
    }
    void move_op(int v, int dest) {
        int src = core[v];
        if (src == dest)
            return;
        vector<int> touched = g.ops[v].inputs;
        touched.insert(touched.end(), g.ops[v].outputs.begin(), g.ops[v].outputs.end());
        Graph::unique_vec(touched);
        for (int t : touched)
            bytes -= tensor_bytes(t);
        for (int d : g.ops[v].direct_inc) {
            const auto &e = g.direct[d];
            if (core[e.a] != core[e.b])
                bytes -= 2 * double(e.size);
        }
        for (int t : g.ops[v].inputs) {
            --cc[size_t(t) * k + src];
            ++cc[size_t(t) * k + dest];
        }
        for (int t : g.ops[v].outputs) {
            --pc[size_t(t) * k + src];
            ++pc[size_t(t) * k + dest];
        }
        load[src][g.ops[v].pipe] -= double(g.ops[v].cycles);
        load[dest][g.ops[v].pipe] += double(g.ops[v].cycles);
        core[v] = dest;
        for (int t : touched)
            bytes += tensor_bytes(t);
        for (int d : g.ops[v].direct_inc) {
            const auto &e = g.direct[d];
            if (core[e.a] != core[e.b])
                bytes += 2 * double(e.size);
        }
    }
    double pipe_bound() const {
        double r = 0;
        for (const auto &l : load)
            for (double x : l)
                r = max(r, x);
        return r;
    }
    double light_score(double alpha) const {
        return max(pipe_bound(), bytes / cfg.bandwidth) + alpha * bytes / cfg.bandwidth;
    }
};

// Bounded-memory reference-string model using farthest-next-use victims.
// This is NOT a replacement for official Step 2: async COPY lifetimes and memory
// reuse edges are not simulated. The estimated spill bytes can differ substantially.
struct SpillEstimate {
    double bytes = 0;
    std::array<double, 2> peak{};
    bool infeasible = false;
};
inline SpillEstimate estimate_spills(const Graph &g, const Config &cfg, const Solution &s, int k,
                                     Deadline *deadline) {
    const int nt = int(g.tensors.size()), NEVER = numeric_limits<int>::max();
    SpillEstimate ans;
    vector<vector<int>> order(k);
    for (int v : s.order)
        order[s.core[v]].push_back(v);
    struct Event {
        int v;
        vector<pair<int, int>> uses;
    };
    for (int c = 0; c < k; ++c) {
        if (deadline)
            deadline->check();
        if (order[c].empty())
            continue;
        int m = int(order[c].size());
        vector<Event> ev(m);
        vector<int> next(nt, NEVER), last_writer(nt, -1);
        for (int i = 0; i < m; ++i)
            for (int t : g.ops[order[c][i]].outputs)
                last_writer[t] = i;
        for (int i = m - 1; i >= 0; --i) {
            int v = order[c][i];
            ev[i].v = v;
            vector<int> tids = g.ops[v].inputs;
            tids.insert(tids.end(), g.ops[v].outputs.begin(), g.ops[v].outputs.end());
            Graph::unique_vec(tids);
            for (int t : tids) {
                ev[i].uses.emplace_back(t, next[t]);
                next[t] = i;
            }
        }
        vector<unsigned char> resident(nt), seen(nt), backed(nt);
        vector<int> nextuse(nt, NEVER), protect(nt, -1);
        using Item = pair<int, int>;
        std::array<priority_queue<Item>, 2> heap;
        std::array<double, 2> used{};
        for (int i = 0; i < m; ++i) {
            if (deadline && (i % 512 == 0))
                deadline->check();
            const Event &e = ev[i];
            for (auto [t, nxt] : e.uses)
                protect[t] = i;
            for (auto [t, nxt] : e.uses) {
                (void)nxt;
                if (resident[t])
                    continue;
                const Tensor &x = g.tensors[t];
                int mem = x.pos;
                double sz = double(x.size);
                vector<Item> held;
                while (used[mem] + sz > double(cfg.capacity[mem]) + 0.01) {
                    while (!heap[mem].empty()) {
                        auto [a, b] = heap[mem].top();
                        if (!resident[b] || nextuse[b] != a) {
                            heap[mem].pop();
                            continue;
                        }
                        if (protect[b] == i) {
                            held.push_back(heap[mem].top());
                            heap[mem].pop();
                            continue;
                        }
                        break;
                    }
                    if (heap[mem].empty()) {
                        ans.infeasible = true;
                        break;
                    }
                    auto [n, b] = heap[mem].top();
                    heap[mem].pop();
                    // The reference Step 2 rewires readers on reload. Spilling
                    // between partial writes can leave COPY_OUT waiting on a
                    // future writer and introduce a dependency/FIFO cycle.
                    // Reject this ordering; do not change the external rules.
                    if (g.tensors[b].producers.size() > 1 && last_writer[b] >= i)
                        ans.infeasible = true;
                    resident[b] = 0;
                    used[mem] -= double(g.tensors[b].size);
                    if (n != NEVER && !backed[b]) {
                        ans.bytes += double(g.tensors[b].size);
                        backed[b] = 1;
                    }
                }
                for (auto xh : held)
                    heap[mem].push(xh);
                if (seen[t])
                    ans.bytes += sz;
                else {
                    bool local = false;
                    for (int p : x.producers)
                        if (s.core[p] == c)
                            local = true;
                    backed[t] = !local;
                }
                resident[t] = seen[t] = 1;
                used[mem] += sz;
                ans.peak[mem] = max(ans.peak[mem], used[mem]);
            }
            for (int t : g.ops[e.v].outputs)
                backed[t] = 0;
            for (auto [t, nxt] : e.uses) {
                nextuse[t] = nxt;
                int mem = g.tensors[t].pos;
                if (nxt == NEVER) {
                    if (resident[t]) {
                        resident[t] = 0;
                        used[mem] -= double(g.tensors[t].size);
                    }
                } else
                    heap[mem].push({nxt, t});
            }
        }
    }
    return ans;
}
struct Metrics {
    double sketch = 0, route = 0, spill = 0, predicted = 0, score = 0, pipe_bound = 0;
    std::array<double, 2> peak{};
    bool memory_warning = false;
};
inline Metrics evaluate_proxy(const Graph &g, int k, const Config &cfg, const Solution &s,
                              double alpha, Deadline *deadline) {
    validate(g, s, k);
    Metrics m;
    Placement p(g, k, cfg);
    for (size_t i = 0; i < s.order.size(); ++i) {
        if (deadline && (i % 256 == 0))
            deadline->check();
        int v = s.order[i];
        p.commit(p.trial(v, s.core[v]));
    }
    m.sketch = p.makespan();
    TrafficState tc(g, cfg, k, s.core);
    m.route = tc.bytes;
    m.pipe_bound = tc.pipe_bound();
    auto sp = estimate_spills(g, cfg, s, k, deadline);
    m.spill = sp.bytes;
    m.peak = sp.peak;
    m.memory_warning = sp.infeasible;
    m.predicted = max(m.sketch, (m.route + m.spill) / cfg.bandwidth) + m.spill / cfg.bandwidth;
    m.score =
        m.predicted + alpha * (m.route + m.spill) / cfg.bandwidth + (sp.infeasible ? 1e18 : 0);
    return m;
}

struct Decomposition {
    vector<int> parent, next, bundle_of;
    vector<vector<int>> paths, bundles;
};
Decomposition decompose(const Graph &g, int k, double scale, double communication_weight,
                        bool fill_unmatched, const Config &cfg, Deadline &deadline,
                        bool cheap_cuts = false) {
    deadline.check();
    Decomposition d;
    int n = g.size();
    d.parent.assign(n, -1);
    d.next.assign(n, -1);
    auto transfer = [&](int u, int v) {
        auto it = g.edge_bytes.find({u, v});
        return cfg.delay + (it == g.edge_bytes.end() ? 0 : 2.0 * it->second / cfg.bandwidth);
    };
    vector<double> finish(n);
    for (int v : g.topo) {
        deadline.tick();
        tuple<double, double, int> best{-1, -1, 0};
        double arrival = 0;
        for (int u : g.ops[v].pred) {
            deadline.tick();
            arrival = max(arrival, finish[u]);
            auto key = make_tuple(finish[u] + communication_weight * transfer(u,v),
                                  transfer(u,v), -u);
            if (key > best) { best = key; d.parent[v] = u; }
        }
        // 通信偏好可能选中非关键前驱；累积完成时间必须仍取全部前驱最大值。
        finish[v] = arrival + g.weight(v);
    }
    for (int v : g.topo) if (d.parent[v] >= 0) {
        deadline.tick();
        int u = d.parent[v], old = d.next[u];
        auto key = [&](int w) {
            return make_pair(g.rank[w] + communication_weight * transfer(u,w), -w);
        };
        if (old < 0 || key(v) > key(old)) d.next[u] = v;
    }
    vector<int> incoming(n, -1);
    for (int u = 0; u < n; ++u) if (d.next[u] >= 0) incoming[d.next[u]] = u;
    // 加权贪心补全未匹配的出/入端点，避免扇出冲突后丢掉第二优连链机会。
    // 每个点仍至多一条链入边、一条链出边；只用原 DAG 边，因此不能成环。
    if (fill_unmatched) {
        vector<tuple<double, double, int, int>> edges;
        for (int u : g.topo) if (d.next[u] < 0) {
            deadline.tick();
            for (int v : g.ops[u].succ) if (incoming[v] < 0) {
                deadline.tick();
                edges.emplace_back(-(transfer(u,v) + communication_weight * (finish[u] + g.rank[v])),
                                   -g.rank[v], u, v);
            }
        }
        deadline.check();
        sort(edges.begin(), edges.end());
        deadline.check();
        for (auto [cost, rank, u, v] : edges) {
            deadline.tick();
            (void)cost; (void)rank;
            if (d.next[u] < 0 && incoming[v] < 0) { d.next[u] = v; incoming[v] = u; }
        }
    }
    Row total{};
    for (int v : g.topo) total[g.ops[v].pipe] += g.weight(v);
    double target = max(1.0, *max_element(total.begin(), total.end()) / k * scale);
    DSU bundles(n);
    for (int root : g.topo) if (incoming[root] < 0) {
        deadline.check();
        vector<int> path;
        for (int v = root; v >= 0; v = d.next[v]) {
            deadline.tick();
            path.push_back(v);
        }
        const int path_size = int(path.size());
        for (int start = 0; start < path_size;) {
            Row load{};
            int end = start;
            while (end < path_size) {
                deadline.tick();
                int v = path[end], p = g.ops[v].pipe;
                if (end > start && load[p] + g.weight(v) > target) break;
                load[p] += g.weight(v);
                ++end;
            }
            if (cheap_cuts && end < path_size && end > start + 1) {
                // 在原负载切点附近选择通信较便宜的真实边；偏离过远/负载超标受罚。
                // 只考察常数窗口，避免把切点优化变成另一轮组合搜索。
                int left = max(start + 1, end - 4);
                int right = end + min(4,path_size - 1 - end), best = end;
                Row prefix{};
                double best_cost = INF;
                for (int cut = start + 1; cut <= right; ++cut) {
                    deadline.tick();
                    int v = path[cut - 1];
                    prefix[g.ops[v].pipe] += g.weight(v);
                    double peak = *max_element(prefix.begin(), prefix.end());
                    if (cut < left || peak > max(target * 1.25, g.weight(path[start]))) continue;
                    double cost = transfer(path[cut - 1], path[cut]) +
                                  .25 * abs(peak - target);
                    if (cost < best_cost) { best_cost = cost; best = cut; }
                }
                end = best;
            }
            if (end <= start || end > path_size) throw logic_error("invalid chain segment boundary");
            for (int i = start + 1; i < end; ++i) {
                deadline.tick();
                bundles.join(path[i - 1],path[i]);
            }
            start = end;
        }
        d.paths.push_back(move(path));
    }
    // 所有部分写入者共同分核。它可能合并不同链段，但不会压缩调度 DAG。
    for (int v = 0; v < n; ++v) {
        deadline.tick();
        bundles.join(v,g.writer_group[v]);
    }
    map<int,int> index;
    d.bundle_of.resize(n);
    for (int v : g.topo) {
        deadline.tick();
        int root = bundles.get(v);
        if (!index.count(root)) { index[root] = int(d.bundles.size()); d.bundles.push_back({}); }
        int b = index.at(root);
        d.bundle_of[v] = b;
        d.bundles[b].push_back(v);
    }
    return d;
}
struct Pins {
    int tensor;
    bool producer, consumer;
};

// Bundles are placement preferences, not contracted DAG nodes. A bundle quotient
// may contain cycles even when the original graph does not; we never schedule it.
vector<int> pack(const Graph &g, const Decomposition &d, int k, double communication,
                 const Config &cfg, Deadline &deadline) {
    deadline.check();
    const int count = int(d.bundles.size());
    vector<Row> work(count), loads(k);
    Row total{};
    vector<double> critical(count);
    vector<vector<Pins>> pins(count);
    for (int b = 0; b < count; ++b) {
        deadline.tick();
        map<int, pair<bool, bool>> touched;
        for (int v : d.bundles[b]) {
            deadline.tick();
            work[b][g.ops[v].pipe] += g.weight(v);
            total[g.ops[v].pipe] += g.weight(v);
            critical[b] = max(critical[b], g.rank[v]);
            for (int t : g.ops[v].inputs)
                touched[t].second = true;
            for (int t : g.ops[v].outputs)
                touched[t].first = true;
        }
        for (auto [t, flags] : touched)
            pins[b].push_back({t, flags.first, flags.second});
    }
    vector<int> ordering(count);
    iota(ordering.begin(), ordering.end(), 0);
    auto priority = [&](int b) {
        double load = 0;
        for (int p = 0; p < 5; ++p)
            load = max(load, work[b][p] / max(1.0, total[p] / k));
        return make_tuple(-load, -critical[b], b);
    };
    deadline.check();
    sort(ordering.begin(), ordering.end(), [&](int a, int b) { return priority(a) < priority(b); });
    deadline.check();
    vector<unsigned> producer(g.tensors.size()), consumer(g.tensors.size());
    auto pop = [](unsigned x) {
        int n = 0;
        while (x) {
            n += int(x & 1U);
            x >>= 1;
        }
        return n;
    };
    auto tensor_cost = [&](int t, unsigned p, unsigned c) {
        const auto &tensor = g.tensors[t];
        int np = pop(p), nc = pop(c), pairs = np * nc - pop(p & c);
        double copies = tensor.producers.empty() ? nc : 2.0 * pairs;
        if (tensor.final_output)
            copies += np;
        return copies * double(tensor.size) / cfg.bandwidth + pairs * cfg.delay;
    };
    vector<int> assignment(g.size(), -1);
    for (int b : ordering) {
        deadline.check();
        int best = 0;
        tuple<double, double, int> best_key{INF, INF, 0};
        for (int c = 0; c < k; ++c) {
            double balance = 0, traffic = 0, peak = 0;
            for (int p = 0; p < 5; ++p) {
                double w = work[b][p];
                balance += (2 * loads[c][p] * w + w * w) / max(1.0, total[p] / k);
                peak = max(peak, loads[c][p] + w);
            }
            for (const auto &pin : pins[b]) {
                deadline.tick();
                unsigned oldp = producer[pin.tensor], oldc = consumer[pin.tensor];
                traffic += tensor_cost(pin.tensor, oldp | (pin.producer ? (1U << c) : 0U),
                                       oldc | (pin.consumer ? (1U << c) : 0U)) -
                           tensor_cost(pin.tensor, oldp, oldc);
            }
            for (int v : d.bundles[b]) for (int di : g.ops[v].direct_inc) {
                deadline.tick();
                const auto &e = g.direct[di];
                int other = e.a == v ? e.b : e.a;
                if (assignment[other] >= 0 && assignment[other] != c)
                    traffic += 2.0 * e.size / cfg.bandwidth + cfg.delay;
            }
            auto key = make_tuple(balance + communication * traffic, peak, c);
            if (key < best_key) {
                best_key = key;
                best = c;
            }
        }
        for (int v : d.bundles[b])
            assignment[v] = best;
        for (int p = 0; p < 5; ++p)
            loads[best][p] += work[b][p];
        for (const auto &pin : pins[b]) {
            if (pin.producer)
                producer[pin.tensor] |= 1U << best;
            if (pin.consumer)
                consumer[pin.tensor] |= 1U << best;
        }
    }
    return assignment;
}

struct Entry {
    Solution solution;
    Metrics metrics;
};
Plan emit(const Graph &g, const Solution &s, Id cores) {
    Plan p;
    p.ncores = cores;
    for (int v : s.order) {
        Id id = g.ops[v].id;
        p.blocks[id] = {id};
        p.core_of[id] = s.core[v];
        p.order.push_back(id);
        p.next_block_id = max(p.next_block_id, id + 1);
    }
    return p;
}
} // namespace

ChainDecomposition chain_decompose(const GraphInfo &graph, Id cores, const Settings &settings,
                                    double segment_scale, double communication_weight,
                                    bool fill_unmatched, double deadline) {
    if (cores < 1 || cores > 5 || segment_scale <= 0) throw invalid_argument("invalid chain settings");
    Deadline timer{deadline};
    Graph g(graph, timer);
    auto d = decompose(g, int(cores), segment_scale, communication_weight, fill_unmatched,
                       Config(settings), timer);
    ChainDecomposition result;
    for (const auto &path : d.paths) {
        Ids ids;
        for (int v : path) ids.push_back(g.ops[v].id);
        result.paths.push_back(move(ids));
    }
    for (const auto &segment : d.bundles) {
        Ids ids;
        for (int v : segment) {
            Id id = g.ops[v].id;
            ids.push_back(id);
            result.segment_of[id] = Id(result.segments.size());
        }
        result.segments.push_back(move(ids));
    }
    return result;
}

NamedPlans chain_seed_plans(const GraphInfo &graph, Id cores, const Settings &settings,
                            double deadline, Id limit) {
    if (cores < 2 || cores > 5 || limit < 0) throw invalid_argument("invalid chain settings");
    Deadline timer{deadline};
    const Graph g(graph, timer);
    const Config cfg(settings);
    vector<Entry> pool;
    set<pair<vector<int>,vector<int>>> seen;
    auto offer = [&](Solution s) {
        timer.check();
        if (!seen.insert({s.core,s.order}).second) return;
        auto m = evaluate_proxy(g, int(cores), cfg, s, .03, &timer);
        if (!m.memory_warning) pool.push_back({move(s),m});
    };
    // 每个变体均完成链分解/分核/一次 ready 排序；不使用局部迁移或旧构造器。
    struct Variant { double scale, critical_comm, pack_comm; bool complete, cheap_cuts; };
    const vector<Variant> variants = {
        {.5,0,.5,false,false}, {1,0,1,false,false}, {.25,0,.25,false,false},
        {.5,.25,1,false,false}, {1,.25,2,false,false}, {.25,.25,.5,false,false},
        {.5,.25,1,true,true}, {1,.25,2,true,true}, {.125,.25,.5,true,true}
    };
    size_t graph_work = g.size() + g.edge_count + g.tensors.size();
    size_t count = graph_work > 200000 ? 3 : variants.size();
    try {
        for (size_t i = 0; i < count; ++i) {
            timer.check();
            const auto &v = variants[i];
            auto d = decompose(g,int(cores),v.scale,v.critical_comm,v.complete,cfg,timer,v.cheap_cuts);
            auto a = pack(g,d,int(cores),v.pack_comm,cfg,timer);
            for (const string &mode : {string("critical"),string("dfs")}) {
                SeedOptions opt;
                opt.mode = mode;
                opt.window = 8;
                opt.memory = .5;
                auto s = operation_plan(g,int(cores),cfg,opt,&a,&timer);
                s.label = "chain_" + to_string(i) + "_" + mode;
                offer(move(s));
            }
            if (i == 2) {
                // 严格 DFS 不从 ready 窗口提前开启其他分支，缩短中间张量的驻留区间。
                Solution s;
                s.core = a;
                s.order = g.dfs_order;
                s.label = "chain_2_strict_dfs";
                offer(move(s));
            }
        }
        if (graph_work <= 200000) {
            // HEFT 启发的链段动态映射：首个就绪节点以预计完成时间选核，后续同段固定。
            // 整段亲和约束始终存在；随后只按该次分核生成最终顺序，不做迁移搜索。
            for (size_t i = 0; i < 3; ++i) {
                const double scale = i == 0 ? .125 : i == 1 ? .5 : 1.0;
                auto d = decompose(g,int(cores),scale,.25,true,cfg,timer,true);
                SeedOptions mapping;
                mapping.mode = "critical";
                mapping.window = 8;
                mapping.balance = i == 2 ? .5 : .15;
                auto preliminary = operation_plan(g,int(cores),cfg,mapping,nullptr,&timer,&d.bundle_of);
                SeedOptions ordering;
                ordering.mode = i == 1 ? "critical" : "dfs";
                ordering.window = 8;
                ordering.memory = .5;
                auto s = operation_plan(g,int(cores),cfg,ordering,&preliminary.core,&timer);
                s.label = "chain_eft_" + to_string(i) + "_" + ordering.mode;
                offer(move(s));
                Solution strict;
                strict.core = preliminary.core;
                strict.order = g.dfs_order;
                strict.label = "chain_eft_" + to_string(i) + "_strict_dfs";
                offer(move(strict));
            }
        }
        // 链分核允许把全部链放在一个核上，提供通信密集/串行图的退化链候选。
        // 顺序仍由同一链初解排序器产生，而非引入旧求解器的单核方案。
        vector<int> together(g.size(),0);
        for (const string &mode : {string("dfs"),string("critical")}) {
            SeedOptions opt;
            opt.mode = mode; opt.window = 8; opt.memory = .5;
            auto s = operation_plan(g,int(cores),cfg,opt,&together,&timer);
            s.label = "chain_coalesced_" + mode;
            offer(move(s));
        }
    } catch (const TimeoutError &) {
        // 局部构造预算结束时保留已完成候选，交给外层在剩余总预算中评估。
        // 尚无完整候选则仍报告超时，不能返回半成品或悄悄混入其他初解。
        if (pool.empty()) throw;
    }
    stable_sort(pool.begin(),pool.end(),[](const Entry &a,const Entry &b) {
        // 带宽下界可能压平时间差；同分优先更短时间草图，最后才用固定编号。
        auto merit = [](const Entry &e) {
            return make_tuple(e.metrics.score,e.metrics.route+e.metrics.spill,e.metrics.sketch);
        };
        const auto left = merit(a), right = merit(b);
        if (left != right) return left < right;
        // 只在标量完全相同后引用比较数组，避免每次比较都复制整个图的分配/顺序。
        return tie(a.solution.core,a.solution.order) < tie(b.solution.core,b.solution.order);
    });
    NamedPlans available;
    set<string> outputs;
    for (const auto &entry : pool) {
        auto p = emit(g,entry.solution,cores);
        p.validate(graph);
        // 不同全局交错可能导出相同的各核 FIFO，按实际方案再次去重。
        if (!outputs.insert(p.fingerprint()).second) continue;
        available.emplace_back(entry.solution.label,move(p));
    }
    if (!limit || available.size() <= size_t(limit)) return available;
    // 正常搜索只接入两个链候选，保留内部排序前两名；强制不同族会挤掉好候选。
    if (limit < 6) {
        available.resize(size_t(limit));
        return available;
    }
    // 开发评测的较大候选池保留静态打包和链段 EFT 两种映射。
    set<size_t> chosen{0};
    auto reserve = [&](bool eft, size_t quota) {
        size_t kept = 0;
        for (size_t i = 0; i < available.size() && chosen.size() < size_t(limit); ++i) {
            bool match = available[i].first.rfind("chain_eft_",0) == 0;
            if (match != eft || available[i].first.rfind("chain_coalesced_",0) == 0) continue;
            chosen.insert(i);
            if (++kept >= quota) break;
        }
    };
    reserve(true,limit >= 6 ? 2 : 1);
    reserve(false,limit >= 6 ? 2 : 1);
    for (size_t i = 0; i < available.size() && chosen.size() < size_t(limit); ++i) chosen.insert(i);
    NamedPlans result;
    for (size_t i : chosen) result.push_back(move(available[i]));
    return result;
}
} // namespace problem2
