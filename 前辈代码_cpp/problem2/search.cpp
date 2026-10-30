// 对应 search.py。
// 场景 B 的完整搜索流程：构造首解、生成种子、多路线搜索、评估与保存。
#include "search.hpp"
#include "activation.hpp"
#include "artifacts.hpp"
#include "bootstrap.hpp"
#include "generator.hpp"
#include "inheritance.hpp"
#include "official.hpp"
#include "pipeline.hpp"
#include "portfolio.hpp"
#include "structure.hpp"
#include "submission_io.hpp"
#include "utils.hpp"
namespace problem2 {
namespace {
using Work = std::map<std::string, double>;
double sum(const Work &row) {
    double value = 0;
    for (auto [p, w] : row) {
        value += w;
    }
    return value;
}
double maximum(const Work &row) {
    double value = 0;
    for (auto [p, w] : row) {
        value = std::max(value, w);
    }
    return value;
}
std::pair<std::map<Id, Work>, PipeLoads> block_loads(const GraphInfo &g, const Plan &p) {
    std::map<Id, Work> blocks;
    PipeLoads cores(p.ncores);
    for (const auto &[b, nodes] : p.blocks) {
        auto &load = blocks[b];
        for (Id v : nodes) {
            load[field_string(g.ops.at(v), "pipe")] += field_id(g.ops.at(v), "cycles");
        }
        for (auto [pipe, w] : load) {
            cores[p.core_of.at(b)][pipe] += w;
        }
    }
    return {blocks, cores};
}
std::optional<Plan> split(const Plan &p, Id b, Id cut) {
    const auto &nodes = p.blocks.at(b);
    if (cut <= 0 || cut >= Id(nodes.size())) {
        return {};
    }
    auto candidate = p;
    Id next = candidate.next_block_id++;
    candidate.blocks[b] = Ids(nodes.begin(), nodes.begin() + cut);
    candidate.blocks[next] = Ids(nodes.begin() + cut, nodes.end());
    candidate.core_of[next] = p.core_of.at(b);
    candidate.order.insert(std::find(candidate.order.begin(), candidate.order.end(), b) + 1, next);
    return candidate;
}
Plan insert(const GraphInfo &g, const Plan &p, Id b, Id desired) {
    auto [lower, upper] = insertion_bounds(g, p, b);
    return *move_block(g, p, b, std::max(lower, std::min(upper, desired)));
}
struct TensorGroup {
    Id traffic, tid;
    IdSet producers;
    std::map<Id, IdSet> groups;
};
std::vector<TensorGroup> tensor_groups(const GraphInfo &g, const Plan &p) {
    auto owner = p.owner();
    std::vector<TensorGroup> rows;
    for (const auto &[tid, tensor] : g.tensors) {
        IdSet producers, consumers, sources;
        std::map<Id, IdSet> groups;
        for (Id v : g.eligible_producers.at(tid)) {
            producers.insert(owner.at(v));
        }
        for (Id v : g.eligible_consumers.at(tid)) {
            consumers.insert(owner.at(v));
        }
        if (consumers.empty()) {
            continue;
        }
        for (Id b : consumers) {
            groups[p.core_of.at(b)].insert(b);
        }
        for (Id b : producers) {
            sources.insert(p.core_of.at(b));
        }
        Id pairs = 0;
        for (Id s : sources) {
            for (const auto &[c, members] : groups) {
                pairs += s != c;
            }
        }
        rows.push_back(
            {field_id(tensor, "size") *
                 (producers.empty() ? std::max<Id>(0, Id(groups.size()) - 1) : 2 * pairs),
             tid, producers, groups});
    }
    std::stable_sort(rows.begin(), rows.end(), [](const TensorGroup &a, const TensorGroup &b) {
        return std::make_pair(-a.traffic, a.tid) < std::make_pair(-b.traffic, b.tid);
    });
    return rows;
}
} // namespace
// 忽略等待和搬运后的流水线负载是安全下界，可用于排除不可能改进的候选。
double pipe_bound(const GraphInfo &g, const Plan &p) {
    double result = 0;
    for (const auto &row : block_loads(g, p).second) {
        result = std::max(result, maximum(row));
    }
    return result;
}
// 三类邻域分别改变切块/顺序、核心分配及相关张量组；此处仅生成候选。
std::vector<Candidate> neighborhoods(const GraphInfo &g, const Plan &p, const Json &profile,
                                     const Json &official_result, int family,
                                     const Settings &settings, bool no_group, bool no_repair) {
    // C++17 的 lambda 不能直接捕获结构化绑定，使用普通引用保留相同数据。
    auto load_data = block_loads(g, p);
    auto &blocks = load_data.first;
    auto &loads = load_data.second;
    auto owner = p.owner();
    std::map<Id, Id> positions;
    for (std::size_t i = 0; i < p.order.size(); ++i) {
        positions[p.order[i]] = Id(i);
    }
    struct Factory {
        std::string kind, label;
        std::function<std::optional<Plan>()> build;
        bool repair;
    };
    std::vector<std::pair<std::string, std::vector<Factory>>> pools;
    auto add = [&](std::string kind, std::string label, std::function<std::optional<Plan>()> build,
                   bool repair = false) {
        auto i = std::find_if(pools.begin(), pools.end(),
                              [&](const auto &pool) { return pool.first == kind; });
        if (i == pools.end()) {
            pools.push_back({kind, {}});
            i = pools.end() - 1;
        }
        i->second.push_back({kind, label, std::move(build), repair});
    };
    auto add_plan = [&](std::string kind, std::string label, std::optional<Plan> plan,
                        bool repair = false) {
        if (plan) {
            add(kind, label, [plan] { return plan; }, repair);
        }
    };
    Ids heavy;
    for (const auto &[b, nodes] : p.blocks) {
        heavy.push_back(b);
    }
    std::sort(heavy.begin(), heavy.end(), [&](Id a, Id b) {
        return std::make_pair(-maximum(blocks[a]), a) < std::make_pair(-maximum(blocks[b]), b);
    });
    if (heavy.size() > 4) {
        heavy.resize(4);
    }
    if (family == 0) {
        if (!no_repair) {
            add_plan("order", "lifetime_repair", repair_order(g, p, settings, profile));
            auto hotspots = profile.value("lifetime_hotspots", array());
            for (std::size_t i = 0; i < std::min<std::size_t>(8, hotspots.size()); ++i) {
                const auto &hot = hotspots[i];
                std::vector<std::pair<Id, Id>> cuts;
                for (const auto &op : hot.value("producer_ops", array())) {
                    if (owner.count(number(op))) {
                        Id b = owner.at(number(op));
                        const auto &nodes = p.blocks.at(b);
                        cuts.emplace_back(b, Id(std::find(nodes.begin(), nodes.end(), number(op)) -
                                                nodes.begin()) +
                                                 1);
                    }
                }
                if (hot.contains("last_use_op") && !hot.at("last_use_op").is_null() &&
                    owner.count(field_id(hot, "last_use_op"))) {
                    Id op = field_id(hot, "last_use_op"), b = owner.at(op);
                    const auto &nodes = p.blocks.at(b);
                    cuts.emplace_back(
                        b, Id(std::find(nodes.begin(), nodes.end(), op) - nodes.begin()));
                }
                for (std::size_t j = 0; j < std::min<std::size_t>(2, cuts.size()); ++j) {
                    auto [b, cut] = cuts[j];
                    add_plan("split", "hotspot_split_" + key(b) + "_" + key(cut), split(p, b, cut),
                             true);
                }
            }
        }
        for (Id b : heavy) {
            if (p.blocks.at(b).size() > 1) {
                double total = 0;
                for (Id v : p.blocks.at(b)) {
                    total += g.weight.at(v);
                }
                Id partial = 0, cut = 1;
                for (std::size_t i = 0; i + 1 < p.blocks.at(b).size(); ++i) {
                    partial += g.weight.at(p.blocks.at(b)[i]);
                    cut = Id(i) + 1;
                    if (partial >= total / 2) {
                        break;
                    }
                }
                auto divided = *split(p, b, cut);
                add_plan("split", "split_" + key(b), divided);
                std::string pipe;
                double work = -1;
                for (Id v : p.blocks.at(b)) {
                    auto name = field_string(g.ops.at(v), "pipe");
                    if (blocks[b][name] > work) {
                        pipe = name;
                        work = blocks[b][name];
                    }
                }
                Ids targets;
                for (Id c = 0; c < p.ncores; ++c) {
                    if (c != p.core_of.at(b)) {
                        targets.push_back(c);
                    }
                }
                std::sort(targets.begin(), targets.end(), [&](Id a, Id c) {
                    return std::make_pair(loads[a][pipe], a) < std::make_pair(loads[c][pipe], c);
                });
                for (std::size_t i = 0; i < std::min<std::size_t>(2, targets.size()); ++i) {
                    add_plan("split", "split_migrate_" + key(b) + "_" + key(targets[i]),
                             migrate(divided, {divided.next_block_id - 1}, targets[i]), true);
                }
            }
        }
        Id merges = 0;
        for (std::size_t i = 1; i < p.order.size() && merges < 8; ++i) {
            Id a = p.order[i - 1], b = p.order[i];
            auto merged = merge_adjacent(g, p, a, b);
            if (merged) {
                add_plan("merge", "merge_" + key(a) + "_" + key(b), merged);
                ++merges;
            }
        }
    } else if (family == 1) {
        auto rows = tensor_groups(g, p);
        for (std::size_t i = 0; i < std::min<std::size_t>(8, rows.size()); ++i) {
            auto row = rows[i];
            if (!row.traffic) {
                continue;
            }
            Id tid = row.tid;
            IdSet targets;
            for (Id b : row.producers) {
                targets.insert(p.core_of.at(b));
            }
            for (const auto &[c, group] : row.groups) {
                targets.insert(c);
            }
            for (const auto &source_group : row.groups) {
                const Id source = source_group.first;
                const auto &members = source_group.second;
                for (Id target : targets) {
                    if (source != target) {
                        auto group = as_vector(members);
                        if (!no_group && group.size() > 1) {
                            add(
                                "tensor",
                                "group_t" + key(tid) + "_" + key(source) + "_" + key(target),
                                [&, group, target] { return migrate(p, group, target); }, true);
                        }
                        for (std::size_t j = 0; j < std::min<std::size_t>(2, group.size()); ++j) {
                            Id b = group[j];
                            add(
                                "tensor",
                                "consumer_t" + key(tid) + "_" + key(b) + "_" + key(target),
                                [&, b, target] { return migrate(p, {b}, target); }, true);
                        }
                        Ids moving = no_group ? Ids{group.front()} : group;
                        double work = 0;
                        for (Id b : moving) {
                            work += sum(blocks[b]);
                        }
                        Ids compensators;
                        for (const auto &[b, nodes] : p.blocks) {
                            if (p.core_of.at(b) == target && !row.producers.count(b)) {
                                compensators.push_back(b);
                            }
                        }
                        std::sort(compensators.begin(), compensators.end(), [&](Id a, Id b) {
                            return std::make_pair(std::abs(sum(blocks[a]) - work), a) <
                                   std::make_pair(std::abs(sum(blocks[b]) - work), b);
                        });
                        for (std::size_t j = 0; j < std::min<std::size_t>(2, compensators.size());
                             ++j) {
                            Id other = compensators[j];
                            add(
                                "swap", "exchange_t" + key(tid) + "_" + key(other),
                                [&, moving, target, other, source] {
                                    auto candidate = migrate(p, moving, target);
                                    candidate.core_of[other] = source;
                                    return candidate;
                                },
                                true);
                        }
                    }
                }
            }
            for (Id b : row.producers) {
                for (const auto &target_group : row.groups) {
                    const Id target = target_group.first;
                    if (p.core_of.at(b) != target) {
                        add(
                            "tensor", "producer_t" + key(tid) + "_" + key(b) + "_" + key(target),
                            [&, b, target] { return migrate(p, {b}, target); }, true);
                    }
                }
            }
        }
        std::tuple<double, Id, std::string> busiest = {0, 0, "PIPE_M"};
        for (Id c = 0; c < p.ncores; ++c) {
            for (auto [pipe, w] : loads[c]) {
                busiest = std::max(busiest, std::make_tuple(w, c, pipe));
            }
        }
        Id source = std::get<1>(busiest);
        auto pipe = std::get<2>(busiest);
        Ids here, targets;
        for (const auto &[b, nodes] : p.blocks) {
            if (p.core_of.at(b) == source) {
                here.push_back(b);
            }
        }
        std::sort(here.begin(), here.end(), [&](Id a, Id b) {
            return std::make_pair(-blocks[a][pipe], a) < std::make_pair(-blocks[b][pipe], b);
        });
        if (here.size() > 4) {
            here.resize(4);
        }
        for (Id c = 0; c < p.ncores; ++c) {
            if (c != source) {
                targets.push_back(c);
            }
        }
        std::sort(targets.begin(), targets.end(), [&](Id a, Id b) {
            return std::make_pair(loads[a][pipe], a) < std::make_pair(loads[b][pipe], b);
        });
        if (targets.size() > 2) {
            targets.resize(2);
        }
        for (Id b : here) {
            for (Id target : targets) {
                add(
                    "load", "load_" + key(b) + "_" + key(target),
                    [&, b, target] { return migrate(p, {b}, target); }, true);
            }
        }
        auto successors = block_dependencies(g, p.blocks).second;
        for (Id b : here) {
            Ids group = {b};
            Id tail = b;
            for (int i = 0; i < 5; ++i) {
                Ids choices;
                for (Id c : successors.at(tail)) {
                    if (p.core_of.at(c) == source &&
                        std::find(group.begin(), group.end(), c) == group.end()) {
                        choices.push_back(c);
                    }
                }
                if (choices.empty()) {
                    break;
                }
                tail = *std::max_element(choices.begin(), choices.end(), [&](Id a, Id c) {
                    return std::make_pair(blocks[a][pipe], -a) <
                           std::make_pair(blocks[c][pipe], -c);
                });
                group.push_back(tail);
            }
            if (group.size() > 1) {
                for (Id target : targets) {
                    add(
                        "load_group", "load_chain_" + key(b) + "_" + key(target),
                        [&, group, target] { return migrate(p, group, target); }, true);
                }
            }
        }
    } else {
        std::vector<std::tuple<double, Id, std::string>> gaps;
        for (const auto &timeline : official_result.value("per_core_timeline", array())) {
            Work previous;
            auto ops = timeline.at("ops");
            std::stable_sort(ops.begin(), ops.end(), [](const Json &a, const Json &b) {
                return std::make_pair(a.at("start").get<double>(), field_id(a, "op_id")) <
                       std::make_pair(b.at("start").get<double>(), field_id(b, "op_id"));
            });
            for (const auto &op : ops) {
                auto pipe = field_string(op, "pipe");
                double gap = op.at("start").get<double>() - previous[pipe];
                if (gap > 0 && op.contains("subgraph_id") && !op.at("subgraph_id").is_null() &&
                    p.blocks.count(field_id(op, "subgraph_id"))) {
                    gaps.emplace_back(gap, field_id(op, "subgraph_id"), pipe);
                }
                previous[pipe] = std::max(previous[pipe], op.at("end").get<double>());
            }
        }
        std::sort(gaps.rbegin(), gaps.rend());
        if (gaps.size() > 8) {
            gaps.resize(8);
        }
        for (const auto &gap_row : gaps) {
            const Id blocked = std::get<1>(gap_row);
            const auto &pipe = std::get<2>(gap_row);
            Id count = 0;
            for (Id i = positions.at(blocked) + 1; i < Id(p.order.size()) && count < 2; ++i) {
                Id b = p.order[i];
                if (p.core_of.at(b) == p.core_of.at(blocked) && blocks[b][pipe] > 0) {
                    add("fifo", "fifo_" + key(blocked) + "_" + key(b),
                        [&, b, blocked] { return insert(g, p, b, positions.at(blocked)); });
                    ++count;
                }
            }
        }
        auto transfers = official_result.value("cross_core_transfers", array());
        std::stable_sort(transfers.begin(), transfers.end(), [](const Json &a, const Json &b) {
            return std::make_tuple(-a.at("copy_in_end").get<double>(), -field_id(a, "size"),
                                   field_id(a, "tensor_id")) <
                   std::make_tuple(-b.at("copy_in_end").get<double>(), -field_id(b, "size"),
                                   field_id(b, "tensor_id"));
        });
        for (std::size_t i = 0; i < std::min<std::size_t>(8, transfers.size()); ++i) {
            const auto &transfer = transfers[i];
            Id tid = field_id(transfer, "tensor_id");
            if (!g.tensors.count(tid)) {
                continue;
            }
            IdSet producers, consumers;
            for (Id v : g.eligible_producers.at(tid)) {
                if (p.core_of.at(owner.at(v)) == field_id(transfer, "source_core")) {
                    producers.insert(owner.at(v));
                }
            }
            for (Id v : g.eligible_consumers.at(tid)) {
                if (p.core_of.at(owner.at(v)) == field_id(transfer, "target_core")) {
                    consumers.insert(owner.at(v));
                }
            }
            if (producers.empty() || consumers.empty()) {
                continue;
            }
            auto compare = [&](Id a, Id b) { return positions.at(a) < positions.at(b); };
            Id a = *std::max_element(producers.begin(), producers.end(), compare),
               b = *std::min_element(consumers.begin(), consumers.end(), compare), tail = b;
            Ids chain = {a, b};
            for (int j = 0; j < 2; ++j) {
                IdSet successors;
                for (Id op : p.blocks.at(tail)) {
                    for (Id v : g.succs.at(op)) {
                        if (std::find(chain.begin(), chain.end(), owner.at(v)) == chain.end()) {
                            successors.insert(owner.at(v));
                        }
                    }
                }
                if (successors.empty()) {
                    break;
                }
                auto rank = [&](Id c) {
                    Id r = 0;
                    for (Id v : p.blocks.at(c)) {
                        r = std::max(r, g.rank.at(v));
                    }
                    return std::make_pair(r, -c);
                };
                tail = *std::max_element(successors.begin(), successors.end(),
                                         [&](Id x, Id y) { return rank(x) < rank(y); });
                chain.push_back(tail);
            }
            IdSet targets;
            for (Id c : chain) {
                targets.insert(p.core_of.at(c));
            }
            for (Id target : targets) {
                add(
                    "chain", "chain_t" + key(tid) + "_" + key(target),
                    [&, chain, target] { return migrate(p, chain, target); }, true);
                add(
                    "chain", "pair_t" + key(tid) + "_" + key(target),
                    [&, chain, target] { return migrate(p, {chain[0], chain[1]}, target); }, true);
            }
            add("release", "early_source_t" + key(tid), [&, a] { return insert(g, p, a, 0); });
            Ids candidates;
            for (Id j = positions.at(a) + 1; j < positions.at(b); ++j) {
                if (std::find(chain.begin(), chain.end(), p.order[j]) == chain.end()) {
                    candidates.push_back(p.order[j]);
                }
            }
            auto io = [&](Id c) {
                IdSet tids;
                for (Id v : p.blocks.at(c)) {
                    tids.insert(g.op_inputs.at(v).begin(), g.op_inputs.at(v).end());
                    tids.insert(g.op_outputs.at(v).begin(), g.op_outputs.at(v).end());
                }
                Id size = 0;
                for (Id t : tids) {
                    size += field_id(g.tensors.at(t), "size");
                }
                return size;
            };
            std::sort(candidates.begin(), candidates.end(), [&](Id x, Id y) {
                return std::make_pair(-io(x), x) < std::make_pair(-io(y), y);
            });
            for (std::size_t j = 0; j < std::min<std::size_t>(2, candidates.size()); ++j) {
                Id c = candidates[j];
                add("bandwidth", "bandwidth_t" + key(tid) + "_" + key(c),
                    [&, c, b] { return insert(g, p, c, positions.at(b)); });
            }
        }
    }
    std::vector<Candidate> result;
    for (std::size_t index = 0; result.size() < 48; ++index) {
        bool changed = false;
        for (const auto &[kind, pool] : pools) {
            if (index < pool.size()) {
                const auto &f = pool[index];
                auto candidate = f.build();
                if (candidate) {
                    result.push_back({f.kind, f.label, *candidate, f.repair});
                }
                changed = true;
                if (result.size() == 48) {
                    break;
                }
            }
        }
        if (!changed) {
            break;
        }
    }
    return result;
}
// 搜索只接纳通过完整官方评估的方案。草图估计负责排序，不能写入最终评分。
// 截止或达到评估次数上限时，保留此前原子写入的最好检查点。
// 两个入口共用下面的算法实现，差别仅在输入与结果持久化方式。
// memory_plan 非空时使用调用方已经解析的图及固定参数，结果直接交回调用方。
static Json solve_impl(const fs::path &graph_path, Id cores, const fs::path &config_path,
                       const fs::path &output, const SearchOptions &options,
                       const GraphInfo *provided_graph = nullptr,
                       const Settings *provided_settings = nullptr, Json *memory_plan = nullptr) {
    const bool memory_only = memory_plan != nullptr;
    if (memory_only && (!options.warm_start.empty() || !options.inherited_checkpoint.empty() ||
                        !options.resume_checkpoint.empty())) {
        throw std::invalid_argument("内存入口不接受外部检查点或预生成方案");
    }
    struct EvaluationLimit : std::exception {};
    double started = monotonic(), time_limit = options.time_limit,
           grace = cores == 1 ? 0 : options.feasibility_grace;
    if (!std::isfinite(time_limit) || time_limit <= 0 || !std::isfinite(grace) || grace < 0 ||
        grace > 120) {
        throw std::invalid_argument("invalid search target or feasibility grace");
    }
    double soft_deadline = started + time_limit, deadline = soft_deadline + grace,
           last_saved = -std::numeric_limits<double>::infinity();
    execution_deadline = deadline;
    if (!memory_only) {
        fs::create_directories(output);
    }
    Json stats = object();
    for (const char *name : {"full_evaluations",
                             "invalid_evaluations",
                             "timeout_evaluations",
                             "expansions",
                             "invalid_expansions",
                             "generated",
                             "duplicates",
                             "safe_pruned",
                             "accepted",
                             "batches",
                             "primary_improvements",
                             "secondary_improvements",
                             "seed_candidates",
                             "elite_restarts",
                             "candidate_pages",
                             "construction_seconds",
                             "screening_seconds",
                             "expansion_seconds",
                             "evaluation_seconds",
                             "generation_seconds",
                             "repair_seconds",
                             "validation_seconds",
                             "record_seconds",
                             "checkpoint_seconds",
                             "coupled_reorders",
                             "coupled_reorder_seconds",
                             "secondary_centers_expanded",
                             "seed_reorders",
                             "seed_reorder_seconds",
                             "local_improvements",
                             "route_steps",
                             "repair_skipped",
                             "activation_rounds"}) {
        stats[name] = 0;
    }
    Json record = {{"status", "running"},
                   {"solver_version", "scene-b-v5-case-budget"},
                   {"time_limit", time_limit},
                   {"ablation", options.ablation},
                   {"feasibility_grace_seconds", grace},
                   {"search_profile", options.compact ? "compact" : "full"},
                   {"warm_start", options.warm_start.empty()
                                      ? Json()
                                      : Json(path_utf8(options.warm_start.filename()))},
                   {"stats", stats},
                   {"history", array()}};
    std::optional<Plan> best_plan;
    Json best_result, last_result;
    Objective best_key = {std::numeric_limits<double>::infinity(), std::numeric_limits<Id>::max()};
    std::map<std::string, std::string> evaluated;
    std::map<std::string, Json> profile_cache, result_cache;
    std::map<std::string, std::pair<double, Id>> proxy_cache;
    std::deque<std::string> result_order, profile_order;
    std::vector<double> durations;
    Route *current_route = nullptr;
    std::vector<std::unique_ptr<Route>> routes;
    auto increment = [&](const char *name, Id amount = 1) {
        stats[name] = stats.at(name).get<Id>() + amount;
    };
    auto timing = [&](const char *name, double seconds) {
        stats[name] = stats.at(name).get<double>() + seconds;
    };
    auto recent = [&](std::size_t n, double fallback) {
        double value = fallback;
        if (!durations.empty()) {
            value = 0;
            for (std::size_t i = durations.size() > n ? durations.size() - n : 0;
                 i < durations.size(); ++i) {
                value = std::max(value, durations[i]);
            }
        }
        return value;
    };
    auto check = [&](bool reserve = false) {
        if (deadline - monotonic() <= (reserve ? std::max(1., 1.25 * recent(3, 0)) : 0)) {
            throw TimeoutError("search deadline reached");
        }
        if (options.max_evaluations &&
            field_id(stats, "full_evaluations") >= options.max_evaluations) {
            throw EvaluationLimit();
        }
    };
    auto save_record = [&](bool force = false) {
        double began = monotonic();
        if (options.compact && !force && began - last_saved < 2) {
            return;
        }
        last_saved = began;
        record["time_seconds"] = monotonic() - started;
        record["stats"] = stats;
        if (!routes.empty()) {
            record["routes"] = array();
            for (const auto &route : routes) {
                record["routes"].push_back(route->stats);
            }
        }
        if (!memory_only) {
            atomic_write(output / "metrics.json", record);
        }
        timing("record_seconds", monotonic() - began);
    };
    std::exception_ptr failure;
    try {
        save_record();
        std::optional<GraphInfo> loaded_graph;
        if (!provided_graph) {
            loaded_graph.emplace(GraphInfo::from_path(graph_path));
        }
        const GraphInfo &graph = provided_graph ? *provided_graph : *loaded_graph;
        const auto settings =
            provided_settings ? *provided_settings : official::load_settings(config_path);
        // 内存入口不继承检查点，因而也无需读取输入或配置文件来计算指纹。
        const auto instance = memory_only ? Json() : instance_fingerprint(graph_path, config_path);
        record["problem_instance"] = instance;
        // 统一处理去重、校验、正式评估与保优，所有搜索路线共享该入口。
        auto evaluate = [&](const Plan &candidate, const std::string &source,
                            std::optional<Objective> expected = std::nullopt) {
            check();
            double validation = monotonic();
            candidate.validate(graph);
            auto signature = candidate.fingerprint();
            timing("validation_seconds", monotonic() - validation);
            if (evaluated.count(signature) &&
                (result_cache.count(signature) || evaluated.at(signature) != "ok")) {
                increment("duplicates");
                last_result = result_cache.count(signature) ? result_cache.at(signature) : Json();
                return 0;
            }
            double began = monotonic();
            increment("full_evaluations");
            auto result = official::evaluate(graph.graph, candidate.to_dict(), settings);
            last_result = result;
            double elapsed = monotonic() - began;
            durations.push_back(elapsed);
            timing("evaluation_seconds", elapsed);
            evaluated[signature] = field_string(result, "status");
            if (!result_cache.count(signature)) {
                result_order.push_back(signature);
            }
            result_cache[signature] = result;
            while (result_order.size() > std::size_t(options.compact ? 12 : 64)) {
                result_cache.erase(result_order.front());
                result_order.pop_front();
            }
            Json entry = {{"source", source},
                          {"status", result.at("status")},
                          {"seconds", elapsed},
                          {"elapsed_seconds", monotonic() - started},
                          {"route", current_route ? current_route->name : "construction"}};
            int improved = 0;
            if (result.at("status") == "ok") {
                auto score = objective(result);
                if (expected) {
                    if (score != *expected) {
                        record["inherited_validation"]["status"] = "failed";
                        record["inherited_validation"]["revalidated_makespan"] = score.first;
                        throw std::invalid_argument(
                            "idle-core embedding changed official objective");
                    }
                    record["inherited_validation"]["status"] = "verified";
                    record["inherited_validation"]["revalidated_makespan"] = score.first;
                    record["inherited_validation"]["revalidated_added_copy_bytes"] = score.second;
                }
                entry["makespan"] = score.first;
                entry["added_copy_bytes"] = score.second;
                if (score < best_key) {
                    improved = score.first < best_key.first ? 2 : 1;
                    Json checkpoint = {
                        {"plan", candidate.to_dict()},
                        {"official_result", result.at("official_result")},
                        {"makespan", result.at("makespan")},
                        {"source", source},
                        {"elapsed_seconds", monotonic() - started},
                        {"problem_instance", instance},
                        {"inherited_validation", record.value("inherited_validation", Json())}};
                    double checkpoint_started = monotonic();
                    if (!memory_only) {
                        atomic_write(output / "best_checkpoint.json", checkpoint);
                    }
                    timing("checkpoint_seconds", monotonic() - checkpoint_started);
                    bool first = !best_plan;
                    best_key = score;
                    best_plan = candidate;
                    best_result = result;
                    if (first) {
                        record["first_solution_seconds"] = monotonic() - started;
                        record["feasibility_extension_used"] = monotonic() > soft_deadline;
                        deadline = soft_deadline;
                        execution_deadline = deadline;
                    }
                    increment(improved == 2 ? "primary_improvements" : "secondary_improvements");
                    increment("accepted");
                    IdSet active;
                    for (auto [b, c] : candidate.core_of) {
                        active.insert(c);
                    }
                    record["makespan"] = result.at("makespan");
                    record["source"] = source;
                    record["solution_available"] = true;
                    record["active_cores"] = active.size();
                    record["data_movement_bytes"] =
                        result.at("official_result").at("data_movement_bytes");
                }
            } else {
                increment(result.at("status") == "timeout" ? "timeout_evaluations"
                                                           : "invalid_evaluations");
                entry["error"] = result.value("error", Json());
            }
            record["history"].push_back(entry);
            save_record(improved != 0);
            if (!memory_only) {
                std::cout << source << ": "
                          << (entry.contains("makespan") ? entry.at("makespan").dump()
                                                         : field_string(entry, "status"))
                          << "; best=" << best_key.first << std::endl;
            }
            if (best_plan && monotonic() >= soft_deadline) {
                throw TimeoutError("search deadline reached");
            }
            return improved;
        };
        auto expand = [&](const Plan &candidate, const Json &result = Json()) {
            check(true);
            auto signature = candidate.fingerprint();
            if (profile_cache.count(signature)) {
                return profile_cache.at(signature);
            }
            double began = monotonic();
            increment("expansions");
            auto profile =
                official::build_profile(graph.graph, candidate.to_dict(), settings, result);
            timing("expansion_seconds", monotonic() - began);
            profile_cache[signature] = profile;
            profile_order.push_back(signature);
            if (profile.value("status", std::string("ok")) != "ok") {
                increment("invalid_expansions");
            }
            return profile;
        };
        auto cheap = [&](const Plan &candidate) {
            auto signature = candidate.fingerprint();
            if (proxy_cache.count(signature)) {
                return proxy_cache.at(signature);
            }
            double estimate = replay_sketch(graph, candidate, settings);
            Id route = route_bytes(graph, candidate);
            auto score = std::make_pair(std::max(estimate, route / settings.bandwidth), route);
            proxy_cache[signature] = score;
            return score;
        };
        Plan fallback{{{0, graph.id_order}}, {{0, 0}}, {0}, 1, cores};
        std::optional<Plan> inherited;
        if (!options.inherited_checkpoint.empty() || !options.resume_checkpoint.empty()) {
            if (!options.inherited_checkpoint.empty() && !options.resume_checkpoint.empty()) {
                throw std::invalid_argument("choose inherited or resume checkpoint");
            }
            bool resume = !options.resume_checkpoint.empty();
            auto [plan, metadata] = load_inherited(
                graph, resume ? options.resume_checkpoint : options.inherited_checkpoint, cores,
                instance, resume);
            if (resume && field_id(metadata, "source_cores") != cores) {
                throw std::invalid_argument("resume checkpoint must have the same core count");
            }
            inherited = plan;
            metadata["mode"] = resume ? "resume" : "inherit";
            record["inherited_validation"] = metadata;
            std::string label = resume ? "resumed_" + key(cores)
                                       : "inherited_" + key(field_id(metadata, "source_cores")) +
                                             "_to_" + key(cores);
            evaluate(plan, label,
                     Objective{metadata.at("source_makespan").get<double>(),
                               field_id(metadata, "source_added_copy_bytes")});
            if (!best_plan) {
                record["inherited_validation"]["status"] = "failed";
                throw std::runtime_error("inherited plan failed official revalidation");
            }
        } else {
            if (options.compact && cores > 1) {
                double began = monotonic();
                auto bootstrap = first_plan(graph, cores);
                timing("construction_seconds", monotonic() - began);
                evaluate(bootstrap, "native_bootstrap");
            }
            if (!best_plan) {
                evaluate(fallback, "whole_graph_fallback");
            }
        }
        Work totals;
        std::map<Id, double> tail;
        double longest = 0;
        for (auto i = graph.id_order.rbegin(); i != graph.id_order.rend(); ++i) {
            double raw = field_id(graph.ops.at(*i), "cycles"), child = 0;
            totals[field_string(graph.ops.at(*i), "pipe")] += raw;
            for (Id w : graph.succs.at(*i)) {
                child = std::max(child, tail.at(w));
            }
            tail[*i] = raw + child;
            longest = std::max(longest, tail.at(*i));
        }
        double global_bound = std::max(
            {maximum(totals) / cores, longest, route_bytes(graph, fallback) / settings.bandwidth});
        record["global_lower_bound"] = global_bound;
        if (cores == 1) {
            record["termination"] = "fixed_singlecore";
        } else {
            auto row_for = [&](const Plan &plan,
                               const std::string &label) -> std::optional<SearchRow> {
                if (last_result.is_object() && last_result.at("status") == "ok") {
                    return SearchRow{objective(last_result), plan, last_result, label};
                }
                return {};
            };
            std::vector<SearchRow> seed_rows;
            if (best_plan) {
                auto row = row_for(*best_plan, record.value("source", std::string()));
                if (row) {
                    seed_rows.push_back(*row);
                }
            }
            // 生成器保存每条种子来源的推进位置，按轮次交替尝试不同构造策略。
            auto legacy = [&]() -> Generator<std::pair<std::string, Plan>> {
                return Generator<std::pair<std::string, Plan>>([&](const auto &yield) {
                    for (double weight : {0., 1.}) {
                        auto plan = construct(graph, cores, weight, settings);
                        yield(std::make_pair(
                            std::string("native_lambda_") + (weight == 0 ? "0" : "1"), plan));
                    }
                });
            };
            auto structure_source = [&]() -> Generator<std::pair<std::string, Plan>> {
                return Generator<std::pair<std::string, Plan>>([&](const auto &yield) {
                    for (auto row : structure_seed_plans(graph, cores, settings, deadline)) {
                        yield(row);
                    }
                });
            };
            auto pipeline_source = [&]() -> Generator<std::pair<std::string, Plan>> {
                return Generator<std::pair<std::string, Plan>>([&](const auto &yield) {
                    for (auto row : pipeline_seed_plans(graph, cores, settings, deadline)) {
                        yield(row);
                    }
                });
            };
            auto activation_source = [&]() -> Generator<std::pair<std::string, Plan>> {
                return Generator<std::pair<std::string, Plan>>([&](const auto &yield) {
                    for (auto row : activation_plans(graph, *inherited, settings, deadline, 6)) {
                        yield(row);
                    }
                });
            };
            std::vector<Generator<std::pair<std::string, Plan>>> sources;
            if (options.ablation != "construction-only") {
                sources.push_back(structure_source());
                sources.push_back(pipeline_source());
            }
            sources.push_back(legacy());
            if (inherited && options.ablation != "construction-only") {
                sources.push_back(activation_source());
            }
            std::vector<std::size_t> active_sources(sources.size());
            std::iota(active_sources.begin(), active_sources.end(), 0);
            double seed_cutoff = started + std::min(time_limit * .25, 180.);
            Id seed_limit = options.compact ? (inherited ? 8 : 6) : (inherited ? 24 : 18),
               seed_count = 0;
            while (!active_sources.empty() && seed_count < seed_limit) {
                std::vector<std::size_t> active;
                for (auto index : active_sources) {
                    check(true);
                    double began = monotonic();
                    auto row = sources[index].next();
                    if (!row) {
                        continue;
                    }
                    timing("construction_seconds", monotonic() - began);
                    active.push_back(index);
                    ++seed_count;
                    increment("seed_candidates");
                    evaluate(row->second, row->first);
                    auto seed = row_for(row->second, row->first);
                    if (seed) {
                        seed_rows.push_back(*seed);
                    }
                    if (seed_count >= seed_limit ||
                        (seed_count >= 4 && monotonic() >= seed_cutoff)) {
                        active.clear();
                        break;
                    }
                }
                active_sources = active;
            }
            // 种子阶段结束即释放休眠线程和候选列表。
            sources.clear();
            if (!options.warm_start.empty()) {
                auto plan = Plan::from_dict(graph, read_json_strict(options.warm_start));
                if (plan.ncores != cores) {
                    throw std::invalid_argument("warm-start core count mismatch");
                }
                evaluate(plan, "explicit_warm_start");
                auto row = row_for(plan, "explicit_warm_start");
                if (row) {
                    seed_rows.push_back(*row);
                }
            }
            if (!best_plan) {
                throw NoFeasiblePlanError("no officially executable seed found");
            }
            auto better = [](const SearchRow &a, const SearchRow &b) { return a.score < b.score; };
            std::vector<SearchRow> native;
            for (const auto &row : seed_rows) {
                if (row.label.rfind("structure_", 0) == 0 || row.label.rfind("pipe_", 0) == 0 ||
                    row.label.rfind("native_", 0) == 0) {
                    native.push_back(row);
                }
            }
            auto representatives = polishing_seeds(graph, seed_rows, options.compact ? 3 : 8);
            if (representatives.empty()) {
                representatives.push_back(
                    *std::min_element(seed_rows.begin(), seed_rows.end(), better));
            }
            auto reference_seed = native.empty()
                                      ? representatives.front()
                                      : *std::min_element(native.begin(), native.end(), better);
            std::vector<SearchRow> polished, reference_variants;
            double polish_cutoff = started + std::min(time_limit * .45, 360.);
            for (std::size_t i = 0; i < representatives.size(); ++i) {
                if (i && monotonic() >= polish_cutoff) {
                    polished.insert(polished.end(), representatives.begin() + i,
                                    representatives.end());
                    break;
                }
                const auto &row = representatives[i];
                std::vector<SearchRow> variants = {row};
                if (options.ablation != "construction-only" && options.ablation != "no-repair") {
                    double began = monotonic();
                    for (auto [label, candidate] : reorder_plans(
                             graph, row.plan, settings, row.result, options.compact ? 1 : 3)) {
                        check(true);
                        increment("seed_reorders");
                        auto name = row.label + "_" + label;
                        evaluate(candidate, name);
                        auto candidate_row = row_for(candidate, name);
                        if (candidate_row) {
                            variants.push_back(*candidate_row);
                        }
                    }
                    timing("seed_reorder_seconds", monotonic() - began);
                }
                polished.push_back(*std::min_element(variants.begin(), variants.end(), better));
                if (row.plan.fingerprint() == reference_seed.plan.fingerprint()) {
                    reference_variants = variants;
                }
            }
            auto reference_rows = native;
            append(reference_rows, reference_variants);
            if (reference_rows.empty()) {
                reference_rows.push_back(reference_seed);
            }
            routes.push_back(std::make_unique<Route>(graph, "native_reference", reference_rows,
                                                     true, options.compact));
            routes.push_back(std::make_unique<Route>(graph, "route_native_joint",
                                                     std::vector<SearchRow>{routes.front()->best},
                                                     false, options.compact));
            std::set<Ids> route_maps = {assignment_key(graph, routes.front()->best.plan)};
            for (const auto &row : diverse_rows(graph, polished, 8)) {
                if (!route_maps.insert(assignment_key(graph, row.plan)).second) {
                    continue;
                }
                routes.push_back(std::make_unique<Route>(graph, "route_" + row.label,
                                                         std::vector<SearchRow>{row}, false,
                                                         options.compact));
                if (routes.size() >= std::size_t(options.compact ? 4 : 6)) {
                    break;
                }
            }
            record["portfolio_policy"] =
                "protected native route; independent local objectives; time-weighted fair service";
            auto assess = [&](Route &route, const Plan &candidate,
                              const std::string &label) -> std::pair<int, Json> {
                evaluate(candidate, label);
                auto row = row_for(candidate, label);
                if (!row) {
                    return {0, Json()};
                }
                int improvement = route.offer(*row);
                if (improvement) {
                    increment("local_improvements");
                }
                return {improvement, row->result};
            };
            auto joint = [&](Route &route, const Plan &plan, const Json &result,
                             const std::string &label) -> std::pair<int, Json> {
                auto mapping = assignment_key(graph, plan);
                if (route.reference || options.ablation == "no-repair" ||
                    route.reordered.count(mapping) ||
                    field_id(route.stats, "fifo_calls") >= (options.compact ? 4 : 16)) {
                    return {0, Json()};
                }
                route.reordered.insert(mapping);
                check(true);
                double began = monotonic();
                auto variants = reorder_plans(graph, plan, settings, result, 1);
                timing("coupled_reorder_seconds", monotonic() - began);
                if (variants.empty()) {
                    return {0, Json()};
                }
                route.stats["fifo_calls"] = field_id(route.stats, "fifo_calls") + 1;
                increment("coupled_reorders");
                return assess(route, variants.front().second, label + "_fifo_joint");
            };
            auto repair = [&](Route &route, const Plan &plan, const Json &result,
                              const std::string &label) -> std::pair<int, Json> {
                auto signature = plan.fingerprint();
                if (options.ablation == "no-repair" || !route.repair_allowed(signature)) {
                    increment("repair_skipped");
                    return {0, Json()};
                }
                route.repaired.insert(signature);
                route.stats["repair_calls"] = field_id(route.stats, "repair_calls") + 1;
                double began = monotonic();
                std::pair<int, Json> outcome = {0, Json()};
                auto finish = [&]() {
                    route.repair_cost += monotonic() - began;
                    route.stats["repair_seconds"] = route.repair_cost;
                    route.repair_failures = outcome.first ? 0 : route.repair_failures + 1;
                };
                try {
                    auto profile = expand(plan, result);
                    if (profile.value("status", std::string("ok")) == "ok") {
                        double t = monotonic();
                        auto candidate = repair_order(graph, plan, settings, profile, {}, 2);
                        timing("repair_seconds", monotonic() - t);
                        if (candidate) {
                            outcome = assess(route, *candidate, label);
                        }
                    }
                } catch (...) {
                    finish();
                    throw;
                }
                finish();
                return outcome;
            };
            // 每次 yield 交回控制权，让其他路线获得探索机会；其余路线保持休眠。
            auto explore = [&](Route &route) -> Generator<int> {
                return Generator<int>([&, route_ptr = &route](const auto &yield) {
                    auto &route = *route_ptr;
                    route.stats["status"] = "searching";
                    while (true) {
                        check(true);
                        auto row = route.next_center();
                        if (!row) {
                            route.stats["status"] = "restricted_neighborhood_exhausted";
                            return;
                        }
                        Plan center = row->plan;
                        Json center_result = row->result;
                        if (route.visited.size() > 1) {
                            increment("elite_restarts");
                        }
                        if (row->score > route.best.score) {
                            increment("secondary_centers_expanded");
                        }
                        auto outcome = joint(route, center, center_result, "center");
                        yield(0);
                        if (outcome.first == 2) {
                            continue;
                        }
                        auto mapping = assignment_key(graph, center);
                        IdSet occupied;
                        for (auto [b, c] : center.core_of) {
                            occupied.insert(c);
                        }
                        if (!route.reference && Id(occupied.size()) < cores &&
                            !route.activated.count(mapping) && route.activated.size() < 4) {
                            route.activated.insert(mapping);
                            increment("activation_rounds");
                            auto activation_candidates =
                                activation_plans(graph, center, settings, deadline, 3);
                            for (const auto &activation_candidate : activation_candidates) {
                                const auto &label = activation_candidate.first;
                                const auto &candidate = activation_candidate.second;
                                route.stats["activation_calls"] =
                                    field_id(route.stats, "activation_calls") + 1;
                                auto assessed = assess(route, candidate, label);
                                int local = assessed.first;
                                Json result = std::move(assessed.second);
                                yield(0);
                                if (result.is_object() && result.at("makespan").get<double>() <=
                                                              route.best.score.first * 1.15) {
                                    auto extra = joint(route, candidate, result, label);
                                    yield(0);
                                    local = std::max(local, extra.first);
                                }
                                if (local == 2) {
                                    break;
                                }
                            }
                            if (route.best.score < row->score) {
                                continue;
                            }
                        }
                        for (int sweep = 0; sweep < (options.compact ? 1 : 2); ++sweep) {
                            bool primary = false;
                            for (int family : {1, 2, 0}) {
                                check(true);
                                increment("batches");
                                if (family == 0 && global_bound > 0 &&
                                    route.best.score.first <= global_bound * 1.01) {
                                    continue;
                                }
                                while (profile_order.size() > 8) {
                                    profile_cache.erase(profile_order.front());
                                    profile_order.pop_front();
                                }
                                auto profile = expand(center, center_result);
                                if (family == 0 && sweep == 0 &&
                                    (!profile.value("lifetime_hotspots", array()).empty() ||
                                     center_result.value("spill_bytes", Id(0)))) {
                                    auto local =
                                        repair(route, center, center_result, "lifetime_repair");
                                    yield(0);
                                    if (local.first == 2) {
                                        primary = true;
                                        break;
                                    }
                                }
                                double began = monotonic();
                                auto raw = neighborhoods(
                                    graph, center, profile, center_result.at("official_result"),
                                    family, settings, options.ablation == "no-group",
                                    family == 0 || options.ablation == "no-repair");
                                timing("generation_seconds", monotonic() - began);
                                began = monotonic();
                                std::vector<std::pair<std::pair<double, Id>, Candidate>> ranked;
                                std::set<std::string> seen;
                                for (const auto &candidate : raw) {
                                    check();
                                    increment("generated");
                                    auto signature = candidate.plan.fingerprint();
                                    bool pending = sweep > 0 && candidate.needs_repair &&
                                                   options.ablation != "no-repair";
                                    if (seen.count(signature) ||
                                        (route.evaluated.count(signature) && !pending)) {
                                        increment("duplicates");
                                        continue;
                                    }
                                    seen.insert(signature);
                                    try {
                                        candidate.plan.validate(graph);
                                    } catch (const std::invalid_argument &) {
                                        continue;
                                    }
                                    if (pipe_bound(graph, candidate.plan) >
                                        route.best.score.first) {
                                        increment("safe_pruned");
                                        continue;
                                    }
                                    ranked.push_back({cheap(candidate.plan), candidate});
                                }
                                timing("screening_seconds", monotonic() - began);
                                std::stable_sort(
                                    ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
                                        return std::make_pair(a.first, a.second.label) <
                                               std::make_pair(b.first, b.second.label);
                                    });
                                std::vector<std::pair<std::string, std::vector<Candidate>>> pools;
                                for (const auto &ranked_candidate : ranked) {
                                    const auto &candidate = ranked_candidate.second;
                                    auto i = std::find_if(pools.begin(), pools.end(),
                                                          [&](const auto &pool) {
                                                              return pool.first == candidate.kind;
                                                          });
                                    if (i == pools.end()) {
                                        pools.push_back({candidate.kind, {}});
                                        i = pools.end() - 1;
                                    }
                                    i->second.push_back(candidate);
                                }
                                std::vector<Candidate> ordered;
                                for (std::size_t i = 0;; ++i) {
                                    bool changed = false;
                                    for (const auto &[kind, pool] : pools) {
                                        if (i < pool.size()) {
                                            ordered.push_back(pool[i]);
                                            changed = true;
                                        }
                                    }
                                    if (!changed) {
                                        break;
                                    }
                                }
                                double cost = std::max(.05, recent(4, .05));
                                Id quota = std::min<Id>(
                                    options.compact ? 6 : 12,
                                    std::max<Id>(3, Id((deadline - monotonic()) / (24 * cost))));
                                if (sweep) {
                                    quota = std::min<Id>(18, quota * 2);
                                }
                                std::optional<std::pair<Plan, Json>> secondary;
                                for (Id i = 0; i < std::min<Id>(quota, Id(ordered.size())); ++i) {
                                    check(true);
                                    if (i % 3 == 0) {
                                        increment("candidate_pages");
                                    }
                                    const auto &candidate = ordered[i];
                                    auto assessed = assess(route, candidate.plan, candidate.label);
                                    int improvement = assessed.first;
                                    Json result = std::move(assessed.second);
                                    yield(0);
                                    if (improvement == 2) {
                                        primary = true;
                                        break;
                                    }
                                    if (improvement == 1) {
                                        secondary = {{candidate.plan, result}};
                                    }
                                    if (result.is_object() && result.at("status") == "ok" &&
                                        result.at("makespan").get<double>() <=
                                            route.best.score.first * 1.10 &&
                                        assignment_key(graph, candidate.plan) !=
                                            assignment_key(graph, center)) {
                                        auto local =
                                            joint(route, candidate.plan, result, candidate.label);
                                        yield(0);
                                        if (local.first == 2) {
                                            primary = true;
                                            break;
                                        }
                                        if (local.first == 1) {
                                            secondary = {{route.best.plan, route.best.result}};
                                        }
                                    }
                                    bool should =
                                        candidate.needs_repair && options.ablation != "no-repair" &&
                                        (sweep > 0 || (result.is_object() &&
                                                       result.value("spill_bytes", Id(0)) > 0));
                                    if (should) {
                                        auto local = repair(route, candidate.plan, result,
                                                            candidate.label + "_repair");
                                        yield(0);
                                        if (local.first == 2) {
                                            primary = true;
                                            break;
                                        }
                                        if (local.first == 1) {
                                            secondary = {{route.best.plan, route.best.result}};
                                        }
                                    }
                                }
                                if (primary) {
                                    break;
                                }
                                if (secondary) {
                                    center = secondary->first;
                                    center_result = secondary->second;
                                }
                            }
                            if (primary) {
                                break;
                            }
                        }
                    }
                });
            };
            std::string finished = options.ablation == "construction-only"
                                       ? "construction_only"
                                       : "restricted_neighborhood_exhausted";
            std::vector<Route *> active;
            std::map<std::string, Generator<int>> iterators;
            if (options.ablation != "construction-only") {
                for (auto &route : routes) {
                    active.push_back(route.get());
                    iterators.emplace(route->name, explore(*route));
                }
            }
            while (!active.empty()) {
                check(true);
                if (global_bound > 0 && best_key.first <= global_bound * 1.005) {
                    finished = "primary_bound_gap";
                    break;
                }
                current_route = select_route(active);
                double began = monotonic();
                auto update = [&]() {
                    current_route->seconds += monotonic() - began;
                    current_route->stats["seconds"] = current_route->seconds;
                    current_route->stats["steps"] = field_id(current_route->stats, "steps") + 1;
                    increment("route_steps");
                };
                try {
                    if (!iterators.at(current_route->name).next()) {
                        active.erase(std::find(active.begin(), active.end(), current_route));
                    }
                } catch (...) {
                    update();
                    throw;
                }
                update();
            }
            current_route = nullptr;
            record["termination"] = finished;
        }
    } catch (const TimeoutError &) {
        record["termination"] = "time_limit";
    } catch (const EvaluationLimit &) {
        record["termination"] = "evaluation_limit";
    } catch (const std::exception &error) {
        record["termination"] = "solver_error";
        record["error"] = safe_message(error.what());
        failure = std::current_exception();
    }
    execution_deadline = 0;
    record["status"] = best_plan ? (record.value("termination", std::string()) == "solver_error"
                                        ? "complete_with_error"
                                        : "complete")
                                 : "failed";
    record["solution_available"] = bool(best_plan);
    save_record(true);
    if (failure) {
        std::rethrow_exception(failure);
    }
    if (memory_only && best_plan) {
        *memory_plan = best_plan->to_dict();
    }
    if (!memory_only && options.export_results) {
        recover(output, path_utf8(graph_path.stem()), cores);
    }
    return record;
}
// 原开发接口原样保留，包括配置读取、检查点继承、日志和评分导出。
Json solve(const fs::path &graph, Id cores, const fs::path &config, const fs::path &output,
           const SearchOptions &options) {
    return solve_impl(graph, cores, config, output, options);
}

Json solve_in_memory(const GraphInfo &graph, Id cores, const SearchOptions &options) {
    const auto settings = submission_settings();
    Json plan;
    solve_impl({}, cores, {}, {}, options, &graph, &settings, &plan);
    return plan;
}
} // namespace problem2
