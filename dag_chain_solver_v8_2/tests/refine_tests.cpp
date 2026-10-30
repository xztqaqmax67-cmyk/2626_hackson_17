#include "chain.hpp"

void require(bool condition, const string &message) {
    if (!condition) throw runtime_error(message);
}
void close(double value, double expected, const string &message) {
    require(abs(value - expected) < 1e-6, message + ": " + to_string(value));
}
int main() {
    try {
        using dag_chain::TimingEvent;
        // Two 600-byte transfers compete, rather than each finishing at t=10.
        vector<TimingEvent> events{{0, 0, 10, true, {}}, {1, 0, 10, true, {}}};
        close(dag_chain::simulate_events(events), 20, "shared DDR");
        // A transfer begins at t=5: the first finishes at 15, the second at 20.
        events = {{0, 0, 10, true, {}}, {1, 2, 5, false, {}},
                  {1, 0, 10, true, {{1, 0}}}, {0, 2, 4, false, {{0, 0}}}};
        close(dag_chain::simulate_events(events), 20, "DDR arrival recomputes rates");
        events = {{0, 1, 10, true, {}}, {1, 0, 10, true, {{0, 1000}}},
                  {1, 2, 7, false, {{1, 0}}}};
        close(dag_chain::simulate_events(events), 1027, "cross-core COPY delay");
        events[0].predecessors = {{2, 0}};
        require(dag_chain::simulate_events(events) >= INF / 2, "reject dependency cycle");
        close(dag_chain::simulate_events({}), 0, "empty event stream");

        string text = R"({"tensors":[{"id":10,"pos":"UB","size":600}],"ops":[{"id":0,"op":"M","pipe":"PIPE_M","cycles":10},{"id":1,"op":"V","pipe":"PIPE_V","cycles":10}],"edges":[{"source":0,"target":10},{"source":10,"target":1}]})";
        Graph g(JsonParser(text).parse());
        Solution s{{0, 1}, {0, 1}, "test"};
        auto r = dag_chain::refine_estimate(g, s, 2);
        require(r.valid, "valid remote tensor");
        close(r.cycles, 1040, "producer/COPY_OUT/delay/COPY_IN/consumer");
        close(r.route, 1200, "remote tensor byte accounting");
        s.core = {0, 0};
        r = dag_chain::refine_estimate(g, s, 2);
        close(r.cycles, 20, "local dependent computation");
        close(r.route, 0, "local intermediate remains on chip");

        string partial = R"({"tensors":[{"id":10,"pos":"UB","size":80000},{"id":11,"pos":"UB","size":80000}],"ops":[{"id":0,"op":"M","pipe":"PIPE_M","cycles":10},{"id":1,"op":"M","pipe":"PIPE_M","cycles":10},{"id":2,"op":"M","pipe":"PIPE_M","cycles":10},{"id":3,"op":"M","pipe":"PIPE_M","cycles":10},{"id":4,"op":"M","pipe":"PIPE_M","cycles":10}],"edges":[{"source":0,"target":10},{"source":3,"target":10},{"source":10,"target":4},{"source":1,"target":11},{"source":11,"target":2}]})";
        Graph pg(JsonParser(partial).parse());
        Solution ps{{0, 0, 0, 0, 0}, {0, 1, 2, 3, 4}, "partial"};
        auto pr = dag_chain::refine_estimate(pg, ps, 2);
        require(pr.valid, "partial-write eviction is supported");
        close(pr.spill, 160000, "dirty spill out plus restore");
        require(pr.peak[1] <= 131072, "hard capacity remains valid");
        require(pr.cycles >= 160000.0 / 60, "spill consumes shared bandwidth");
        ps.order = {1, 2, 0, 3, 4};
        auto clustered = dag_chain::refine_estimate(pg, ps, 2);
        require(clustered.valid && clustered.cycles < pr.cycles,
                "lifetime clustering eliminates the eviction");
        close(clustered.spill, 0, "clustered spill bytes");

        // Communication chooses a shorter parent, but finish must still wait
        // for the longer OTHER predecessor before propagating downstream.
        string joins = R"({"tensors":[{"id":10,"pos":"L1","size":60000}],"ops":[{"id":0,"op":"M","pipe":"PIPE_M","cycles":100},{"id":1,"op":"M","pipe":"PIPE_M","cycles":1},{"id":2,"op":"M","pipe":"PIPE_M","cycles":3},{"id":3,"op":"M","pipe":"PIPE_M","cycles":2}],"edges":[{"source":0,"target":2},{"source":1,"target":10},{"source":10,"target":2},{"source":2,"target":3}]})";
        Graph jg(JsonParser(joins).parse());
        auto d = dag_chain::decompose(jg, 2, 1, 0.25);
        require(d.parent[2] == 1, "communication-biased preferred parent");
        close(d.earliest_finish[2], 103, "all-predecessor join finish");
        close(d.earliest_finish[3], 105, "propagate corrected finish");
        cout << "shared DDR, sync, cycles, partial writes, memory reuse and join propagation: passed\n";
    } catch (const exception &e) {
        cerr << e.what() << '\n';
        return 1;
    }
    return 0;
}

