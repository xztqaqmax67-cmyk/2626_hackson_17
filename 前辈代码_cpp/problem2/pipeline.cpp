// 对应 pipeline.py。
// 按流水线就绪时间安排算子；同时生成不同粒度与顺序的起始方案。
#include "pipeline.hpp"
namespace problem2 {
namespace {
using PipeTimes = std::array<double, 5>;
std::size_t pipe_index(const std::string &pipe) {
    auto it = std::find(PIPES.begin(), PIPES.end(), pipe);
    if (it == PIPES.end()) throw std::invalid_argument("unknown pipe");
    return std::size_t(it - PIPES.begin());
}
struct Proposal {
    Id op = 0, core = 0, bytes = 0;
    double start = 0, end = 0;
    std::map<Id, PipeTimes> rows;
    std::map<std::string, double> routes;
};
class Placement {
    const GraphInfo &g;
    const Settings &settings;

public:
    std::vector<PipeTimes> available, fixed;
    std::map<Id, std::size_t> op_pipes;
    std::map<Id, double> finish;
    std::map<Id, Id> core;
    std::map<std::string, double> routes;
    Id bytes = 0;
    Placement(const GraphInfo &graph, Id cores, const Settings &cfg)
        : g(graph), settings(cfg), available(cores), fixed(cores) {
        for (Id op : g.ids) op_pipes[op] = pipe_index(field_string(g.ops.at(op), "pipe"));
    }
    Proposal trial(Id v, Id target) const {
        Proposal p;
        p.op = v;
        p.core = target;
        auto row = [&](Id c) -> PipeTimes & {
            if (!p.rows.count(c)) {
                p.rows[c] = available[c];
            }
            return p.rows[c];
        };
        auto copy = [&](std::string route, Id c, std::size_t pipe, Id size, double release) {
            if (routes.count(route)) {
                return routes.at(route);
            }
            if (p.routes.count(route)) {
                return p.routes.at(route);
            }
            double end = std::max(row(c)[pipe], release) + copy_duration(size, settings.bandwidth);
            row(c)[pipe] = end;
            p.routes[route] = end;
            p.bytes += size;
            return end;
        };
        auto transfer = [&](std::string route, Id source, Id size, double release) {
            route += ":" + key(source) + ":" + key(target);
            double out = copy("out:" + route, source, 1, size, release);
            return copy("in:" + route, target, 0, size,
                        out + settings.cross_core_copy_delay);
        };
        double ready = 0;
        for (Id pred : g.preds.at(v)) {
            ready = std::max(ready, finish.at(pred));
        }
        for (Id tid : g.op_inputs.at(v)) {
            Id size = field_id(g.tensors.at(tid), "size");
            const auto &producers = g.eligible_producers.at(tid);
            if (producers.empty()) {
                ready = std::max(ready, copy("input:" + key(tid) + ":" + key(target), target,
                                             0, size, 0));
                continue;
            }
            std::map<Id, double> releases;
            for (Id pred : producers) {
                releases[core.at(pred)] = std::max(releases[core.at(pred)], finish.at(pred));
            }
            for (auto [source, release] : releases) {
                ready = std::max(ready, source == target ? release
                                                         : transfer("tensor:" + key(tid), source,
                                                                    size, release));
            }
        }
        if (g.direct_inputs.count(v)) {
            for (auto [index, pred, size] : g.direct_inputs.at(v)) {
                if (core.at(pred) != target) {
                    ready = std::max(ready, transfer("direct:" + key(index), core.at(pred), size,
                                                     finish.at(pred)));
                }
            }
        }
        auto pipe = op_pipes.at(v);
        p.start = std::max(ready, row(target)[pipe]);
        p.end = p.start + g.weight.at(v);
        row(target)[pipe] = p.end;
        for (Id tid : g.op_outputs.at(v)) {
            if (g.original_output_relations.count(tid)) {
                bool complete = true;
                const auto &producers = g.eligible_producers.at(tid);
                for (Id pred : producers) {
                    if (pred != v && !finish.count(pred)) {
                        complete = false;
                    }
                }
                if (!complete) {
                    continue;
                }
                std::map<Id, double> releases;
                for (Id pred : producers) {
                    Id source = pred == v ? target : core.at(pred);
                    releases[source] =
                        std::max(releases[source], pred == v ? p.end : finish.at(pred));
                }
                for (auto [source, release] : releases) {
                    copy("output:" + key(tid) + ":" + key(source), source, 1,
                         field_id(g.tensors.at(tid), "size"), release);
                }
            }
        }
        return p;
    }
    void commit(const Proposal &p) {
        for (const auto &[c, row] : p.rows) {
            available[c] = row;
        }
        for (auto [route, end] : p.routes) {
            routes[route] = end;
        }
        finish[p.op] = p.end;
        core[p.op] = p.core;
        fixed[p.core][op_pipes.at(p.op)] += field_id(g.ops.at(p.op), "cycles");
        bytes += p.bytes;
    }
};
} // namespace
// 维护每条流水线的可用时间；每次只选择依赖已经满足的算子。
Plan operation_plan(const GraphInfo &g, Id cores, const Settings &settings, double balance,
                    Id window, const std::string &mode,
                    const std::optional<std::map<Id, Id>> &assignment, double deadline,
                    const std::map<Id, double> &starts) {
    if (cores < 1 || window < 1) {
        throw std::invalid_argument("cores and window must be positive");
    }
    Placement sketch(g, cores, settings);
    std::map<Id, Id> depth, degree;
    std::map<Id, std::vector<double>> priority;
    for (Id v : g.id_order) {
        Id d = 0;
        for (Id p : g.preds.at(v)) {
            d = std::max(d, depth.at(p) + g.weight.at(p));
        }
        depth[v] = d;
    }
    for (Id v : g.ids) {
        if (!starts.empty()) {
            priority[v] = {starts.count(v) ? starts.at(v) : std::numeric_limits<double>::infinity(),
                           -double(g.rank.at(v)), double(v)};
        } else if (mode == "breadth") {
            priority[v] = {double(depth.at(v)), -double(g.rank.at(v)), double(v)};
        } else if (mode == "id") {
            priority[v] = {double(v)};
        } else {
            priority[v] = {-double(g.rank.at(v)), double(v)};
        }
        degree[v] = Id(g.preds.at(v).size());
    }
    using Entry = std::pair<std::vector<double>, Id>;
    std::set<Entry> heap;
    for (Id v : g.ids) {
        if (!degree[v]) {
            heap.insert({priority.at(v), v});
        }
    }
    Ids order;
    while (!heap.empty()) {
        check_deadline(deadline);
        std::vector<Entry> candidates;
        for (Id i = 0; i < window && !heap.empty(); ++i) {
            candidates.push_back(*heap.begin());
            heap.erase(heap.begin());
        }
        std::optional<Proposal> chosen;
        std::pair<double, std::vector<double>> chosen_key;
        for (const auto &[prio, v] : candidates) {
            auto pipe = sketch.op_pipes.at(v);
            std::optional<Proposal> best;
            std::tuple<double, Id, double, Id> best_score;
            for (Id c = 0; c < cores; ++c) {
                if (assignment && assignment->at(v) != c) {
                    continue;
                }
                auto trial = sketch.trial(v, c);
                double load = 0;
                for (std::size_t p = 0; p < PIPES.size(); ++p) {
                    load = std::max(load, sketch.fixed[c][p] + (p == pipe ? g.weight.at(v) : 0));
                }
                auto score = std::make_tuple(trial.end + balance * load, trial.bytes, load, c);
                if (!best || score < best_score) {
                    best = std::move(trial);
                    best_score = score;
                }
            }
            auto score = std::make_pair(best->start, prio);
            if (!chosen || score < chosen_key) {
                chosen = std::move(*best);
                chosen_key = score;
            }
        }
        Id v = chosen->op;
        for (const auto &entry : candidates) {
            if (entry.second != v) {
                heap.insert(entry);
            }
        }
        sketch.commit(*chosen);
        order.push_back(v);
        for (Id successor : g.succs.at(v)) {
            if (--degree[successor] == 0) {
                heap.insert({priority.at(successor), successor});
            }
        }
    }
    Plan p;
    p.ncores = cores;
    p.order = order;
    p.core_of = sketch.core;
    p.next_block_id = g.ids.empty() ? 0 : g.ids.back() + 1;
    for (Id v : g.ids) {
        p.blocks[v] = {v};
    }
    p.validate(g);
    return p;
}
// 依照原版顺序枚举窗口与负载权重，保留不同分配方式作为搜索起点。
void visit_pipeline_seeds(const GraphInfo &g, Id cores, const Settings &settings,
                          double deadline, const SeedVisitor &visit) {
    std::set<std::string> seen;
    for (auto [label, mode, balance, window] :
         std::vector<std::tuple<std::string, std::string, double, Id>>{
             {"pipe_critical_eft", "critical", 0, 1},
             {"pipe_ready_window", "critical", 0, 8},
             {"pipe_balanced_window", "critical", .5, 8},
             {"pipe_breadth_balanced", "breadth", .5, 8}}) {
        check_deadline(deadline);
        auto p = operation_plan(g, cores, settings, balance, window, mode, {}, deadline);
        if (seen.insert(p.fingerprint()).second) {
            if (!visit(label, std::move(p))) return;
        }
    }
}
NamedPlans pipeline_seed_plans(const GraphInfo &g, Id cores, const Settings &settings,
                               double deadline) {
    NamedPlans result;
    visit_pipeline_seeds(g, cores, settings, deadline, [&](std::string label, Plan p) {
        result.emplace_back(std::move(label), std::move(p));
        return true;
    });
    return result;
}
// 保持分配关系，借助已评估的开始时间尝试改善同核顺序。
NamedPlans reorder_plans(const GraphInfo &g, const Plan &p, const Settings &settings,
                         const Json &result, Id limit) {
    auto owners = p.owner();
    std::map<Id, Id> assignment;
    for (Id v : g.ids) {
        assignment[v] = p.core_of.at(owners.at(v));
    }
    std::map<Id, double> measured;
    std::set<std::string> seen = {p.fingerprint()};
    if (result.is_object()) {
        const auto &official =
            result.contains("official_result") ? result.at("official_result") : result;
        for (const auto &c : official.value("per_core_timeline", array())) {
            for (const auto &op : c.value("ops", array())) {
                if (g.eligible.count(field_id(op, "op_id"))) {
                    measured[field_id(op, "op_id")] = op.at("start").get<double>();
                }
            }
        }
    }
    std::vector<std::pair<std::string, Id>> variants = {
        {"critical", 8}, {"breadth", 8}, {"id", 4}, {"critical", 1}};
    if (!measured.empty()) {
        variants.insert(variants.begin(), {"measured", 8});
    }
    NamedPlans output;
    for (auto [mode, window] : variants) {
        if (Id(output.size()) >= std::max<Id>(0, limit)) {
            break;
        }
        auto candidate = operation_plan(g, p.ncores, settings, 0, window, mode, assignment, 0,
                                        mode == "measured" ? measured : std::map<Id, double>{});
        if (seen.insert(candidate.fingerprint()).second) {
            output.emplace_back("fifo_" + mode + "_" + key(window), candidate);
        }
    }
    return output;
}
} // namespace problem2
