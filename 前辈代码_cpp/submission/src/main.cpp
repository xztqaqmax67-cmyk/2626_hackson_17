// Deterministic standalone adapter for the modular C++ algorithm.
// Full evaluators, subprocess watchdogs and checkpoint inheritance remain in the research project.
#include "structure.hpp"
#include "utils.hpp"
#include "submission_cli.hpp"
using namespace problem2;

struct Candidate {
    Plan plan;
    double cost;
    Id traffic;
    std::string fingerprint;
};
// Keep all writers of a logical tensor on one core. Otherwise an incoming COPY_IN
// can become a predecessor of a local COPY_OUT assigned to an earlier subgraph.
static Plan writer_affinity(const GraphInfo &g, const Plan &source) {
    std::map<Id, Id> parent;
    for (Id op : g.ids) parent[op] = op;
    auto root = [&](Id op) {
        while (parent[op] != op) { parent[op] = parent[parent[op]]; op = parent[op]; }
        return op;
    };
    bool shared = false;
    for (const auto &[tensor, writers] : g.eligible_producers) {
        if (writers.size() < 2) continue;
        shared = true;
        for (Id writer : writers) parent[root(writer)] = root(*writers.begin());
    }
    if (!shared) return source;
    auto owner = source.owner();
    std::map<Id, Id> selected;
    for (Id op : g.id_order) {
        Id group = root(op);
        if (!selected.count(group)) selected[group] = source.core_of.at(owner.at(op));
    }
    Plan result;
    result.ncores = source.ncores;
    result.next_block_id = g.ids.empty() ? 0 : g.ids.back() + 1;
    for (Id block : source.order) for (Id op : source.blocks.at(block)) {
        result.blocks[op] = {op};
        result.core_of[op] = selected.at(root(op));
        result.order.push_back(op);
    }
    return result;
}
// Serial capacity filter, not a scoring simulator. Refuse an eviction while a
// multiwriter tensor is unfinished; future writers can otherwise block its COPY_OUT.
static bool safe_writer_lifetimes(const GraphInfo &g, const Plan &plan, const Capacity &capacity) {
    bool shared = false;
    for (const auto &[id, writers] : g.eligible_producers) shared |= writers.size() > 1;
    if (!shared) return true;
    std::vector<Ids> orders(static_cast<std::size_t>(plan.ncores));
    for (Id b : plan.order) append(orders[plan.core_of.at(b)], plan.blocks.at(b));
    for (const auto &order : orders) {
        std::map<Id, Ids> uses;
        std::map<Id, Id> last_writer;
        std::vector<IdSet> touched(order.size());
        for (std::size_t i = 0; i < order.size(); ++i) {
            Id op = order[i];
            for (Id t : g.op_inputs.at(op)) touched[i].insert(t);
            for (Id t : g.op_outputs.at(op)) { touched[i].insert(t); last_writer[t] = Id(i); }
            for (Id t : touched[i]) uses[t].push_back(Id(i));
        }
        IdSet resident;
        Capacity used{{"L1",0},{"UB",0}};
        auto position = [&](Id tid) {
            auto pos = field_string(g.tensors.at(tid), "pos");
            return pos == "DDR" ? std::string("UB") : pos;
        };
        auto next = [&](Id tid, Id at) {
            const auto &list = uses.at(tid);
            auto it = std::upper_bound(list.begin(), list.end(), at);
            return it == list.end() ? std::numeric_limits<Id>::max() : *it;
        };
        for (std::size_t i = 0; i < order.size(); ++i) {
            check_deadline();
            for (Id tid : touched[i]) {
                if (resident.count(tid)) continue;
                auto pos = position(tid);
                Id size = field_id(g.tensors.at(tid), "size");
                while (used[pos] + size > capacity.at(pos)) {
                    std::optional<Id> victim;
                    for (Id other : resident) {
                        if (touched[i].count(other) || position(other) != pos) continue;
                        if (!victim || std::make_pair(next(other,Id(i)),other) >
                                       std::make_pair(next(*victim,Id(i)),*victim)) victim = other;
                    }
                    if (!victim) return false;
                    if (g.eligible_producers.at(*victim).size() > 1 && last_writer[*victim] > Id(i)) return false;
                    used[pos] -= field_id(g.tensors.at(*victim), "size");
                    resident.erase(*victim);
                }
                resident.insert(tid);
                used[pos] += size;
            }
            for (Id tid : touched[i]) if (next(tid, Id(i)) == std::numeric_limits<Id>::max()) {
                resident.erase(tid);
                used[position(tid)] -= field_id(g.tensors.at(tid), "size");
            }
        }
    }
    return true;
}
static Plan solve(const GraphInfo &g, Id cores) {
    const Settings settings{{{"L1", 1572864}, {"UB", 131072}}, 60, 1000};
    std::vector<Candidate> pool;
    std::set<std::string> seen;
    auto add = [&](Plan plan) {
        check_deadline();
        plan = writer_affinity(g, plan);
        plan.validate(g);
        auto fingerprint = plan.fingerprint();
        if (!seen.insert(fingerprint).second) return;
        if (!safe_writer_lifetimes(g, plan, settings.capacity)) return;
        const Id traffic = route_bytes(g, plan);
        const double sketch = replay_sketch(g, plan, settings);
        const auto memory = lifetime_proxy(g, plan, settings.capacity);
        // This ranks candidates only. It does not implement the official Step 2/3 evaluator.
        const double average_excess = double(memory.first) / std::max<std::size_t>(1, plan.order.size());
        const double cost = std::max(sketch, traffic / settings.bandwidth) +
                            .15 * traffic / settings.bandwidth + 2 * average_excess / settings.bandwidth;
        pool.push_back({std::move(plan), cost, traffic, std::move(fingerprint)});
        std::sort(pool.begin(), pool.end(), [](const Candidate &a, const Candidate &b) {
            return std::tie(a.cost, a.traffic, a.fingerprint) < std::tie(b.cost, b.traffic, b.fingerprint);
        });
        if (pool.size() > 8) pool.resize(8);
    };
    work_visits = 0;
    // An initial complete plan survives exhaustion of the deterministic search allowance.
    add(single_core_plan(g, cores));
    // DFS clusters related writers and is a second conservative fallback.
    Plan dfs = single_core_plan(g, cores);
    if (!dfs.blocks.empty()) dfs.blocks[0] = reverse_dfs_order(g);
    add(std::move(dfs));
    try {
        add(operation_plan(g, cores, settings));
        for (auto &entry : structure_seed_plans(g, cores, settings)) add(std::move(entry.second));
        for (auto &entry : pipeline_seed_plans(g, cores, settings)) add(std::move(entry.second));
        for (double lambda : {0., .2, 1.}) add(construct(g, cores, lambda, settings));
        std::vector<Plan> starts;
        for (std::size_t i = 0; i < std::min<std::size_t>(2, pool.size()); ++i) starts.push_back(pool[i].plan);
        for (const auto &start : starts) {
            for (auto &entry : reorder_plans(g, start, settings, Json(), 2)) add(std::move(entry.second));
        }
        if (pool.empty()) throw submission::Error(5, "no plan passes the memory risk check");
        // Fixed neighborhood: relocate the six heaviest blocks, then split the largest block.
        const Plan base = pool.front().plan;
        std::vector<std::pair<Id, Id>> heavy;
        for (const auto &[block, nodes] : base.blocks) {
            Id work = 0;
            for (Id op : nodes) work += g.weight.at(op);
            heavy.emplace_back(-work, block);
        }
        std::sort(heavy.begin(), heavy.end());
        for (std::size_t i = 0; i < std::min<std::size_t>(6, heavy.size()); ++i) {
            Id block = heavy[i].second;
            for (Id core = 0; core < cores; ++core) {
                if (core != base.core_of.at(block)) add(migrate(base, {block}, core));
            }
        }
        if (!heavy.empty()) {
            auto split = split_block(g, base, heavy.front().second);
            if (split) {
                add(*split);
                for (Id core = 0; core < cores; ++core)
                    add(migrate(*split, {split->next_block_id - 1}, core));
            }
        }
    } catch (const TimeoutError &) {
        // Deterministic visit count, never elapsed time, selects this stopping point.
    }
    if (pool.empty()) throw submission::Error(5, "no structurally valid plan");
    pool.front().plan.validate(g);
    return pool.front().plan;
}
static int run_submission(int argc, char **argv) {
    try {
        auto opt = submission::parse(argc, argv);
        if (opt.version) { std::cout << "npu_sched modular scene-b-20261030.1\n"; return 0; }
        std::unique_ptr<GraphInfo> graph;
        try { graph = std::make_unique<GraphInfo>(read_json_strict(opt.input)); }
        catch (const submission::Error &) { throw; }
        catch (const std::exception &e) { throw submission::Error(3, e.what()); }
        const auto plan = solve(*graph, opt.cores);
        submission::write(opt.output, plan.to_dict().dump(2) + "\n");
        return 0;
    } catch (const submission::Error &e) { std::cerr << e.what() << '\n'; return e.code; }
      catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
SUBMISSION_MAIN(run_submission)
