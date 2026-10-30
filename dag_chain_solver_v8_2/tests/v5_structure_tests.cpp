#include "chain.hpp"
#include "regions.hpp"
#include "slack.hpp"

namespace {
void require(bool condition, const string &message) {
    if (!condition) throw runtime_error(message);
}

Graph graph(const vector<int> &cycles, const vector<pair<int, int>> &edges) {
    ostringstream text;
    text << "{\"ops\":[";
    for (size_t i = 0; i < cycles.size(); ++i) {
        if (i) text << ',';
        text << "{\"id\":" << i << ",\"op\":\"COMPUTE\",\"pipe\":\"PIPE_M\",\"cycles\":"
             << cycles[i] << '}';
    }
    text << "],\"tensors\":[],\"edges\":[";
    for (size_t i = 0; i < edges.size(); ++i) {
        if (i) text << ',';
        text << "{\"source\":" << edges[i].first << ",\"target\":" << edges[i].second << '}';
    }
    text << "]}";
    const string source = text.str();
    return Graph(JsonParser(source).parse());
}

string fingerprint(const Graph &g) {
    ostringstream out;
    for (const auto &op : g.ops) {
        out << op.id << ':' << op.cycles << ':' << op.pipe << ':' << op.name << ';';
        for (const auto *values : {&op.pred, &op.succ, &op.inputs, &op.outputs}) {
            for (int v : *values) out << v << ',';
            out << '|';
        }
    }
    for (const auto &tensor : g.tensors) {
        out << tensor.id << ':' << tensor.pos << ':' << tensor.size << ':' << tensor.final_output;
        for (int v : tensor.producers) out << 'p' << v;
        for (int v : tensor.consumers) out << 'c' << v;
    }
    for (const auto &e : g.direct) out << 'e' << e.a << ':' << e.b << ':' << e.size;
    for (int v : g.writer_group) out << 'w' << v;
    for (int v : g.topo) out << 't' << v;
    return out.str();
}

Solution seed(const Graph &g) {
    Solution s;
    s.core.assign(g.size(), 0);
    s.order = g.topo;
    s.label = "test_seed";
    return s;
}

void partition_invariants(const Graph &g, int bands) {
    auto p = dag_chain::make_region_partition(g, bands);
    require(p.phase_of.size() == size_t(g.size()), "each operation needs exactly one phase");
    vector<int> occurrences(g.size()), component(g.size(), -1);
    int serial = 0;
    for (size_t phase = 0; phase < p.components.size(); ++phase) {
        for (const auto &block : p.components[phase]) {
            require(!block.empty(), "empty component is not a core-budget item");
            for (int v : block) {
                require(v >= 0 && v < g.size(), "unknown operation in phase");
                require(p.phase_of[v] == int(phase), "component/phase membership mismatch");
                ++occurrences[v];
                component[v] = serial;
            }
            ++serial;
        }
    }
    for (int v = 0; v < g.size(); ++v) {
        require(occurrences[v] == 1, "phase components must be a disjoint cover");
        for (int u : g.ops[v].pred) {
            require(p.phase_of[u] <= p.phase_of[v], "phase ordering reverses a dependency");
            if (p.phase_of[u] == p.phase_of[v])
                require(component[u] == component[v], "dependent operations called independent");
        }
        for (int u = 0; u < g.size(); ++u)
            if (g.writer_group[u] == g.writer_group[v] && p.phase_of[u] == p.phase_of[v])
                require(component[u] == component[v], "same-phase writers split into disjoint core sets");
    }
}

void candidate_invariants(const Graph &g, int k) {
    const string before = fingerprint(g);
    auto s = seed(g);
    const string initial = solution_json(g, s, k);
    dag_chain::RegionStats rs;
    dag_chain::SlackStats ss;
    auto a = dag_chain::region_candidates(g, k, s, &rs);
    auto b = dag_chain::slack_candidates(g, k, s, &ss);
    auto again_a = dag_chain::region_candidates(g, k, s);
    auto again_b = dag_chain::slack_candidates(g, k, s);
    require(a.size() == again_a.size() && b.size() == again_b.size(), "candidate count nondeterminism");
    require(a.size() <= 8 && b.size() <= 4, "candidate generation is not bounded");
    for (size_t i = 0; i < a.size(); ++i) {
        validate(g, a[i], k);
        require(solution_json(g, a[i], k) == solution_json(g, again_a[i], k), "region nondeterminism");
    }
    for (size_t i = 0; i < b.size(); ++i) {
        validate(g, b[i], k);
        require(solution_json(g, b[i], k) == solution_json(g, again_b[i], k), "slack nondeterminism");
    }
    require(before == fingerprint(g), "candidate construction modified original dependencies or tensors");
    require(initial == solution_json(g, s, k), "candidate construction modified incumbent solution");
}
} // namespace

