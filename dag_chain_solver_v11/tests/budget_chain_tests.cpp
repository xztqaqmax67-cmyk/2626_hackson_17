#include "chain.hpp"
#include "official_timing.hpp"

using namespace dag_chain;
namespace {
void require(bool ok, const string &message) { if (!ok) throw runtime_error(message); }
Graph graph(int count) {
    ostringstream out; out << "{\"ops\":[";
    for (int v = 0; v < count; ++v) {
        if (v) out << ',';
        out << "{\"id\":" << v << ",\"op\":\"COMPUTE\",\"pipe\":\""
            << (v % 3 ? "PIPE_M" : "PIPE_V") << "\",\"cycles\":" << (10 + v % 7) << '}';
    }
    out << "],\"tensors\":[],\"edges\":[";
    for (int v = 2; v < count; ++v) {
        if (v > 2) out << ',';
        out << "{\"source\":" << v - 2 << ",\"target\":" << v << ",\"data_size\":0}";
    }
    out << "]}"; return Graph(JsonParser(out.str()).parse());
}
void check_complete(const Graph &g, int cores, const Result &r, const vector<Solution> &seeds) {
    validate(g, r.solution, cores);
    require(r.refined.valid && isfinite(r.refined.cycles), "deadline returned an unrefined winner");
    require(!seeds.empty(), "deadline lost the complete feedback incumbent");
    for (const auto &s : seeds) validate(g, s, cores);
    const auto exact = official_timing(g, r.solution, cores);
    require(exact.valid, exact.error);
}
void expired_budget_keeps_fallback() {
    const auto g = graph(16);
    Deadline deadline; deadline.start = Clock::now() - chrono::seconds(10); deadline.limit = 0.001;
    vector<Solution> seeds;
    const auto result = solve(g, 5, false, 7, &seeds, &deadline);
    check_complete(g, 5, result, seeds);
    require(result.solution.label == "budget_single_core_fallback", "already-expired search ran optional portfolios");
    require(result.candidates == 1 && result.refined_candidates == 1, "shared fallback counted once per core worker");
}
void short_budget_returns_completed_candidate() {
    const auto g = graph(48);
    Deadline deadline; deadline.limit = 0.01;
    vector<Solution> seeds;
    const auto result = solve(g, 4, false, 7, &seeds, &deadline);
    check_complete(g, 4, result, seeds);
    // Deliberately no timing or exact-plan assertion: the deadline can interrupt
    // a different candidate boundary on a slower/busier machine.
}
void disabled_deadline_preserves_fixed_search() {
    const auto g = graph(6);
    Deadline disabled; disabled.start = Clock::now() - chrono::seconds(10); disabled.limit = 0.001; disabled.enabled = false;
    vector<Solution> left_seeds, right_seeds;
    const auto left = solve(g, 2, true, 0, &left_seeds);
    const auto right = solve(g, 2, true, 0, &right_seeds, &disabled);
    require(solution_json(g,left.solution,2) == solution_json(g,right.solution,2), "disabled deadline changed fixed result");
    require(left.candidates == right.candidates && left.refined_candidates == right.refined_candidates &&
            left.refined.cycles == right.refined.cycles, "disabled deadline changed fixed search work");
    check_complete(g, 2, right, right_seeds);
}
} // namespace
int main() {
    try {
        expired_budget_keeps_fallback(); short_budget_returns_completed_candidate();
        disabled_deadline_preserves_fixed_search();
        cout << "budget chain tests passed\n"; return 0;
    } catch (const exception &e) { cerr << e.what() << '\n'; return 1; }
}
