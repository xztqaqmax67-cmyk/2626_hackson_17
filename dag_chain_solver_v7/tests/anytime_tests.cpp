#include "anytime.hpp"

namespace {
void require(bool condition, const string &message) {
    if (!condition) throw runtime_error(message);
}

// This graph is independent of the public benchmark. Its parallel branches,
// skip edge and multi-writer tensor exercise whole-group moves and ordering.
Graph fixture() {
    const vector<int> cycles{170, 2700, 90, 1200, 400, 60, 800, 200, 50, 300, 100, 700};
    const vector<pair<int, int>> edges{
        {0, 3}, {1, 4}, {2, 5}, {3, 6}, {4, 7}, {5, 8},
        {6, 9}, {7, 9}, {8, 10}, {9, 11}, {10, 11}, {0, 10}};
    ostringstream text;
    text << "{\"ops\":[";
    for (size_t i = 0; i < cycles.size(); ++i) {
        if (i) text << ',';
        text << "{\"id\":" << i << ",\"op\":\"WORK\",\"pipe\":\""
             << (i % 3 == 0 ? "PIPE_V" : "PIPE_M") << "\",\"cycles\":" << cycles[i] << '}';
    }
    text << "],\"tensors\":[{\"id\":100,\"pos\":\"UB\",\"size\":256}],\"edges\":[";
    bool first = true;
    auto add = [&](int a, int b) {
        if (!first) text << ',';
        first = false;
        text << "{\"source\":" << a << ",\"target\":" << b << '}';
    };
    for (auto [a, b] : edges) add(a, b);
    add(0, 100);
    add(2, 100);
    add(100, 9);
    text << "]}";
    return Graph(JsonParser(text.str()).parse());
}

string fingerprint(const Graph &g) {
    ostringstream text;
    for (const auto &op : g.ops) {
        text << op.id << ':' << op.cycles << ':' << op.pipe << ':';
        for (const auto *row : {&op.pred, &op.succ, &op.inputs, &op.outputs}) {
            for (int v : *row) text << v << ',';
            text << '|';
        }
    }
    for (const auto &t : g.tensors) {
        text << t.id << ':' << t.pos << ':' << t.size << ':' << t.final_output;
        for (int v : t.producers) text << 'p' << v;
        for (int v : t.consumers) text << 'c' << v;
    }
    for (const auto &e : g.direct) text << 'd' << e.a << ':' << e.b << ':' << e.size;
    for (int v : g.writer_group) text << 'w' << v;
    return text.str();
}

template<class Work>
void expires(Work work, const string &message) {
    bool caught = false;
    try {
        work();
    } catch (const SearchExpired &) {
        caught = true;
    }
    require(caught, message);
}

auto merit(const dag_chain::Result &result) {
    return make_pair(result.refined.cycles, result.refined.route + result.refined.spill);
}
} // namespace

int main() {
    try {
        Deadline deadline;
        deadline.start = Clock::now() - chrono::seconds(1);
        deadline.limit = 0.001;
        expires([&] { deadline.check(); }, "expired deadline must throw the typed cancellation");
        const vector<dag_chain::TimingEvent> empty;
        expires([&] { dag_chain::simulate_events(empty, &deadline); },
                "event simulation must observe cancellation even for an empty input");

        const auto g = fixture();
        const auto before = fingerprint(g);
        const int cores = 5;
        const auto base = dag_chain::solve(g, cores);
        require(base.refined.valid, "the deterministic seed must be a completed feasible estimate");
        validate(g, base.solution, cores);
        expires([&] { dag_chain::refine_estimate(g, base.solution, cores, &deadline); },
                "refinement must not return a partial result after cancellation");
        const auto normal = dag_chain::refine_estimate(g, base.solution, cores, nullptr);
        require(normal.valid && normal.cycles == base.refined.cycles,
                "null deadline must preserve the original refinement behavior");
        deadline.enabled = false;
        require(dag_chain::refine_estimate(g, base.solution, cores, &deadline).cycles == normal.cycles,
                "a disabled deadline must not cancel valid refinement");

        dag_chain::TimedSearchStats zero_stats;
        const auto zero = dag_chain::solve_for_budget(g, cores, Clock::now(), 0, &zero_stats);
        require(solution_json(g, zero.solution, cores) == solution_json(g, base.solution, cores),
                "zero budget must retain the complete v5 seed, not an empty or partial plan");
        require(zero_stats.attempted == 0 && zero_stats.refined == 0,
                "zero budget must not start the expanded search");

        dag_chain::TimedSearchStats expired_stats;
        const auto expired = dag_chain::solve_for_budget(
            g, cores, Clock::now() - chrono::seconds(1), 0.1, &expired_stats);
        require(solution_json(g, expired.solution, cores) == solution_json(g, base.solution, cores),
                "expired budget must preserve the complete seed");
        require(expired_stats.attempted == 0, "expired budget must not launch new candidates");

        // Timing changes how many candidates finish. Do not assert byte equality
        // across repeated wall-clock runs or an exact elapsed number of seconds.
        dag_chain::TimedSearchStats stats;
        const auto started = Clock::now();
        const auto searched = dag_chain::solve_for_budget(g, cores, started, 0.35, &stats);
        validate(g, searched.solution, cores);
        require(stats.attempted > 0 && stats.unique_candidates > 0 && stats.refined > 0,
                "short budget must actually explore and refine new complete candidates");
        require(stats.workers >= 1 && stats.workers <= 4, "algorithm worker cap exceeded");
        require(searched.refined.valid && merit(searched) <= merit(base),
                "timed search must retain the incumbent under its internal selection criterion");
        const auto repeated = dag_chain::refine_estimate(g, searched.solution, cores);
        require(repeated.valid && repeated.cycles == searched.refined.cycles &&
                    repeated.route == searched.refined.route && repeated.spill == searched.refined.spill,
                "published incumbent must contain a completed refinement, not partial scratch state");
        require(fingerprint(g) == before, "timed search mutated original graph or writer constraints");
        for (int v = 0; v < g.size(); ++v)
            require(searched.solution.core[v] == searched.solution.core[g.writer_group[v]],
                    "a timed move split an atomic writer group");
        require(chrono::duration<double>(Clock::now() - started).count() < 10,
                "tiny timed search did not stop within a generous cancellation allowance");
        cout << "anytime deadline, completed incumbent, useful search, writer/dependency preservation passed\n";
        return 0;
    } catch (const exception &error) {
        cerr << error.what() << '\n';
        return 1;
    }
}
