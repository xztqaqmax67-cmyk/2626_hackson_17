// 对应 structure.py。
// 利用图的层次、分支和分隔结构生成分块方案。
#include "structure.hpp"
namespace problem2 {
std::map<Id, Id> levels(const GraphInfo &g) {
    std::map<Id, Id> result;
    for (Id v : g.id_order) {
        Id depth = -1;
        for (Id p : g.preds.at(v)) {
            depth = std::max(depth, result.at(p));
        }
        result[v] = depth + 1;
    }
    return result;
}
// 按阶段切分弱连接分支，使具有较强内部依赖的算子尽量留在一起。
std::pair<std::vector<Ids>, Ids> stage_branches(const GraphInfo &g, std::optional<Id> width,
                                                bool reverse, double deadline) {
    if (!width) {
        return {g.atoms, Ids(g.atoms.size(), 0)};
    }
    if (*width < 1) {
        throw std::invalid_argument("stage width must be positive");
    }
    std::map<Id, Id> stage, owner;
    if (reverse) {
        std::map<Id, Id> height;
        for (auto i = g.id_order.rbegin(); i != g.id_order.rend(); ++i) {
            Id h = -1;
            for (Id s : g.succs.at(*i)) {
                h = std::max(h, height.at(s));
            }
            height[*i] = h + 1;
        }
        for (Id v : g.ids) {
            stage[v] = -(height.at(v) / *width);
        }
    } else {
        for (auto [v, depth] : levels(g)) {
            stage[v] = depth / *width;
        }
    }
    std::vector<Ids> parts;
    Ids stages;
    for (Id root : g.id_order) {
        check_deadline(deadline);
        if (owner.count(root)) {
            continue;
        }
        Id b = Id(parts.size());
        parts.push_back({});
        stages.push_back(stage.at(root));
        owner[root] = b;
        Ids stack = {root};
        while (!stack.empty()) {
            Id v = stack.back();
            stack.pop_back();
            IdSet adjacent = g.preds.at(v);
            adjacent.insert(g.succs.at(v).begin(), g.succs.at(v).end());
            for (Id w : adjacent) {
                if (!owner.count(w) && stage.at(w) == stage.at(root)) {
                    owner[w] = b;
                    stack.push_back(w);
                }
            }
        }
    }
    for (Id v : g.id_order) {
        parts[owner.at(v)].push_back(v);
    }
    return {parts, stages};
}
Ids reverse_dfs_order(const GraphInfo &g) {
    IdSet seen;
    Ids result, roots = g.ids;
    std::sort(roots.begin(), roots.end(), [&](Id a, Id b) {
        return std::make_tuple(!g.succs.at(a).empty(), -g.position.at(a), a) <
               std::make_tuple(!g.succs.at(b).empty(), -g.position.at(b), b);
    });
    for (Id root : roots) {
        if (seen.count(root)) {
            continue;
        }
        std::vector<std::pair<Id, bool>> stack = {{root, false}};
        while (!stack.empty()) {
            auto [v, expanded] = stack.back();
            stack.pop_back();
            if (expanded) {
                if (seen.insert(v).second) {
                    result.push_back(v);
                }
                continue;
            }
            if (seen.count(v)) {
                continue;
            }
            stack.emplace_back(v, true);
            auto preds = as_vector(g.preds.at(v));
            std::sort(preds.begin(), preds.end(), [&](Id a, Id b) {
                return std::make_pair(g.rank.at(a), -a) < std::make_pair(g.rank.at(b), -b);
            });
            for (Id p : preds) {
                stack.emplace_back(p, false);
            }
        }
    }
    return result;
}
namespace {
using Work = std::map<std::string, double>;
double total_work(const Work &row) {
    double total = 0;
    for (auto [p, w] : row) {
        total += w;
    }
    return total;
}
std::map<Id, Id> pack(const GraphInfo &g, const std::vector<Ids> &parts, const Ids &stages,
                      Id cores, const Settings &settings, double coefficient, double deadline) {
    std::map<Id, Id> owner;
    for (std::size_t b = 0; b < parts.size(); ++b) {
        for (Id v : parts[b]) {
            owner[v] = Id(b);
        }
    }
    std::vector<Work> work(parts.size());
    std::vector<std::map<std::pair<Id, Id>, Id>> incoming(parts.size());
    std::vector<IdSet> external(parts.size());
    Work totals, target;
    for (std::size_t b = 0; b < parts.size(); ++b) {
        for (Id v : parts[b]) {
            auto pipe = field_string(g.ops.at(v), "pipe");
            work[b][pipe] += g.weight.at(v);
            totals[pipe] += g.weight.at(v);
            for (Id tid : g.op_inputs.at(v)) {
                const auto &producers = g.eligible_producers.at(tid);
                if (producers.empty()) {
                    external[b].insert(tid);
                } else {
                    for (Id p : producers) {
                        if (owner.at(p) != Id(b)) {
                            incoming[b][{owner.at(p), tid}] = field_id(g.tensors.at(tid), "size");
                        }
                    }
                }
            }
            if (g.direct_inputs.count(v)) {
                for (auto [index, p, size] : g.direct_inputs.at(v)) {
                    if (owner.at(p) != Id(b)) {
                        incoming[b][{owner.at(p), -v - 1}] += size;
                    }
                }
            }
        }
    }
    for (auto [pipe, w] : totals) {
        target[pipe] = std::max(1., w / cores);
    }
    std::vector<Work> loads(cores);
    std::vector<IdSet> inputs(cores);
    std::map<Id, Id> assignment;
    std::map<Id, Ids> by_stage;
    for (std::size_t b = 0; b < stages.size(); ++b) {
        by_stage[stages[b]].push_back(Id(b));
    }
    for (auto [stage, units] : by_stage) {
        auto unit_key = [&](Id b) {
            double maximum = 0;
            for (auto [pipe, w] : work[b]) {
                maximum = std::max(maximum, w / target.at(pipe));
            }
            return std::make_tuple(-maximum, -total_work(work[b]), b);
        };
        std::sort(units.begin(), units.end(),
                  [&](Id a, Id b) { return unit_key(a) < unit_key(b); });
        for (Id b : units) {
            check_deadline(deadline);
            auto cost = [&](Id c) {
                double balance = 0;
                for (auto [pipe, w] : work[b]) {
                    balance += (2 * loads[c][pipe] * w + w * w) / target.at(pipe);
                }
                Id extra = 0;
                for (Id tid : external[b]) {
                    if (!inputs[c].count(tid)) {
                        extra += field_id(g.tensors.at(tid), "size");
                    }
                }
                for (auto [source, size] : incoming[b]) {
                    if (assignment.at(source.first) != c) {
                        extra += 2 * size;
                    }
                }
                return std::make_tuple(balance + coefficient * extra / settings.bandwidth,
                                       total_work(loads[c]), c);
            };
            Id core = 0;
            for (Id c = 1; c < cores; ++c) {
                if (cost(c) < cost(core)) {
                    core = c;
                }
            }
            assignment[b] = core;
            for (auto [pipe, w] : work[b]) {
                loads[core][pipe] += w;
            }
            inputs[core].insert(external[b].begin(), external[b].end());
        }
    }
    return assignment;
}
Plan emit_plan(const GraphInfo &g, const std::vector<Ids> &parts, const Ids &stages,
               const std::map<Id, Id> &assignment, Id cores, bool merged, double deadline) {
    check_deadline(deadline);
    Plan p;
    p.ncores = cores;
    if (merged) {
        std::map<std::pair<Id, Id>, Ids> buckets;
        for (std::size_t b = 0; b < parts.size(); ++b) {
            append(buckets[{stages[b], assignment.at(Id(b))}], parts[b]);
        }
        for (auto [stage_core, nodes] : buckets) {
            Id b = Id(p.blocks.size());
            std::sort(nodes.begin(), nodes.end(),
                      [&](Id a, Id z) { return g.position.at(a) < g.position.at(z); });
            p.blocks[b] = nodes;
            p.core_of[b] = stage_core.second;
        }
    } else {
        for (std::size_t b = 0; b < parts.size(); ++b) {
            p.blocks[Id(b)] = parts[b];
        }
        p.core_of = assignment;
    }
    auto [preds, succs] = block_dependencies(g, p.blocks);
    std::map<Id, double> priority;
    for (const auto &[b, nodes] : p.blocks) {
        Id rank = 0;
        for (Id v : nodes) {
            rank = std::max(rank, g.rank.at(v));
        }
        priority[b] = -double(rank);
    }
    p.order = topological_blocks(preds, succs, priority);
    p.next_block_id = Id(p.blocks.size());
    p.validate(g);
    return p;
}
// 尝试沿图的分隔位置分配核心；不存在合适划分时返回空值。
std::optional<std::map<Id, Id>> separator_assignment(const GraphInfo &g, Id cores,
                                                     const Settings &settings, Id threshold,
                                                     double deadline) {
    IdSet separators;
    for (Id v : g.ids) {
        if (Id(g.succs.at(v).size()) >= threshold) {
            separators.insert(v);
        }
    }
    if (separators.empty()) {
        return {};
    }
    IdSet seen = separators;
    std::vector<Ids> groups;
    for (Id root : g.id_order) {
        check_deadline(deadline);
        if (seen.count(root)) {
            continue;
        }
        Ids stack = {root}, nodes;
        seen.insert(root);
        while (!stack.empty()) {
            Id v = stack.back();
            stack.pop_back();
            nodes.push_back(v);
            IdSet adjacent = g.preds.at(v);
            adjacent.insert(g.succs.at(v).begin(), g.succs.at(v).end());
            for (Id w : adjacent) {
                if (seen.insert(w).second) {
                    stack.push_back(w);
                }
            }
        }
        groups.push_back(nodes);
    }
    if (Id(groups.size()) < cores) {
        return {};
    }
    std::vector<Work> work(groups.size()), loads(cores);
    Work total, target;
    for (Id v : g.ids) {
        total[field_string(g.ops.at(v), "pipe")] += g.weight.at(v);
    }
    for (auto [pipe, w] : total) {
        target[pipe] = std::max(1., w / cores);
    }
    Ids order;
    for (std::size_t b = 0; b < groups.size(); ++b) {
        order.push_back(Id(b));
        for (Id v : groups[b]) {
            work[b][field_string(g.ops.at(v), "pipe")] += g.weight.at(v);
        }
    }
    std::sort(order.begin(), order.end(), [&](Id a, Id b) {
        return std::make_pair(-total_work(work[a]), a) < std::make_pair(-total_work(work[b]), b);
    });
    std::map<Id, Id> assignment;
    for (Id b : order) {
        auto cost = [&](Id c) {
            double value = 0;
            for (auto [pipe, w] : work[b]) {
                value += (2 * loads[c][pipe] * w + w * w) / target.at(pipe);
            }
            return std::make_pair(value, c);
        };
        Id c = 0;
        for (Id k = 1; k < cores; ++k) {
            if (cost(k) < cost(c)) {
                c = k;
            }
        }
        for (Id v : groups[b]) {
            assignment[v] = c;
        }
        for (auto [pipe, w] : work[b]) {
            loads[c][pipe] += w;
        }
    }
    for (Id v : g.id_order) {
        if (separators.count(v)) {
            check_deadline(deadline);
            std::map<Id, double> affinity;
            IdSet adjacent = g.preds.at(v);
            adjacent.insert(g.succs.at(v).begin(), g.succs.at(v).end());
            for (Id w : adjacent) {
                if (assignment.count(w)) {
                    Id bytes = 0;
                    for (auto pair : {std::make_pair(v, w), std::make_pair(w, v)}) {
                        if (g.edge_bytes.count(pair)) {
                            bytes += g.edge_bytes.at(pair);
                        }
                    }
                    affinity[assignment.at(w)] +=
                        bytes / settings.bandwidth + settings.cross_core_copy_delay;
                }
            }
            auto pipe = field_string(g.ops.at(v), "pipe");
            double weight = g.weight.at(v);
            auto cost = [&](Id c) {
                return std::make_pair((2 * loads[c][pipe] * weight + weight * weight) /
                                              target.at(pipe) -
                                          .5 * affinity[c],
                                      c);
            };
            Id c = 0;
            for (Id k = 1; k < cores; ++k) {
                if (cost(k) < cost(c)) {
                    c = k;
                }
            }
            assignment[v] = c;
            loads[c][pipe] += weight;
        }
    }
    return assignment;
}
Plan allocation_plan(const GraphInfo &g, const std::map<Id, Id> &assignment, Id cores,
                     const std::string &mode) {
    auto order = mode == "dfs" ? reverse_dfs_order(g) : g.topological_order(mode);
    Plan p;
    p.ncores = cores;
    Ids current;
    std::optional<Id> previous;
    auto flush = [&]() {
        Id b = Id(p.blocks.size());
        p.blocks[b] = current;
        p.core_of[b] = *previous;
        p.order.push_back(b);
        current.clear();
    };
    for (Id v : order) {
        Id c = assignment.at(v);
        if (!current.empty() && c != previous) {
            flush();
        }
        current.push_back(v);
        previous = c;
    }
    if (!current.empty()) {
        flush();
    }
    p.next_block_id = Id(p.blocks.size());
    p.validate(g);
    return p;
}
} // namespace
// 按原版顺序组合结构划分策略；重复方案由指纹去重。
NamedPlans structure_seed_plans(const GraphInfo &g, Id cores, const Settings &settings,
                                double deadline) {
    if (cores < 1) {
        throw std::invalid_argument("ncores must be positive");
    }
    check_deadline(deadline);
    NamedPlans result;
    std::set<std::string> seen;
    auto add = [&](std::string label, Plan p) {
        if (seen.insert(p.fingerprint()).second) {
            result.emplace_back(label, p);
        }
    };
    if (g.ids.empty()) {
        Plan p;
        p.ncores = cores;
        add("structure_empty", p);
        return result;
    }
    double total = 0, maximum = 0;
    for (const auto &part : g.atoms) {
        double sum = 0;
        for (Id v : part) {
            sum += g.weight.at(v);
        }
        total += sum;
        maximum = std::max(maximum, sum);
    }
    bool dominant = maximum > total / std::max<Id>(2, cores);
    if (Id(g.atoms.size()) >= cores && !dominant) {
        auto [parts, stages] = stage_branches(g);
        for (double coefficient : {0., .5}) {
            auto assignment = pack(g, parts, stages, cores, settings, coefficient, deadline);
            for (bool merged : {true, false}) {
                add(std::string("structure_atoms_c") + (coefficient == 0 ? "0" : "0.5") + "_" +
                        (merged ? "merged" : "branches"),
                    emit_plan(g, parts, stages, assignment, cores, merged, deadline));
            }
        }
    }
    if (dominant) {
        Id depth = 0;
        for (auto [v, d] : levels(g)) {
            depth = std::max(depth, d + 1);
        }
        for (Id threshold : {4, 8}) {
            auto assignment = separator_assignment(g, cores, settings, threshold, deadline);
            if (assignment) {
                for (const auto &mode : threshold == 4 ? std::vector<std::string>{"id", "locality"}
                                                       : std::vector<std::string>{"id"}) {
                    add("structure_separator" + key(threshold) + "_" + mode,
                        allocation_plan(g, *assignment, cores, mode));
                }
            }
        }
        for (auto [width, reverse, coefficient] : std::vector<std::tuple<Id, bool, double>>{
                 {4, false, .1}, {8, false, .5}, {16, false, .5}, {8, true, .5}}) {
            if (width >= depth && Id(g.atoms.size()) >= cores) {
                continue;
            }
            auto [parts, stages] = stage_branches(g, width, reverse, deadline);
            auto assignment = pack(g, parts, stages, cores, settings, coefficient, deadline);
            for (bool merged : (width == 4 || width == 8) && !reverse
                                   ? std::vector<bool>{true, false}
                                   : std::vector<bool>{true}) {
                add("structure_stage" + key(width) + "_" + (reverse ? "reverse" : "forward") + "_" +
                        (merged ? "merged" : "branches"),
                    emit_plan(g, parts, stages, assignment, cores, merged, deadline));
            }
        }
        if (Id(g.atoms.size()) >= cores) {
            auto [parts, stages] = stage_branches(g);
            add("structure_atoms_fallback",
                emit_plan(g, parts, stages, pack(g, parts, stages, cores, settings, 0, deadline),
                          cores, true, deadline));
        }
        check_deadline(deadline);
        auto order = reverse_dfs_order(g);
        double target = std::max(1., total / (cores * 4));
        std::vector<Ids> parts;
        Ids part;
        Id weight = 0;
        for (Id v : order) {
            if (!part.empty() && weight + g.weight.at(v) > target) {
                parts.push_back(part);
                part.clear();
                weight = 0;
            }
            part.push_back(v);
            weight += g.weight.at(v);
        }
        if (!part.empty()) {
            parts.push_back(part);
        }
        Ids stages(parts.size());
        std::iota(stages.begin(), stages.end(), 0);
        add("structure_dfs_4k",
            emit_plan(g, parts, stages, pack(g, parts, stages, cores, settings, .1, deadline),
                      cores, false, deadline));
    }
    return result;
}
} // namespace problem2
