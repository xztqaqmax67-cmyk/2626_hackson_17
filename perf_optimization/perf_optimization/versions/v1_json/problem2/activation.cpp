// 对应 activation.py。
// 从繁忙核心迁出一组有关联的算子，尝试启用空闲核心。
#include "activation.hpp"
namespace problem2 {
namespace {
using Work = std::map<std::string, double>;
double sum(const Work &row) {
    double value = 0;
    for (auto [pipe, w] : row) {
        value += w;
    }
    return value;
}
double maximum(const Work &row) {
    double value = 0;
    for (auto [pipe, w] : row) {
        value = std::max(value, w);
    }
    return value;
}
std::pair<std::vector<Ids>, std::map<Id, Id>> parts(const GraphInfo &g,
                                                    const std::map<Id, Id> &assignment, Id source,
                                                    std::optional<Id> width, bool separators,
                                                    double deadline) {
    IdSet allowed;
    std::map<Id, Id> levels, bands, owner;
    std::vector<Ids> result;
    for (Id v : g.ids) {
        if (assignment.at(v) == source) {
            Id ps = 0, ss = 0;
            for (Id w : g.preds.at(v)) {
                ps += assignment.at(w) == source;
            }
            for (Id w : g.succs.at(v)) {
                ss += assignment.at(w) == source;
            }
            if (!separators || (ps < 3 && ss < 3)) {
                allowed.insert(v);
            }
        }
    }
    if (width) {
        for (Id v : g.id_order) {
            check_deadline(deadline);
            Id depth = -1;
            for (Id p : g.preds.at(v)) {
                depth = std::max(depth, levels.at(p));
            }
            levels[v] = depth + 1;
        }
        for (Id v : allowed) {
            bands[v] = levels.at(v) / *width;
        }
    }
    for (Id root : g.id_order) {
        check_deadline(deadline);
        if (!allowed.count(root) || owner.count(root)) {
            continue;
        }
        owner[root] = Id(result.size());
        result.push_back({});
        Ids stack = {root};
        while (!stack.empty()) {
            Id v = stack.back();
            stack.pop_back();
            IdSet adjacent = g.preds.at(v);
            adjacent.insert(g.succs.at(v).begin(), g.succs.at(v).end());
            for (Id w : adjacent) {
                if (allowed.count(w) && !owner.count(w) &&
                    (!width || bands.at(w) == bands.at(root))) {
                    owner[w] = owner.at(root);
                    stack.push_back(w);
                }
            }
        }
    }
    for (Id v : g.id_order) {
        if (owner.count(v)) {
            result[owner.at(v)].push_back(v);
        }
    }
    return {result, owner};
}
// 迁移指定算子集合，重建受影响的块；保持其余算子的相对顺序。
Plan relocate(const Plan &p, const IdSet &moved, Id target) {
    Plan result;
    result.ncores = p.ncores;
    result.next_block_id = p.next_block_id;
    for (Id b : p.order) {
        const auto &nodes = p.blocks.at(b);
        std::size_t start = 0;
        bool first = true;
        for (std::size_t end = 1; end <= nodes.size(); ++end) {
            if (end < nodes.size() && moved.count(nodes[end]) == moved.count(nodes[start])) {
                continue;
            }
            Id id = first ? b : result.next_block_id++;
            result.blocks[id] = Ids(nodes.begin() + start, nodes.begin() + end);
            result.core_of[id] = moved.count(nodes[start]) ? target : p.core_of.at(b);
            result.order.push_back(id);
            start = end;
            first = false;
        }
    }
    return result;
}
} // namespace
// 从繁忙核心挑选迁移单元，兼顾负载与新增搬运量，并限制候选数量。
NamedPlans activation_plans(const GraphInfo &g, const Plan &p, const Settings &settings,
                            double deadline, Id limit) {
    check_deadline(deadline);
    if (limit <= 0) {
        return {};
    }
    p.validate(g);
    IdSet occupied;
    for (auto [b, c] : p.core_of) {
        occupied.insert(c);
    }
    std::optional<Id> target;
    for (Id c = 0; c < p.ncores; ++c) {
        if (!occupied.count(c)) {
            target = c;
            break;
        }
    }
    if (!target || g.ids.empty()) {
        return {};
    }
    double bandwidth = std::max(1., settings.bandwidth);
    auto owners = p.owner();
    std::map<Id, Id> assignment;
    for (Id v : g.ids) {
        assignment[v] = p.core_of.at(owners.at(v));
    }
    auto loads = pipe_loads(g, p);
    Ids sources = as_vector(occupied);
    std::sort(sources.begin(), sources.end(), [&](Id a, Id b) {
        return std::make_tuple(-maximum(loads[a]), -sum(loads[a]), a) <
               std::make_tuple(-maximum(loads[b]), -sum(loads[b]), b);
    });
    if (sources.size() > 2) {
        sources.resize(2);
    }
    Id base = route_bytes(g, p);
    Work total;
    for (auto row : loads) {
        for (auto [pipe, w] : row) {
            total[pipe] += w;
        }
    }
    std::set<IdSet> unique;
    using Row = std::tuple<std::tuple<double, double, Id>, std::string, Plan>;
    std::vector<Row> shortlist;
    for (Id source : sources) {
        Id count = 0;
        for (auto [v, c] : assignment) {
            count += c == source;
        }
        if (count < 2) {
            continue;
        }
        for (auto [label, width, separators] :
             std::vector<std::tuple<std::string, std::optional<Id>, bool>>{
                 {"components", {}, false},
                 {"branches", {}, true},
                 {"stage4", 4, false},
                 {"stage8", 8, false}}) {
            auto [units, owner] = parts(g, assignment, source, width, separators, deadline);
            if (units.empty()) {
                continue;
            }
            std::vector<Work> work(units.size());
            std::vector<Id> boundary(units.size());
            for (std::size_t b = 0; b < units.size(); ++b) {
                check_deadline(deadline);
                for (Id v : units[b]) {
                    work[b][field_string(g.ops.at(v), "pipe")] += g.weight.at(v);
                }
            }
            for (const auto &[tid, tensor] : g.tensors) {
                check_deadline(deadline);
                IdSet related = g.eligible_producers.at(tid);
                related.insert(g.eligible_consumers.at(tid).begin(),
                               g.eligible_consumers.at(tid).end());
                IdSet indices;
                bool outside = false;
                for (Id v : related) {
                    if (owner.count(v)) {
                        indices.insert(owner.at(v));
                    } else {
                        outside = true;
                    }
                }
                if (indices.size() > 1 || outside) {
                    for (Id b : indices) {
                        boundary[b] += field_id(tensor, "size") *
                                       (g.eligible_producers.at(tid).empty() ? 1 : 2);
                    }
                }
            }
            for (const auto &e : g.direct_edges) {
                Id a = field_id(e, "source"), b = field_id(e, "target");
                if ((owner.count(a) ? owner.at(a) : -1) != (owner.count(b) ? owner.at(b) : -1)) {
                    for (Id v : {a, b}) {
                        if (owner.count(v)) {
                            boundary[owner.at(v)] +=
                                2 * std::max<Id>(0, e.value("data_size", Id(0)));
                        }
                    }
                }
            }
            for (double fraction : {.5, .35}) {
                Work desired;
                for (auto [pipe, w] : loads[source]) {
                    if (w > 0) {
                        desired[pipe] = std::max(1., w * fraction);
                    }
                }
                if (desired.empty()) {
                    continue;
                }
                Ids order;
                for (std::size_t b = 0; b < units.size(); ++b) {
                    order.push_back(Id(b));
                }
                auto order_key = [&](Id b) {
                    double normalized = 0;
                    for (auto [pipe, w] : desired) {
                        normalized = std::max(normalized, work[b][pipe] / w);
                    }
                    return std::make_tuple(-sum(work[b]) / (1. + .1 * boundary[b] / bandwidth),
                                           -normalized, b);
                };
                std::sort(order.begin(), order.end(),
                          [&](Id a, Id b) { return order_key(a) < order_key(b); });
                Work moved_work;
                IdSet moved;
                auto loss = [&](Work row) {
                    double value = 0;
                    for (auto [pipe, w] : desired) {
                        value += (row[pipe] - w) * (row[pipe] - w) / w;
                    }
                    return value;
                };
                double current = loss(moved_work);
                for (Id b : order) {
                    check_deadline(deadline);
                    auto trial = moved_work;
                    for (auto [pipe, w] : work[b]) {
                        trial[pipe] += w;
                    }
                    double value = loss(trial);
                    if (value < current) {
                        moved.insert(units[b].begin(), units[b].end());
                        moved_work = trial;
                        current = value;
                    }
                }
                if (moved.empty() || Id(moved.size()) >= count || !unique.insert(moved).second) {
                    continue;
                }
                auto candidate = relocate(p, moved, *target);
                Id delta = route_bytes(g, candidate) - base;
                auto new_load = loads;
                for (Id v : moved) {
                    auto pipe = field_string(g.ops.at(v), "pipe");
                    Id amount = field_id(g.ops.at(v), "cycles");
                    new_load[source][pipe] -= amount;
                    new_load[*target][pipe] += amount;
                }
                double bound = 0, energy = 0;
                for (auto row : new_load) {
                    bound = std::max(bound, maximum(row));
                    for (auto [pipe, w] : row) {
                        energy += w * w / std::max(1., total[pipe]);
                    }
                }
                auto rank =
                    std::make_tuple(bound + .1 * std::max<Id>(0, delta) / bandwidth,
                                    energy + .05 * std::max<Id>(0, delta) / bandwidth, delta);
                shortlist.emplace_back(rank,
                                       "activate_" + key(source) + "_to_" + key(*target) + "_" +
                                           label + "_" + key(Id(100 * fraction)),
                                       candidate);
            }
        }
    }
    std::stable_sort(shortlist.begin(), shortlist.end(), [](const Row &a, const Row &b) {
        return std::make_pair(std::get<0>(a), std::get<1>(a)) <
               std::make_pair(std::get<0>(b), std::get<1>(b));
    });
    NamedPlans output;
    for (std::size_t i = 0; i < shortlist.size() && Id(i) < limit; ++i) {
        check_deadline(deadline);
        output.emplace_back(std::get<1>(shortlist[i]), std::get<2>(shortlist[i]));
    }
    return output;
}
} // namespace problem2
