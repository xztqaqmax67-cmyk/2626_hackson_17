#include "chain.hpp"
#include "spill_mapping.hpp"
#include "search_limits.hpp"

namespace {
void require(bool condition, const string &message) {
    if (!condition) throw runtime_error(message);
}
Graph independent_work() {
    string text = R"({"ops":[{"id":0,"op":"COMPUTE","pipe":"PIPE_V","cycles":10000},
        {"id":1,"op":"COMPUTE","pipe":"PIPE_V","cycles":10000},
        {"id":2,"op":"COMPUTE","pipe":"PIPE_V","cycles":10000},
        {"id":3,"op":"COMPUTE","pipe":"PIPE_V","cycles":10000},
        {"id":4,"op":"COMPUTE","pipe":"PIPE_V","cycles":10000},
        {"id":5,"op":"COMPUTE","pipe":"PIPE_V","cycles":10000}],"tensors":[],"edges":[]})";
    return Graph(JsonParser(text).parse());
}
Graph multiple_writers() {
    string text = R"({"ops":[{"id":0,"op":"COMPUTE","pipe":"PIPE_V","cycles":50},
        {"id":1,"op":"COMPUTE","pipe":"PIPE_V","cycles":50},
        {"id":2,"op":"COMPUTE","pipe":"PIPE_V","cycles":50},
        {"id":3,"op":"COMPUTE","pipe":"PIPE_V","cycles":50},
        {"id":4,"op":"COMPUTE","pipe":"PIPE_V","cycles":50}],
        "tensors":[{"id":100,"size":65536,"pos":"UB"},{"id":101,"size":65536,"pos":"UB"}],
        "edges":[{"source":0,"target":100},{"source":2,"target":100},{"source":100,"target":3},
                 {"source":1,"target":101},{"source":101,"target":4}]})";
    return Graph(JsonParser(text).parse());
}
void check_portfolio() {
    const auto graph = independent_work();
    for (int cores : {2, 3, 5}) {
        const auto original = dag_chain::solve(graph, cores);
        vector<Solution> seeds, repeat;
        const auto collected = dag_chain::solve(graph, cores, false, 7, &seeds);
        const auto again = dag_chain::solve(graph, cores, false, 7, &repeat);
        require(solution_json(graph, original.solution, cores) == solution_json(graph, collected.solution, cores),
                "portfolio collection changed the original search winner");
        require(original.candidates == collected.candidates && original.refined_candidates == collected.refined_candidates,
                "portfolio collection added full search evaluations");
        require(!seeds.empty() && seeds.size() <= dag_chain::search_limits::feedback_seed_limit,
                "portfolio empty or over configured limit");
        require(seeds.size() == repeat.size(), "portfolio size nondeterministic");
        set<string> outputs;
        for (size_t i = 0; i < seeds.size(); ++i) {
            validate(graph, seeds[i], cores);
            auto timing = dag_chain::refine_estimate(graph, seeds[i], cores);
            require(timing.valid && timing.cycles <= original.refined.cycles * dag_chain::search_limits::feedback_seed_cycle_ratio,
                    "portfolio includes an infeasible or non-near-best seed");
            auto json = solution_json(graph, seeds[i], cores);
            require(outputs.insert(json).second, "duplicate portfolio output");
            require(json == solution_json(graph, repeat[i], cores), "portfolio contents nondeterministic");
        }
        require(again.refined.cycles == collected.refined.cycles, "repeat changed best cost");
    }
}
void check_mapping() {
    const auto graph = multiple_writers();
    Solution seed;
    seed.core.assign(graph.size(), 0);
    for (int v = 0; v < graph.size(); ++v) seed.order.push_back(v);
    validate(graph, seed, 3);
    const auto trace = dag_chain::official_spill_trace(graph, seed, 3);
    require(trace.valid && trace.spill_bytes > 0, "mapping fixture needs actual spills");
    const auto plans = dag_chain::spill_mapping_candidates(graph, 3, seed, trace);
    const auto repeat = dag_chain::spill_mapping_candidates(graph, 3, seed, trace);
    require(!plans.empty() && plans.size() <= dag_chain::search_limits::feedback_mapping_candidates,
            "event mapping empty or over cap");
    require(plans.size() == repeat.size(), "event mapping size nondeterministic");
    bool moved_joint_writers = false;
    set<vector<int>> assignments;
    for (size_t i = 0; i < plans.size(); ++i) {
        const auto &plan = plans[i];
        validate(graph, plan, 3);
        require(plan.order == seed.order, "mapping changed original topological order");
        require(plan.core != seed.core && assignments.insert(plan.core).second, "mapping duplicate/no-op");
        require(plan.core[0] == plan.core[2], "multiwriter tensor split across cores");
        moved_joint_writers |= plan.core[0] != seed.core[0];
        require(plan.core == repeat[i].core, "mapping nondeterministic");
        require(dag_chain::official_spill_trace(graph, plan, 3).valid, "legal mapped plan cannot be replayed");
    }
    require(moved_joint_writers, "mapping never exercised a whole multiwriter move");
    auto no_spill = trace;
    no_spill.records.clear();
    require(dag_chain::spill_mapping_candidates(graph, 3, seed, no_spill).empty(), "mapping invented a hotspot");
}
} // namespace
int main() {
    try {
        check_portfolio();
        check_mapping();
        cout << "bounded refined portfolios preserve incumbent; event moves preserve writer groups and DAG order\n";
        return 0;
    } catch (const exception &error) {
        cerr << error.what() << '\n';
        return 1;
    }
}
