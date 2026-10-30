#include "anytime.hpp"
#include "parallel_jobs.hpp"
#include "search_limits.hpp"
#include "solution_key.hpp"
#include "tensor_neighborhood.hpp"
#include "memory_grid.hpp"

namespace dag_chain {
namespace {
using Signature = pair<uint64_t, uint64_t>;
constexpr size_t family_count = 9;

// 用两组固定哈希识别同一核心分配及核内顺序，避免反复评估相同方案。
// 哈希碰撞只会略过候选；候选是否合法仍由完整校验决定。
Signature signature(const Solution &s, int cores) {
    uint64_t a = 1469598103934665603ULL, b = 7809847782465536322ULL;
    auto feed = [&](uint64_t x) {
        a = (a ^ x) * 1099511628211ULL;
        b ^= x + 0x9e3779b97f4a7c15ULL + (b << 6) + (b >> 2);
    };
    for (int c = 0; c < cores; ++c) {
        feed(uint64_t(c) + 1);
        for (int v : s.order)
            if (s.core[v] == c)
                feed(uint64_t(v) + 17);
        feed(0);
    }
    return {a, b};
}

uint64_t product(initializer_list<uint64_t> factors) {
    uint64_t value = 1;
    for (uint64_t factor : factors) {
        if (!factor)
            return 0;
        if (value > numeric_limits<uint64_t>::max() / factor)
            return numeric_limits<uint64_t>::max();
        value *= factor;
    }
    return value;
}

auto result_key(const Result &r) {
    return detail::solution_key(r.refined.cycles, r.refined.route + r.refined.spill,
                                r.generated_for_cores, r.solution);
}

struct Neighborhoods {
    vector<vector<int>> writers, chains;
    vector<int> tensors;
    unique_ptr<TensorNeighborhoods> consumer_moves;
    // 九类邻域依次为：内存重排、写者组迁移、写者组交换、张量相关组聚合、
    // 链迁移、重新分配、就绪节点顺序旋转、双组联合迁移、消费者组聚合。
    array<uint64_t, family_count> counts{};
};

Neighborhoods neighborhoods(const Graph &g, int cores, Deadline &deadline,
                            const TimedSearchOptions &options) {
    Neighborhoods result;
    vector<vector<int>> writers(g.size());
    for (int v : g.topo) {
        if ((v & 127) == 0)
            deadline.check();
        writers[g.writer_group[v]].push_back(v);
    }
    for (auto &group : writers)
        if (!group.empty())
            result.writers.push_back(move(group));
    auto sort_by_work = [&](vector<vector<int>> &groups) {
        // 每组工作量只计算一次；工作量相同时按组首节点确定顺序。
        vector<pair<double, int>> keys;
        vector<size_t> order(groups.size());
        keys.reserve(groups.size());
        iota(order.begin(), order.end(), size_t(0));
        for (const auto &group : groups) {
            double total = 0;
            for (int v : group)
                total += g.weight(v);
            keys.push_back({-total, group.front()});
        }
        stable_sort(order.begin(), order.end(),
                    [&](size_t a, size_t b) { return keys[a] < keys[b]; });
        vector<vector<int>> sorted;
        sorted.reserve(groups.size());
        for (size_t index : order)
            sorted.push_back(move(groups[index]));
        groups = move(sorted);
    };
    sort_by_work(result.writers);
    deadline.check();
    result.chains = decompose(g, cores, 0.5, 0.25).bundles;
    sort_by_work(result.chains);
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
    result.consumer_moves = make_unique<TensorNeighborhoods>(g, &deadline);
    result.counts = {memory_grid::count(options.useful_memory_orders_first),
                     product({m, k, 4}),
                     product({m, m, 4}),
                     product({uint64_t(result.tensors.size()), k, 4}),
                     product({uint64_t(result.chains.size()), k, 4}),
                     576,
                     product({uint64_t(g.size()), 2}),
                     product({m, m, k, k, 4}),
                     product({result.consumer_moves->count(cores), 2})};
    deadline.check();
    return result;
}

struct SearchNode {
    Result result;
    // 每个方案独立记录九类邻域的进度，轮流尝试各类，避免只搜索一种变换。
    array<uint64_t, family_count> cursor{};
    size_t next_family = 0;
    explicit SearchNode(Result value, int lane, int cursor_start) : result(move(value)) {
        cursor.fill(uint64_t(cursor_start));
        next_family = size_t(lane) % family_count;
    }
    bool exhausted(const Neighborhoods &space) const {
        for (size_t f = 0; f < family_count; ++f)
            if (cursor[f] < space.counts[f])
                return false;
        return true;
    }
};

SeedOptions order_options(uint64_t style, int lane) {
    static const array<string, 4> modes{{"critical", "dfs", "breadth", "id"}};
    static const array<int, 4> windows{{4, 8, 16, 32}};
    static const array<double, 4> memory{{0.5, 1.0, 2.0, 4.0}};
    SeedOptions options;
    options.mode = modes[style % 4];
    options.window = windows[style % 4];
    options.memory = memory[(style + uint64_t(lane / 2)) % 4];
    return options;
}

// 只旋转已就绪节点的优先级；仍须等全部前驱完成，保证生成合法拓扑序。
Solution rotated_order(const Graph &g, const Solution &base, uint64_t index,
                       const Deadline &deadline) {
    const int n = g.size();
    if (!n)
        return base;
    vector<int> position(n), degree(n);
    for (int i = 0; i < n; ++i)
        position[base.order[i]] = i;
    int offset = int((index / 2) % uint64_t(n));
    bool reverse = (index & 1U) != 0;
    auto key = [&](int v) {
        int rank = (position[v] + n - offset) % n;
        if (reverse)
            rank = n - 1 - rank;
        return make_pair(rank, v);
    };
    auto cmp = [&](int a, int b) { return key(a) > key(b); };
    priority_queue<int, vector<int>, decltype(cmp)> ready(cmp);
    for (int v = 0; v < n; ++v) {
        degree[v] = int(g.ops[v].pred.size());
        if (!degree[v])
            ready.push(v);
    }
    Solution result = base;
    result.order.clear();
    while (!ready.empty()) {
        if ((result.order.size() & 127U) == 0)
            deadline.check();
        int v = ready.top();
        ready.pop();
        result.order.push_back(v);
        for (int w : g.ops[v].succ)
            if (!--degree[w])
                ready.push(w);
    }
    return result;
}

bool proposal(const Graph &g, int cores, const Neighborhoods &space, const Solution &base,
              size_t family, uint64_t index, int lane, Deadline &deadline, Solution &out,
              const TimedSearchOptions &search_options) {
    deadline.check();
    const Config cfg;
    static const array<string, 4> modes{{"critical", "dfs", "breadth", "id"}};
    static const array<int, 5> windows{{1, 4, 8, 16, 32}};
    const size_t m = space.writers.size();
    uint64_t style = index % 4;
    if (family == 0) {
        const SeedOptions options =
            memory_grid::options(index, search_options.useful_memory_orders_first);
        out = operation_plan(g, cores, cfg, options, &base.core, &deadline);
    } else if (family == 5) {
        static const array<double, 6> weights{{0, 0.125, 0.25, 0.5, 1, 2}};
        SeedOptions options;
        options.mode = modes[index % 4];
        options.balance = weights[(index / 4) % 6];
        options.communication = weights[(index / 24) % 6];
        options.window = windows[(index / 144) % 4];
        out = operation_plan(g, cores, cfg, options, nullptr, &deadline);
    } else if (family == 8) {
        // 保留生产者的并行分配，只聚合同一来源核心上的消费者；必要时
        // 交换一个无关写者组以补偿流水线负载，闭包处理见 TensorNeighborhoods。
        if (!space.consumer_moves->propose(g, base, cores, index / 2, &deadline, out))
            return false;
        if (index & 1U)
            out = operation_plan(g, cores, cfg, order_options(uint64_t(lane), lane), &out.core,
                                 &deadline);
    } else if (family == 6) {
        out = rotated_order(g, base, index, deadline);
    } else {
        // 迁移分支修改分配副本，候选构造失败时不会影响父方案。
        auto assignment = base.core;
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
            if (a >= b)
                return false;
            int ca = assignment[space.writers[a].front()],
                cb = assignment[space.writers[b].front()];
            if (ca == cb)
                return false;
            put(space.writers[a], cb);
            put(space.writers[b], ca);
        } else if (family == 3) {
            uint64_t code = index / 4;
            int tensor = space.tensors[size_t(code / uint64_t(cores))];
            int core = int(code % uint64_t(cores));
            set<int> roots;
            for (int v : g.tensors[tensor].producers)
                roots.insert(g.writer_group[v]);
            for (int v : g.tensors[tensor].consumers)
                roots.insert(g.writer_group[v]);
            // 写者闭包包含所有必须同核的共享写者，可能跨越多个计算阶段。
            // 因此按组根迁移全部成员，不能只搬当前张量直接关联的节点。
            for (int v = 0; v < g.size(); ++v) {
                if ((v & 127) == 0)
                    deadline.check();
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
            int cb = int(code % uint64_t(cores));
            code /= uint64_t(cores);
            int ca = int(code % uint64_t(cores));
            code /= uint64_t(cores);
            size_t b = size_t(code % m), a = size_t(code / m);
            if (a >= b)
                return false;
            put(space.writers[a], ca);
            put(space.writers[b], cb);
        }
        if (!changed)
            return false;
        out = operation_plan(g, cores, cfg, order_options(style, lane), &assignment, &deadline);
    }
    out.label =
        "timed_lane" + to_string(lane) + "_family" + to_string(family) + "_" + to_string(index);
    validate(g, out, cores);
    deadline.check();
    return true;
}

struct Lane {
    Result best;
    TimedSearchStats stats;
};

Lane search_lane(const Graph &g, int cores, int lane, int cursor_start, int cursor_stride,
                 const Result &seed, const Neighborhoods &space, Clock::time_point started,
                 double limit, const TimedSearchOptions &options, size_t fixed_rounds = 0) {
    Lane result;
    result.best = seed;
    Deadline deadline;
    // 正式入口传入固定轮数，关闭时限中断；机器负载不会影响候选集合。
    // 每条搜索路线（lane）持有自己的 beam、去重集合、游标及当前最优解。
    deadline.start = started;
    deadline.limit = limit;
    deadline.enabled = fixed_rounds == 0;
    const Config cfg;
    vector<shared_ptr<SearchNode>> beam{make_shared<SearchNode>(seed, lane, cursor_start)};
    set<Signature> seen{signature(seed.solution, cores)};
    size_t round = 0;
    auto prune_beam = [&]() {
        if (beam.size() <= search_limits::timed_beam)
            return;
        // 路线可侧重周期或搬运，但最终选择始终使用相同的精评比较规则。
        stable_sort(beam.begin(), beam.end(), [&](const auto &a, const auto &b) {
            auto cost = [&](const Result &r) {
                double traffic = r.refined.route + r.refined.spill;
                return lane % 2 ? max(r.refined.cycles, traffic / cfg.bandwidth) +
                                      0.10 * traffic / cfg.bandwidth
                                : r.refined.cycles;
            };
            return detail::solution_key(cost(a->result),
                                        a->result.refined.route + a->result.refined.spill,
                                        a->result.solution) <
                   detail::solution_key(cost(b->result),
                                        b->result.refined.route + b->result.refined.spill,
                                        b->result.solution);
        });
        beam.resize(search_limits::timed_beam);
    };
    try {
        while (!fixed_rounds || round < fixed_rounds) {
            deadline.check();
            if (seen.size() >= search_limits::timed_seen_limit) {
                result.stats.cache_limited_lanes = 1;
                break;
            }
            shared_ptr<SearchNode> parent;
            for (size_t i = 0; i < beam.size(); ++i) {
                auto p = beam[(round + i) % beam.size()];
                if (!p->exhausted(space)) {
                    parent = move(p);
                    break;
                }
            }
            if (!parent) {
                result.stats.exhausted_lanes = 1;
                break;
            }
            ++round;
            struct Coarse {
                Solution solution;
                Metrics metrics;
            };
            vector<Coarse> candidates;
            size_t attempted_in_round = 0;
            size_t coarse_in_round = 0;
            // 固定模式每轮最多尝试 96 个邻域槽位、粗估 12 个不同方案、精评 3 个方案。
            // 内存预警候选也计入固定粗估额度，避免无效候选导致工作量失控。
            const size_t attempt_limit = search_limits::timed_coarse_per_round * 8;
            while (candidates.size() < search_limits::timed_coarse_per_round &&
                   (!fixed_rounds || coarse_in_round < search_limits::timed_coarse_per_round) &&
                   attempted_in_round < attempt_limit && !parent->exhausted(space)) {
                deadline.check();
                size_t family = parent->next_family++ % family_count;
                if (parent->cursor[family] >= space.counts[family])
                    continue;
                uint64_t index = parent->cursor[family];
                parent->cursor[family] +=
                    min(uint64_t(cursor_stride),
                        numeric_limits<uint64_t>::max() - parent->cursor[family]);
                ++attempted_in_round;
                ++result.stats.attempted;
                Solution candidate;
                if (!proposal(g, cores, space, parent->result.solution, family, index, lane,
                              deadline, candidate, options))
                    continue;
                auto key = signature(candidate, cores);
                if (!seen.insert(key).second)
                    continue;
                ++result.stats.unique_candidates;
                ++coarse_in_round;
                auto metrics = evaluate_proxy(g, cores, cfg, candidate, 0.03, &deadline);
                if (!metrics.memory_warning)
                    candidates.push_back({move(candidate), move(metrics)});
                if (seen.size() >= search_limits::timed_seen_limit)
                    break;
            }
            stable_sort(
                candidates.begin(), candidates.end(), [&](const Coarse &a, const Coarse &b) {
                    auto cost = [&](const Metrics &m) {
                        if (lane == 1)
                            return m.score + 0.10 * (m.route + m.spill) / cfg.bandwidth;
                        if (lane == 2)
                            return max(m.sketch, (m.route + m.spill) / cfg.bandwidth);
                        if (lane == 3)
                            return m.score + 0.10 * m.spill / cfg.bandwidth;
                        return m.score;
                    };
                    return detail::solution_key(cost(a.metrics), a.metrics.route + a.metrics.spill,
                                                a.solution) <
                           detail::solution_key(cost(b.metrics), b.metrics.route + b.metrics.spill,
                                                b.solution);
                });
            for (size_t i = 0; i < min(search_limits::timed_refine_per_round, candidates.size());
                 ++i) {
                deadline.check();
                auto refined = refine_estimate(g, candidates[i].solution, cores, &deadline);
                ++result.stats.refined;
                if (!refined.valid)
                    continue;
                Result candidate;
                candidate.solution = move(candidates[i].solution);
                candidate.estimate = move(candidates[i].metrics);
                candidate.refined = move(refined);
                candidate.generated_for_cores = cores;
                if (result_key(candidate) < result_key(result.best)) {
                    if (make_pair(candidate.refined.cycles,
                                  candidate.refined.route + candidate.refined.spill) <
                        make_pair(result.best.refined.cycles,
                                  result.best.refined.route + result.best.refined.spill))
                        ++result.stats.improvements;
                    result.best = candidate;
                }
                beam.push_back(make_shared<SearchNode>(move(candidate), lane, cursor_start));
            }
            prune_beam();
        }
    } catch (const SearchExpired &) {
        // 开发限时模式中，被中断的候选只存在于局部变量，不覆盖已有有效解。
        result.stats.cancelled = 1;
        result.stats.deadline_reached = true;
    }
    return result;
}
} // namespace

Result continue_from_seed_for_budget(const Graph &g, int cores, Clock::time_point started,
                                     double seconds, const Result &seed,
                                     vector<Result> seed_portfolio, TimedSearchStats *statistics,
                                     TimedSearchOptions options) {
    if (!isfinite(seconds) || seconds < 0)
        throw runtime_error("invalid internal time budget");
    TimedSearchStats stats;
    auto elapsed = [&]() { return chrono::duration<double>(Clock::now() - started).count(); };
    if (cores < 2 || cores > 5 || !seed.refined.valid)
        throw runtime_error("invalid continuation seed");
    validate(g, seed.solution, cores);
    for (const auto &candidate : seed_portfolio) {
        if (!candidate.refined.valid)
            throw runtime_error("incomplete continuation seed");
        validate(g, candidate.solution, cores);
    }
    const double limit = max(0.0, seconds - search_limits::timed_reserve_seconds);
    Result best = seed;
    if (limit > 0 && elapsed() < limit && g.size() > 0) {
        Deadline deadline;
        deadline.start = started;
        deadline.limit = limit;
        deadline.enabled = true;
        try {
            const auto space = neighborhoods(g, cores, deadline, options);
            const int workers = search_limits::timed_workers;
            // 复用不同核心预算下已完成的优胜方案作为独立起点，避免重复种子搜索。
            // 路线 0 保留原赢家，其余路线探索不同分配；最终选择仍保留原赢家。
            stable_sort(
                seed_portfolio.begin(), seed_portfolio.end(),
                [](const Result &a, const Result &b) { return result_key(a) < result_key(b); });
            vector<const Result *> lane_seeds{&seed};
            set<Signature> seed_keys{signature(seed.solution, cores)};
            for (const auto &candidate : seed_portfolio) {
                if (lane_seeds.size() >= size_t(workers))
                    break;
                if (seed_keys.insert(signature(candidate.solution, cores)).second)
                    lane_seeds.push_back(&candidate);
            }
            while (lane_seeds.size() < size_t(workers))
                lane_seeds.push_back(&seed);
            // 仅对相同起点分片：四条路线同起点时分别从 0..3 开始、步长为 4；
            // 独有起点从 0 开始、步长为 1，避免漏掉某一索引余数类。
            array<int, 4> cursor_start{}, cursor_stride{};
            for (int i = 0; i < workers; ++i)
                for (int j = 0; j < workers; ++j)
                    if (lane_seeds[size_t(i)] == lane_seeds[size_t(j)]) {
                        ++cursor_stride[size_t(i)];
                        if (j < i)
                            ++cursor_start[size_t(i)];
                    }
            array<Lane, 4> lanes;
            stats.workers = size_t(workers);
            detail::run_independent_jobs(workers, [&](int index) {
                lanes[size_t(index)] = search_lane(
                    g, cores, index, cursor_start[size_t(index)], cursor_stride[size_t(index)],
                    *lane_seeds[size_t(index)], space, started, limit, options);
            });
            for (int i = 0; i < workers; ++i) {
                const auto &lane = lanes[size_t(i)];
                if (result_key(lane.best) < result_key(best))
                    best = lane.best;
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
    if (statistics)
        *statistics = stats;
    return best;
}

Result solve_for_budget(const Graph &g, int cores, Clock::time_point started, double seconds,
                        TimedSearchStats *statistics, TimedSearchOptions options) {
    if (!isfinite(seconds) || seconds < 0)
        throw runtime_error("invalid internal time budget");
    const auto base_started = Clock::now();
    vector<Result> portfolio;
    Result seed = solve(g, cores, false, 7, &portfolio);
    const double base_seconds = chrono::duration<double>(Clock::now() - base_started).count();
    TimedSearchStats stats;
    auto result = continue_from_seed_for_budget(g, cores, started, seconds, seed, move(portfolio),
                                                &stats, options);
    stats.base_seconds = base_seconds;
    if (statistics)
        *statistics = stats;
    return result;
}

Result solve_fixed_work(const Graph &g, int cores, TimedSearchStats *statistics) {
    const auto started = Clock::now();
    vector<Result> portfolio;
    Result seed = solve(g, cores, false, 7, &portfolio);
    TimedSearchStats stats;
    stats.base_seconds = chrono::duration<double>(Clock::now() - started).count();
    Result best = seed;

    // 工作量只取决于图规模：小图追加 4 轮，中图 2 轮，超大图沿用种子解。
    // 这里只控制搜索成本，不修改输入图、依赖或硬件规则。
    const size_t graph_work = g.ops.size() + g.tensors.size() + g.edge_count;
    const size_t rounds = graph_work > search_limits::large_graph_work ? 0
                          : graph_work > search_limits::medium_graph_work
                              ? search_limits::fixed_rounds_medium
                              : search_limits::fixed_rounds_small;
    if (rounds && g.size() > 0) {
        Deadline no_deadline;
        no_deadline.enabled = false;
        const TimedSearchOptions options;
        const auto space = neighborhoods(g, cores, no_deadline, options);
        const int workers = search_limits::timed_workers;
        // 先按统一规则排序，再按方案签名去重，使多起点组合及其顺序可复现。
        stable_sort(portfolio.begin(), portfolio.end(),
                    [](const Result &a, const Result &b) { return result_key(a) < result_key(b); });
        vector<const Result *> lane_seeds{&seed};
        set<Signature> seed_keys{signature(seed.solution, cores)};
        for (const auto &candidate : portfolio) {
            if (lane_seeds.size() == size_t(workers))
                break;
            if (seed_keys.insert(signature(candidate.solution, cores)).second)
                lane_seeds.push_back(&candidate);
        }
        while (lane_seeds.size() < size_t(workers))
            lane_seeds.push_back(&seed);

        // 相同起点分片枚举，独有起点完整枚举；分片只影响尝试顺序，不改邻域定义。
        array<int, 4> cursor_start{}, cursor_stride{};
        for (int i = 0; i < workers; ++i)
            for (int j = 0; j < workers; ++j)
                if (lane_seeds[size_t(i)] == lane_seeds[size_t(j)]) {
                    ++cursor_stride[size_t(i)];
                    if (j < i)
                        ++cursor_start[size_t(i)];
                }
        array<Lane, 4> lanes;
        stats.workers = size_t(workers);
        detail::run_independent_jobs(workers, [&](int index) {
            lanes[size_t(index)] = search_lane(
                g, cores, index, cursor_start[size_t(index)], cursor_stride[size_t(index)],
                *lane_seeds[size_t(index)], space, started, 0, options, rounds);
        });
        // 所有路线完成后按固定下标归并，不使用线程完成先后决定赢家。
        // best 初值为种子赢家，因此追加搜索不会丢弃内部精评更好的已有方案。
        for (int i = 0; i < workers; ++i) {
            const auto &lane = lanes[size_t(i)];
            if (result_key(lane.best) < result_key(best))
                best = lane.best;
            stats.attempted += lane.stats.attempted;
            stats.unique_candidates += lane.stats.unique_candidates;
            stats.refined += lane.stats.refined;
            stats.improvements += lane.stats.improvements;
            stats.exhausted_lanes += lane.stats.exhausted_lanes;
            stats.cache_limited_lanes += lane.stats.cache_limited_lanes;
        }
    }
    best.candidates = seed.candidates + stats.unique_candidates;
    best.refined_candidates = seed.refined_candidates + stats.refined;
    best.region_proposals = seed.region_proposals;
    best.slack_proposals = seed.slack_proposals;
    best.spill_proposals = seed.spill_proposals;
    best.repair_attempts = seed.repair_attempts;
    validate(g, best.solution, cores);
    stats.elapsed_seconds = chrono::duration<double>(Clock::now() - started).count();
    if (statistics)
        *statistics = stats;
    return best;
}

} // namespace dag_chain
