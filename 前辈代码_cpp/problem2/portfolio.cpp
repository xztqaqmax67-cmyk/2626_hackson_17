// 对应 portfolio.py。
// 管理多条搜索路线的精英方案，按预算和改进情况公平分配探索机会。
#include "portfolio.hpp"
namespace problem2 {
Objective objective(const Json &result) {
    return {result.at("makespan").get<double>(), field_id(result, "added_copy_bytes")};
}
Ids assignment_key(const GraphInfo &g, const Plan &p) {
    auto owner = p.owner();
    Ids result;
    for (Id v : g.ids) {
        result.push_back(p.core_of.at(owner.at(v)));
    }
    return result;
}
std::string seed_family(const std::string &label) {
    for (std::string prefix :
         {"structure_separator", "structure_stage", "structure_atoms", "structure_dfs", "pipe_",
          "native_", "chain_", "activate_", "inherited_", "resumed_"}) {
        if (label.rfind(prefix, 0) == 0) {
            if (prefix == "inherited_" || prefix == "resumed_") {
                return "inherited";
            }
            while (!prefix.empty() && prefix.back() == '_') {
                prefix.pop_back();
            }
            return prefix;
        }
    }
    return label;
}
std::vector<SearchRow> diverse_rows(const GraphInfo &g, std::vector<SearchRow> rows, Id limit) {
    std::stable_sort(rows.begin(), rows.end(),
                     [](const SearchRow &a, const SearchRow &b) { return a.score < b.score; });
    std::vector<SearchRow> chosen;
    std::set<Ids> maps;
    std::set<std::string> used;
    for (bool distinct : {true, false}) {
        for (const auto &row : rows) {
            auto key = row.plan.fingerprint();
            if (used.count(key)) {
                continue;
            }
            auto mapping = assignment_key(g, row.plan);
            if (distinct && maps.count(mapping)) {
                continue;
            }
            chosen.push_back(row);
            maps.insert(mapping);
            used.insert(key);
            if (Id(chosen.size()) == limit) {
                return chosen;
            }
        }
    }
    return chosen;
}
std::vector<SearchRow> polishing_seeds(const GraphInfo &g, const std::vector<SearchRow> &rows,
                                       Id limit) {
    auto ordered = rows;
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const SearchRow &a, const SearchRow &b) { return a.score < b.score; });
    std::vector<SearchRow> chosen;
    std::set<std::string> used, families;
    std::set<Ids> maps;
    auto add = [&](const SearchRow &row) {
        auto key = row.plan.fingerprint();
        if (used.count(key) || Id(chosen.size()) >= limit) {
            return;
        }
        chosen.push_back(row);
        used.insert(key);
        families.insert(seed_family(row.label));
        maps.insert(assignment_key(g, row.plan));
    };
    for (const auto &row : ordered) {
        if (row.label.rfind("structure_", 0) == 0 || row.label.rfind("pipe_", 0) == 0 ||
            row.label.rfind("native_", 0) == 0) {
            add(row);
            break;
        }
    }
    for (const auto &row : ordered) {
        if (!families.count(seed_family(row.label))) {
            add(row);
        }
    }
    for (const auto &row : ordered) {
        if (!maps.count(assignment_key(g, row.plan))) {
            add(row);
        }
    }
    return chosen;
}
Route::Route(const GraphInfo &g, std::string route_name, const std::vector<SearchRow> &rows,
             bool ref, bool small)
    : graph(&g), name(std::move(route_name)), reference(ref), compact(small) {
    if (rows.empty()) {
        throw std::invalid_argument("route requires a seed");
    }
    best = *std::min_element(rows.begin(), rows.end(), [](const SearchRow &a, const SearchRow &b) {
        return a.score < b.score;
    });
    initial = best.score.first;
    stats = {{"name", name},
             {"reference", reference},
             {"initial_makespan", initial},
             {"best_makespan", initial},
             {"local_improvements", 0},
             {"steps", 0},
             {"seconds", 0.},
             {"secondary_centers", 0},
             {"fifo_calls", 0},
             {"activation_calls", 0},
             {"repair_calls", 0},
             {"repair_skipped", 0},
             {"repair_seconds", 0.},
             {"status", "pending"}};
    for (const auto &row : rows) {
        offer(row);
    }
}
int Route::offer(const SearchRow &row) {
    auto key = row.plan.fingerprint();
    evaluated.insert(key);
    int improvement = 0;
    if (row.score < best.score) {
        improvement = row.score.first < best.score.first ? 2 : 1;
        best = row;
        stats["local_improvements"] = field_id(stats, "local_improvements") + 1;
        stats["best_makespan"] = row.score.first;
    }
    bool replaced = false;
    for (auto &existing : pool) {
        if (existing.plan.fingerprint() == key) {
            existing = row;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        pool.push_back(row);
    }
    if (reference) {
        std::stable_sort(pool.begin(), pool.end(),
                         [](const SearchRow &a, const SearchRow &b) { return a.score < b.score; });
        if (pool.size() > std::size_t(compact ? 2 : 4)) {
            pool.resize(compact ? 2 : 4);
        }
    } else {
        pool = diverse_rows(*graph, pool, compact ? 3 : 5);
    }
    return improvement;
}
std::optional<SearchRow> Route::next_center() {
    auto ordered = pool;
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const SearchRow &a, const SearchRow &b) { return a.score < b.score; });
    for (const auto &row : ordered) {
        if (!visited.insert(row.plan.fingerprint()).second) {
            continue;
        }
        if (row.score > best.score) {
            if (field_id(stats, "secondary_centers") >= (compact ? 3 : 16)) {
                continue;
            }
            stats["secondary_centers"] = field_id(stats, "secondary_centers") + 1;
        }
        return row;
    }
    return {};
}
bool Route::repair_allowed(const std::string &key) {
    ++repair_opportunities;
    bool allowed = !repaired.count(key) && repair_cost <= std::max(1., .18 * seconds) &&
                   (repair_failures < 4 || repair_opportunities % 12 == 0);
    if (!allowed) {
        stats["repair_skipped"] = field_id(stats, "repair_skipped") + 1;
    }
    return allowed;
}
double Route::weight() const {
    double gain = std::max(0., (initial - best.score.first) / std::max(1., initial));
    return (reference ? 2. : 1.) * (1. + std::min(1., 5. * gain));
}
Route *select_route(const std::vector<Route *> &routes) {
    return *std::min_element(routes.begin(), routes.end(), [](const Route *a, const Route *b) {
        return std::make_pair(a->seconds / a->weight(), a->name) <
               std::make_pair(b->seconds / b->weight(), b->name);
    });
}
} // namespace problem2
