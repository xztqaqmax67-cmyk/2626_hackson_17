#include "anytime.hpp"
#include "parallel_jobs.hpp"
#include "search_limits.hpp"

namespace dag_chain {
namespace {
using Signature = pair<uint64_t, uint64_t>;
constexpr size_t family_count = 8;

// 去重用的固定雙雜湊，不是隨機搜尋。碰撞最多略過候選，絕不使非法候選通過。
// 只記錄固定大小的簽章，避免為大型圖保存數十萬份完整方案。
Signature signature(const Solution &s, int cores) {
    uint64_t a = 1469598103934665603ULL, b = 7809847782465536322ULL;
    auto feed = [&](uint64_t x) {
        a = (a ^ x) * 1099511628211ULL;
        b ^= x + 0x9e3779b97f4a7c15ULL + (b << 6) + (b >> 2);
    };
    for (int c = 0; c < cores; ++c) {
        feed(uint64_t(c) + 1);
        for (int v : s.order)
            if (s.core[v] == c) feed(uint64_t(v) + 17);
        feed(0);
    }
    return {a, b};
}

uint64_t product(initializer_list<uint64_t> factors) {
    uint64_t value = 1;
    for (uint64_t factor : factors) {
        if (!factor) return 0;
        if (value > numeric_limits<uint64_t>::max() / factor)
            return numeric_limits<uint64_t>::max();
        value *= factor;
    }
    return value;
}

auto result_key(const Result &r) {
    return make_tuple(r.refined.cycles, r.refined.route + r.refined.spill,
                      r.generated_for_cores, r.solution.core, r.solution.order);
}

struct Neighborhoods {
    vector<vector<int>> writers, chains;
    vector<int> tensors;
    array<uint64_t, family_count> counts{};
};

Neighborhoods neighborhoods(const Graph &g, int cores, const Deadline &deadline) {
    Neighborhoods result;
    vector<vector<int>> writers(g.size());
    for (int v : g.topo) {
        if ((v & 127) == 0) deadline.check();
        writers[g.writer_group[v]].push_back(v);
    }
    for (auto &group : writers)
        if (!group.empty()) result.writers.push_back(move(group));
    auto work = [&](const vector<int> &group) {
        double total = 0;
        for (int v : group) total += g.weight(v);
        return total;
    };
    stable_sort(result.writers.begin(), result.writers.end(), [&](const auto &a, const auto &b) {
        return make_pair(-work(a), a.front()) < make_pair(-work(b), b.front());
    });
    deadline.check();
    result.chains = decompose(g, cores, 0.5, 0.25).bundles;
    stable_sort(result.chains.begin(), result.chains.end(), [&](const auto &a, const auto &b) {
        return make_pair(-work(a), a.front()) < make_pair(-work(b), b.front());
    });
    result.tensors.resize(g.tensors.size());
    iota(result.tensors.begin(), result.tensors.end(), 0);
    auto tensor_work = [&](int t) {
        const auto &x = g.tensors[t];
        return double(x.size) * double(x.producers.size() + x.consumers.size());
    };
    stable_sort(result.tensors.begin(), result.tensors.end(), [&](int a, int b) {
        return make_pair(-tensor_work(a), a) < make_pair(-tensor_work(b), b);
    });
    const uint64_t m = result.writers.size(), k = uint64_t(cores);
    result.counts = {160, product({m,k,4}), product({m,m,4}),
                     product({uint64_t(result.tensors.size()),k,4}),
                     product({uint64_t(result.chains.size()),k,4}), 576,
                     product({uint64_t(g.size()),2}), product({m,m,k,k,4})};
    deadline.check();
    return result;
}

struct SearchNode {
    Result result;
    array<uint64_t, family_count> cursor{};
    size_t next_family = 0;
    explicit SearchNode(Result value, int lane) : result(move(value)) {
        cursor.fill(uint64_t(lane));
        next_family = size_t(lane) % family_count;
    }
    bool exhausted(const Neighborhoods &space) const {
        for (size_t f = 0; f < family_count; ++f)
            if (cursor[f] < space.counts[f]) return false;
        return true;
    }
};

SeedOptions order_options(uint64_t style, int lane) {
    static const array<string,4> modes{{"critical","dfs","breadth","id"}};
    static const array<int,4> windows{{4,8,16,32}};
    static const array<double,4> memory{{0.5,1.0,2.0,4.0}};
    SeedOptions options;
    options.mode = modes[style % 4];
    options.window = windows[style % 4];
    options.memory = memory[(style + uint64_t(lane / 2)) % 4];
    return options;
}

// 旋轉的是 ready 節點的優先序，絕不直接打亂合法拓撲序或刪除前驅。
Solution rotated_order(const Graph &g, const Solution &base, uint64_t index,
                       const Deadline &deadline) {
    const int n = g.size();
    if (!n) return base;
    vector<int> position(n), degree(n);
    for (int i = 0; i < n; ++i) position[base.order[i]] = i;
    int offset = int((index / 2) % uint64_t(n));
    bool reverse = (index & 1U) != 0;
    auto key = [&](int v) {
        int rank = (position[v] + n - offset) % n;
        if (reverse) rank = n - 1 - rank;
        return make_pair(rank, v);
    };
    auto cmp = [&](int a, int b) { return key(a) > key(b); };
    priority_queue<int, vector<int>, decltype(cmp)> ready(cmp);
    for (int v = 0; v < n; ++v) {
        degree[v] = int(g.ops[v].pred.size());
        if (!degree[v]) ready.push(v);
    }
    Solution result = base;
    result.order.clear();
    while (!ready.empty()) {
        if ((result.order.size() & 127U) == 0) deadline.check();
        int v = ready.top(); ready.pop(); result.order.push_back(v);
        for (int w : g.ops[v].succ) if (!--degree[w]) ready.push(w);
    }
    return result;
}

bool proposal(const Graph &g, int cores, const Neighborhoods &space,
              const Solution &base, size_t family, uint64_t index, int lane,
              Deadline &deadline, Solution &out) {
    deadline.check();
    const Config cfg;
    static const array<string,4> modes{{"critical","dfs","breadth","id"}};
    static const array<double,8> memory{{0,0.125,0.25,0.5,1,2,4,8}};
    static const array<int,5> windows{{1,4,8,16,32}};
    const size_t m = space.writers.size();
    auto assignment = base.core;
    uint64_t style = index % 4;
    if (family == 0) {
        SeedOptions options;
        options.mode = modes[index % 4];
        options.memory = memory[(index / 4) % 8];
        options.window = windows[(index / 32) % 5];
        out = operation_plan(g, cores, cfg, options, &assignment, &deadline);
    } else if (family == 5) {
        static const array<double,6> weights{{0,0.125,0.25,0.5,1,2}};
        SeedOptions options;
        options.mode = modes[index % 4];
        options.balance = weights[(index / 4) % 6];
        options.communication = weights[(index / 24) % 6];
        options.window = windows[(index / 144) % 4];
        out = operation_plan(g, cores, cfg, options, nullptr, &deadline);
    } else if (family == 6) {
        out = rotated_order(g, base, index, deadline);
    } else {
        bool changed = false;
        auto put = [&](const vector<int> &group, int core) {
            for (int v : group) {
                changed = changed || assignment[v] != core;
                assignment[v] = core;
            }
        };
        if (family == 1) {
            uint64_t code = index / 4;
            put(space.writers[size_t(code / uint64_t(cores))], int(code % uint64_t(cores)));
        } else if (family == 2) {
            uint64_t code = index / 4;
            size_t a = size_t(code / m), b = size_t(code % m);
            if (a >= b) return false;
            int ca = assignment[space.writers[a].front()], cb = assignment[space.writers[b].front()];
            if (ca == cb) return false;
            put(space.writers[a], cb); put(space.writers[b], ca);
        } else if (family == 3) {
            uint64_t code = index / 4;
            int tensor = space.tensors[size_t(code / uint64_t(cores))];
            int core = int(code % uint64_t(cores));
            set<int> roots;
            for (int v : g.tensors[tensor].producers) roots.insert(g.writer_group[v]);
            for (int v : g.tensors[tensor].consumers) roots.insert(g.writer_group[v]);
            // Writer 閉包跨越階段仍有效，不可只搬一部分寫入者。
            for (int v = 0; v < g.size(); ++v) {
                if ((v & 127) == 0) deadline.check();
                if (roots.count(g.writer_group[v])) {
                    changed = changed || assignment[v] != core;
                    assignment[v] = core;
                }
            }
        } else if (family == 4) {
            uint64_t code = index / 4;
            put(space.chains[size_t(code / uint64_t(cores))], int(code % uint64_t(cores)));
        } else {
            uint64_t code = index / 4;
            int cb = int(code % uint64_t(cores)); code /= uint64_t(cores);
            int ca = int(code % uint64_t(cores)); code /= uint64_t(cores);
            size_t b = size_t(code % m), a = size_t(code / m);
            if (a >= b) return false;
            put(space.writers[a], ca); put(space.writers[b], cb);
        }
        if (!changed) return false;
        out = operation_plan(g, cores, cfg, order_options(style, lane), &assignment, &deadline);
    }
    out.label = "timed_lane" + to_string(lane) + "_family" + to_string(family) + "_" + to_string(index);
    validate(g, out, cores);
    deadline.check();
    return true;
}

struct Lane {
    Result best;
    TimedSearchStats stats;
};

Lane search_lane(const Graph &g, int cores, int lane, int workers, const Result &seed,
                 const Neighborhoods &space, Clock::time_point started, double limit) {
    Lane result;
    result.best = seed;
    Deadline deadline;
    deadline.start = started; deadline.limit = limit; deadline.enabled = true;
    const Config cfg;
    vector<shared_ptr<SearchNode>> beam{make_shared<SearchNode>(seed, lane)};
    set<Signature> seen{signature(seed.solution, cores)};
    size_t round = 0;
    auto prune_beam = [&]() {
        if (beam.size() <= search_limits::timed_beam) return;
        // 各 lane 保留不同取向，但最終輸出始終使用相同的細化準則。
        stable_sort(beam.begin(), beam.end(), [&](const auto &a, const auto &b) {
            auto cost = [&](const Result &r) {
                double traffic = r.refined.route + r.refined.spill;
                return lane % 2 ? max(r.refined.cycles, traffic / cfg.bandwidth) +
                                  0.10 * traffic / cfg.bandwidth : r.refined.cycles;
            };
            return make_tuple(cost(a->result), a->result.refined.route + a->result.refined.spill,
                              a->result.solution.core, a->result.solution.order) <
                   make_tuple(cost(b->result), b->result.refined.route + b->result.refined.spill,
                              b->result.solution.core, b->result.solution.order);
        });
        beam.resize(search_limits::timed_beam);
    };
    try {
        while (true) {
            deadline.check();
            if (seen.size() >= search_limits::timed_seen_limit) {
                result.stats.cache_limited_lanes = 1; break;
            }
            shared_ptr<SearchNode> parent;
            for (size_t i = 0; i < beam.size(); ++i) {
                auto p = beam[(round + i) % beam.size()];
                if (!p->exhausted(space)) { parent = move(p); break; }
            }
            if (!parent) { result.stats.exhausted_lanes = 1; break; }
            ++round;
            struct Coarse { Solution solution; Metrics metrics; };
            vector<Coarse> candidates;
            size_t attempted_in_round = 0;
            const size_t attempt_limit = search_limits::timed_coarse_per_round * 8;
            while (candidates.size() < search_limits::timed_coarse_per_round &&
                   attempted_in_round < attempt_limit && !parent->exhausted(space)) {
                deadline.check();
                size_t family = parent->next_family++ % family_count;
                if (parent->cursor[family] >= space.counts[family]) continue;
                uint64_t index = parent->cursor[family];
                parent->cursor[family] += min(uint64_t(workers),
                    numeric_limits<uint64_t>::max() - parent->cursor[family]);
                ++attempted_in_round; ++result.stats.attempted;
                Solution candidate;
                if (!proposal(g, cores, space, parent->result.solution, family, index, lane, deadline, candidate))
                    continue;
                auto key = signature(candidate, cores);
                if (!seen.insert(key).second) continue;
                ++result.stats.unique_candidates;
                auto metrics = evaluate_proxy(g, cores, cfg, candidate, 0.03, &deadline);
                if (!metrics.memory_warning) candidates.push_back({move(candidate), move(metrics)});
                if (seen.size() >= search_limits::timed_seen_limit) break;
            }
            stable_sort(candidates.begin(), candidates.end(), [&](const Coarse &a, const Coarse &b) {
                auto cost = [&](const Metrics &m) {
                    if (lane == 1) return m.score + 0.10 * (m.route + m.spill) / cfg.bandwidth;
                    if (lane == 2) return max(m.sketch, (m.route + m.spill) / cfg.bandwidth);
                    if (lane == 3) return m.score + 0.10 * m.spill / cfg.bandwidth;
                    return m.score;
                };
                return make_tuple(cost(a.metrics), a.metrics.route + a.metrics.spill, a.solution.core, a.solution.order) <
                       make_tuple(cost(b.metrics), b.metrics.route + b.metrics.spill, b.solution.core, b.solution.order);
            });
            for (size_t i = 0; i < min(search_limits::timed_refine_per_round, candidates.size()); ++i) {
                deadline.check();
                auto refined = refine_estimate(g, candidates[i].solution, cores, &deadline);
                ++result.stats.refined;
                if (!refined.valid) continue;
                Result candidate;
                candidate.solution = move(candidates[i].solution);
                candidate.estimate = move(candidates[i].metrics);
                candidate.refined = move(refined);
                candidate.generated_for_cores = cores;
                if (result_key(candidate) < result_key(result.best)) {
                    if (make_pair(candidate.refined.cycles, candidate.refined.route + candidate.refined.spill) <
                        make_pair(result.best.refined.cycles, result.best.refined.route + result.best.refined.spill))
                        ++result.stats.improvements;
                    result.best = candidate;
                }
                beam.push_back(make_shared<SearchNode>(move(candidate), lane));
            }
            prune_beam();
        }
    } catch (const SearchExpired &) {
        // 未完成的 proposal／估價只存在區域變數中，不覆蓋已完成的 incumbent。
        result.stats.cancelled = 1;
        result.stats.deadline_reached = true;
    }
    return result;
}
} // namespace

Result solve_for_budget(const Graph &g, int cores, Clock::time_point started,
                        double seconds, TimedSearchStats *statistics) {
    if (!isfinite(seconds) || seconds < 0) throw runtime_error("invalid internal time budget");
    TimedSearchStats stats;
    auto elapsed = [&]() { return chrono::duration<double>(Clock::now() - started).count(); };
    // 此階段不被預算打斷，以保留原 v5 可用方案；超大圖的種子可能已超出預算。
    const auto base_started = Clock::now();
    Result seed = solve(g, cores);
    stats.base_seconds = chrono::duration<double>(Clock::now() - base_started).count();
    const double limit = max(0.0, seconds - search_limits::timed_reserve_seconds);
    Result best = seed;
    if (limit > 0 && elapsed() < limit && g.size() > 0) {
        Deadline deadline;
        deadline.start = started; deadline.limit = limit; deadline.enabled = true;
        try {
            const auto space = neighborhoods(g, cores, deadline);
            const int workers = search_limits::timed_workers;
            array<Lane,4> lanes;
            stats.workers = size_t(workers);
            detail::run_independent_jobs(workers, [&](int index) {
                lanes[size_t(index)] = search_lane(g, cores, index, workers, seed, space, started, limit);
            });
            for (int i = 0; i < workers; ++i) {
                const auto &lane = lanes[size_t(i)];
                if (result_key(lane.best) < result_key(best)) best = lane.best;
                stats.attempted += lane.stats.attempted;
                stats.unique_candidates += lane.stats.unique_candidates;
                stats.refined += lane.stats.refined;
                stats.improvements += lane.stats.improvements;
                stats.cancelled += lane.stats.cancelled;
                stats.exhausted_lanes += lane.stats.exhausted_lanes;
                stats.cache_limited_lanes += lane.stats.cache_limited_lanes;
                stats.deadline_reached = stats.deadline_reached || lane.stats.deadline_reached;
            }
        } catch (const SearchExpired &) {
            stats.deadline_reached = true;
        }
    } else {
        stats.deadline_reached = elapsed() >= limit;
    }
    best.candidates = seed.candidates + stats.unique_candidates;
    best.refined_candidates = seed.refined_candidates + stats.refined;
    best.region_proposals = seed.region_proposals;
    best.slack_proposals = seed.slack_proposals;
    best.repair_attempts = seed.repair_attempts;
    validate(g, best.solution, cores);
    stats.elapsed_seconds = elapsed();
    if (statistics) *statistics = stats;
    return best;
}
} // namespace dag_chain