int main() {
    try {
        auto budgets = dag_chain::enumerate_core_budgets(2, 5);
        require(budgets.size() == 10, "two components with <=5 cores have ten positive budgets");
        require(dag_chain::enumerate_core_budgets(3, 5).size() == 10,
                "three components with <=5 cores have ten positive budgets");
        require(set<vector<int>>(budgets.begin(), budgets.end()).size() == budgets.size(), "duplicate core budget");
        require(find(budgets.begin(), budgets.end(), vector<int>{1, 4}) != budgets.end() &&
                    find(budgets.begin(), budgets.end(), vector<int>{4, 1}) != budgets.end(),
                "three-core preference must not forbid unbalanced components");
        for (const auto &budget : budgets) {
            require(all_of(budget.begin(), budget.end(), [](int x) { return x >= 1; }), "empty core allocation");
            require(accumulate(budget.begin(), budget.end(), 0) <= 5, "core budget exceeded");
        }

        auto independent = graph({4000, 1000, 4000, 1000, 4000, 1000}, {{0, 2}, {2, 4}, {1, 3}, {3, 5}});
        auto whole = dag_chain::make_region_partition(independent, 1);
        require(whole.components.size() == 1 && whole.components[0].size() == 2,
                "two disconnected chains should remain two natural regions");
        dag_chain::RegionStats region_stats;
        auto regional = dag_chain::region_candidates(independent, 5, seed(independent), &region_stats);
        require(!regional.empty() && region_stats.budget_vectors > 0,
                "independent chain fixture must exercise regional budget candidates");
        auto crossing = graph({0, 1, 2000, 0, 400, 20, 0},
                              {{0, 1}, {0, 2}, {1, 3}, {2, 4}, {3, 5}, {4, 5}, {0, 5}, {5, 6}});
        const string writer_json = R"({"ops":[
            {"id":0,"op":"A","pipe":"PIPE_M","cycles":10},
            {"id":1,"op":"B","pipe":"PIPE_V","cycles":6000},
            {"id":2,"op":"C","pipe":"PIPE_FIX","cycles":10},
            {"id":3,"op":"D","pipe":"PIPE_M","cycles":10}],
            "tensors":[{"id":10,"pos":"UB","size":60000}],
            "edges":[{"source":0,"target":10},{"source":2,"target":10},
                     {"source":10,"target":3},{"source":0,"target":1},{"source":1,"target":2}]})";
        Graph writers(JsonParser(writer_json).parse());
        const string separate_writers_json = R"({"ops":[
            {"id":0,"op":"W0","pipe":"PIPE_M","cycles":100},
            {"id":1,"op":"W1","pipe":"PIPE_V","cycles":100},
            {"id":2,"op":"JOIN","pipe":"PIPE_M","cycles":100},
            {"id":3,"op":"SLOW","pipe":"PIPE_FIX","cycles":10000}],
            "tensors":[{"id":10,"pos":"UB","size":4096}],
            "edges":[{"source":0,"target":10},{"source":1,"target":10},
                     {"source":10,"target":2},{"source":3,"target":2}]})";
        Graph separate_writers(JsonParser(separate_writers_json).parse());
        for (int bands : {1, 2, 3, 5}) {
            partition_invariants(independent, bands);
            partition_invariants(crossing, bands);
            partition_invariants(writers, bands);
            partition_invariants(separate_writers, bands);
        }
        for (int k = 2; k <= 5; ++k) {
            candidate_invariants(independent, k);
            candidate_invariants(crossing, k);
            candidate_invariants(writers, k);
            candidate_invariants(separate_writers, k);
        }
        auto too_many_blocks = graph({10, 20, 30, 40, 50, 60, 70}, {});
        candidate_invariants(too_many_blocks, 2);

        Config cfg;
        require(dag_chain::slack_uncontended_arrival(100, 600, cfg) == 1120,
                "consumer availability includes COPY_OUT, delay, then COPY_IN");
        require(dag_chain::slack_exposed_delay(2300, 3000) == 0,
                "a consumer already waiting for necessary input can hide an earlier arrival");
        require(dag_chain::slack_exposed_delay(2300, 1000) == 1300,
                "an unrelated later graph finish cannot provide consumer slack");
        auto unrelated = graph({100, 100, 10000}, {{0, 1}});
        auto us = seed(unrelated);
        us.core = {0, 0, 1};
        auto proposals = dag_chain::slack_candidates(unrelated, 3, us);
        for (const auto &proposal : proposals)
            require(proposal.core[0] == proposal.core[1],
                    "a slow unrelated core was incorrectly used to hide a critical consumer transfer");

        const string hidden_json = R"({"ops":[
            {"id":0,"op":"P","pipe":"PIPE_M","cycles":100},
            {"id":1,"op":"X","pipe":"PIPE_V","cycles":5000},
            {"id":2,"op":"JOIN","pipe":"PIPE_M","cycles":100}],"tensors":[],
            "edges":[{"source":0,"target":2},{"source":1,"target":2}]})";
        Graph hidden(JsonParser(hidden_json).parse());
        auto hs = seed(hidden);
        hs.core = {0, 1, 0};
        hs.order = {0, 1, 2};
        auto hidden_proposals = dag_chain::slack_candidates(hidden, 3, hs);
        require(!hidden_proposals.empty(), "necessary slow join input should expose a real slack candidate");
        for (const auto &proposal : hidden_proposals) validate(hidden, proposal, 3);
        require(hidden.ops[2].pred.size() == 2, "slack release removed a join dependency");

        // Integration test compares internal estimates only, not official cycles.
        // It prevents new heuristics from removing the retained v4 incumbent.
        auto base = dag_chain::solve(crossing, 3, false, 0);
        auto full = dag_chain::solve(crossing, 3, false, 3);
        validate(crossing, full.solution, 3);
        require(base.refined.valid && full.refined.valid && full.refined.cycles <= base.refined.cycles,
                "v5 must preserve the v4 incumbent under the same internal estimator");
        cout << "v5 phase/core-budget/dependency/writer/slack/determinism invariants passed\n";
        return 0;
    } catch (const exception &error) {
        cerr << error.what() << '\n';
        return 1;
    }
}

