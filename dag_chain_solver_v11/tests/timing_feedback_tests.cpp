#include "timing_feedback.hpp"

using namespace dag_chain;
namespace {
void require(bool ok, const string &message) { if (!ok) throw runtime_error(message); }
Graph graph(const vector<pair<int, i64>> &ops, const vector<pair<int, int>> &edges,
            const vector<pair<i64, i64>> &tensors = {}) {
    const char *pipes[] = {"PIPE_MTE2", "PIPE_MTE3", "PIPE_M", "PIPE_V", "PIPE_FIX"};
    ostringstream out; out << "{\"ops\":[";
    for (size_t i = 0; i < ops.size(); ++i) {
        if (i) out << ',';
        out << "{\"id\":" << i << ",\"op\":\"COMPUTE\",\"pipe\":\"" << pipes[ops[i].first]
            << "\",\"cycles\":" << ops[i].second << '}';
    }
    out << "],\"tensors\":[";
    for (size_t i = 0; i < tensors.size(); ++i) {
        if (i) out << ',';
        out << "{\"id\":" << tensors[i].first << ",\"pos\":\"UB\",\"size\":" << tensors[i].second << '}';
    }
    out << "],\"edges\":[";
    for (size_t i = 0; i < edges.size(); ++i) {
        if (i) out << ',';
        out << "{\"source\":" << edges[i].first << ",\"target\":" << edges[i].second << '}';
    }
    out << "]}"; return Graph(JsonParser(out.str()).parse());
}
Solution plan(vector<int> core, vector<int> order, vector<int> group = {}) {
    Solution s; s.core = move(core); s.order = move(order); s.placement_group = move(group); return s;
}
void verify(const Graph &g, const Solution &seed, const vector<Solution> &candidates) {
    for (const auto &s : candidates) {
        validate(g, s, 2);
        require(s.core == seed.core && s.placement_group == seed.placement_group,
                "timeline feedback changed mapping or placement metadata");
        const auto timing = official_timing(g, s, 2);
        require(timing.valid, timing.error);
    }
}
int pos(const Solution &s, int v) { return int(find(s.order.begin(), s.order.end(), v) - s.order.begin()); }
void downstream_overlap() {
    const auto g = graph({{2,100},{2,10},{3,1000}}, {{1,2}});
    const auto s = plan({0,0,0},{0,1,2},{0,1,0});
    const auto before = official_timing(g, s, 2); require(before.valid, before.error);
    const auto candidates = timing_feedback_candidates(g, 2, s, before);
    require(!candidates.empty(), "critical downstream work was never advanced"); verify(g,s,candidates);
    bool improved = false;
    for (const auto &p : candidates) improved |= official_timing(g,p,2).cycles < before.cycles;
    require(before.cycles == 1110 && improved, "independent same-pipe reordering should expose downstream overlap");
}
void idle_hole_and_global_closure() {
    // Op 1 is later globally but on core 1; advancing 2 before 0 must also move
    // predecessor 1 globally. This must not change either core assignment.
    const auto g = graph({{2,100},{4,20},{2,10},{3,1000}}, {{1,2},{2,3}});
    const auto s = plan({0,1,0,0},{0,1,2,3},{0,1,2,0});
    const auto timing = official_timing(g,s,2); require(timing.valid,timing.error);
    const auto candidates = timing_feedback_candidates(g,2,s,timing); verify(g,s,candidates);
    auto colliding_ids = timing;
    colliding_ids.cores[1].timeline.push_back({g.ops[0].id,0,1});
    const auto with_generated_copy = timing_feedback_candidates(g,2,s,colliding_ids);
    require(with_generated_copy.size() == candidates.size(),"foreign-core generated ID hid original timing");
    for (size_t i = 0; i < candidates.size(); ++i)
        require(with_generated_copy[i].order == candidates[i].order,"execution IDs must be scoped by core");
    bool closure = false;
    for (const auto &p : candidates) if (pos(p,2) < pos(p,0)) {
        require(pos(p,1) < pos(p,2), "cross-core predecessor missing from closure"); closure = true;
    }
    require(closure,"global predecessor closure candidate missing");
    // Separate remote dependency makes a hole before the blocked pipe head.
    const auto gap = graph({{2,50},{2,10},{3,1000},{2,1000}},{{3,0},{1,2}});
    const auto seed = plan({0,0,0,1},{3,0,1,2});
    const auto before = official_timing(gap,seed,2); require(before.valid,before.error);
    const auto reordered = timing_feedback_candidates(gap,2,seed,before); verify(gap,seed,reordered);
    bool improved = false;
    for (const auto &p : reordered) improved |= official_timing(gap,p,2).cycles < before.cycles;
    require(improved,"independent work should fill the remote-wait pipe hole");
}
void writer_and_dependency_guards() {
    const auto writers = graph({{2,100},{2,10},{4,1},{3,1000}},{{1,100},{2,100},{100,3}},{{100,60}});
    const auto s = plan({0,0,0,0},{0,2,1,3},{0,1,1,0});
    const auto timing = official_timing(writers,s,2); require(timing.valid,timing.error);
    const auto candidates = timing_feedback_candidates(writers,2,s,timing);
    require(!candidates.empty(),"writer fixture produced no reorder"); verify(writers,s,candidates);
    for (const auto &p : candidates) require(p.core[1] == p.core[2],"mandatory writers split");
    const auto chain = graph({{2,100},{2,10},{3,1000}},{{0,1},{1,2}});
    const auto dependent = plan({0,0,0},{0,1,2});
    const auto measured = official_timing(chain,dependent,2); require(measured.valid,measured.error);
    require(timing_feedback_candidates(chain,2,dependent,measured).empty(),"ancestor anchor illegally crossed");
}
void bounded_deterministic_and_missing_timing() {
    const auto g = graph({{2,100},{4,20},{2,10},{3,1000}},{{1,2},{2,3}});
    const auto s = plan({0,0,0,0},{0,1,2,3});
    const auto timing = official_timing(g,s,2); require(timing.valid,timing.error);
    const auto a = timing_feedback_candidates(g,2,s,timing,1), b = timing_feedback_candidates(g,2,s,timing,1);
    require(a.size() == 1 && a[0].order == b[0].order && a[0].label == b[0].label,"not deterministic or output cap ignored");
    TimingFeedbackLimits limits; limits.closure_nodes = 1;
    require(timing_feedback_candidates(g,2,s,timing,4,limits).empty(),"partial predecessor closure emitted");
    limits = {}; limits.per_core_distance = 1;
    require(timing_feedback_candidates(g,2,s,timing,4,limits).empty(),"distance cap ignored");
    limits = {}; limits.closure_attempts = 0;
    require(timing_feedback_candidates(g,2,s,timing,4,limits).empty(),"zero attempt budget ignored");
    auto missing = timing; missing.cores[0].timeline.clear();
    require(timing_feedback_candidates(g,2,s,missing).empty(),"incomplete time evidence used");
    require(s.order == vector<int>({0,1,2,3}),"input plan was mutated");
}
} // namespace
int main() {
    try {
        downstream_overlap(); idle_hole_and_global_closure(); writer_and_dependency_guards();
        bounded_deterministic_and_missing_timing();
        cout << "timing feedback tests passed\n"; return 0;
    } catch (const exception &e) { cerr << e.what() << '\n'; return 1; }
}
