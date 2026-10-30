#include "annealing.hpp"

using namespace dag_chain;
namespace {
void require(bool ok, const string &message) { if (!ok) throw runtime_error(message); }
Graph graph(const vector<pair<int, i64>> &ops, const vector<pair<int, int>> &edges = {},
            const vector<pair<i64, i64>> &tensors = {}, i64 direct_size = 0) {
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
        out << "{\"source\":" << edges[i].first << ",\"target\":" << edges[i].second;
        if (edges[i].first < int(ops.size()) && edges[i].second < int(ops.size()))
            out << ",\"data_size\":" << direct_size;
        out << '}';
    }
    out << "]}"; return Graph(JsonParser(out.str()).parse());
}
Decomposition groups(vector<vector<int>> bundles) { Decomposition d; d.bundles = move(bundles); return d; }
void verify_counts(const AnnealingStats &stats) {
    require(stats.proposals == stats.evaluated + stats.skipped_size + stats.skipped_budget + stats.skipped_noop,
            "proposal accounting lost a branch");
    require(stats.evaluated == stats.accepted + stats.rejected &&
            stats.proposals == stats.move_proposals + stats.swap_proposals,
            "acceptance or neighborhood accounting mismatch");
    require(stats.uphill_accepted <= stats.accepted && stats.undo_nodes <= stats.touched_nodes,
            "rollback or uphill counters are inconsistent");
}
double energy(const Graph &g, int cores, const vector<int> &assignment, const AnnealingLimits &limits) {
    const Config cfg;
    TrafficState state(g,cfg,cores,assignment); Row total{};
    for (int v = 0; v < g.size(); ++v) {
        state.load[assignment[v]][g.ops[v].pipe] += g.weight(v) - double(g.ops[v].cycles);
        total[g.ops[v].pipe] += g.weight(v);
    }
    double square = 0;
    for (const auto &row : state.load) for (int p = 0; p < 5; ++p) square += row[p]*row[p]/max(1.0,total[p]);
    return state.light_score(limits.route_weight) + limits.balance_weight*square;
}
void verify(const Graph &g, int cores, const AnnealingResult &result, const AnnealingLimits &limits) {
    for (const auto &candidate : result.candidates) {
        Solution s; s.core = candidate.assignment; s.order = g.topo; s.placement_group = result.placement_group;
        validate(g,s,cores);
        require(abs(candidate.energy-energy(g,cores,candidate.assignment,limits)) < 1e-8,
                "returned candidate energy differs from a fresh full computation");
        const Config cfg; TrafficState fresh(g,cfg,cores,candidate.assignment);
        require(candidate.route_bytes == fresh.bytes,"incremental tensor/direct traffic disagrees with full counts");
    }
}
void true_annealing_and_greedy_fallback() {
    const auto g = graph({{2,100},{2,100}});
    const auto d = decompose(g,2,0.5);
    const auto greedy = pack_decomposition(g,d,2,2.0);
    AnnealingLimits limits; limits.proposals = 64; limits.swap_percent = 0;
    limits.start_temperature = 1000000; limits.end_temperature = 10000;
    AnnealingStats stats;
    const auto result = anneal_assignment(g,d,2,greedy,limits,&stats);
    verify_counts(stats);
    require(stats.uphill_accepted > 0,"algorithm never accepted a worse state at high temperature");
    require(stats.final_temperature < stats.initial_temperature,"temperature did not cool");
    require(stats.best_energy <= stats.initial_energy,"best proxy state lost the greedy fallback");
    require(stats.initial_energy == energy(g,2,greedy,limits),"annealing did not start from the supplied greedy assignment");
    verify(g,2,result,limits);
}
void rollback_and_budgets() {
    const auto g = graph({{2,100},{2,100}},{{100,0},{100,1}},{{100,600}});
    const auto d = groups({{0},{1}}); const vector<int> greedy{0,1};
    AnnealingLimits limits; limits.proposals = 96; limits.swap_percent = 0;
    limits.start_temperature = 1e-20; limits.end_temperature = 1e-22;
    AnnealingStats stats;
    const auto result = anneal_assignment(g,d,2,greedy,limits,&stats);
    verify_counts(stats);
    require(stats.rejected == 96 && stats.accepted == 0,"cold uphill proposals should all be rejected");
    require(stats.final_energy == stats.initial_energy && result.best_assignment == greedy,
            "rejected moves did not restore exact totals/assignment");
    require(stats.undo_nodes == stats.touched_nodes && stats.rebasings >= 2,"rollback/rebase not exercised");
    verify(g,2,result,limits);
    limits.node_touches = 3;
    anneal_assignment(g,d,2,greedy,limits,&stats);
    verify_counts(stats);
    require(stats.touched_nodes == 3 && stats.evaluated == 3 && stats.skipped_budget > 0,"node-touch bound ignored");
    limits.proposals = 0;
    const auto stopped = anneal_assignment(g,d,2,greedy,limits,&stats);
    verify_counts(stats);
    require(stats.proposals == 0 && stopped.best_assignment == greedy,"zero-step run changed greedy");
}
void transitive_writers_and_nonconvex_groups() {
    const auto g = graph({{2,1},{2,2},{3,3},{3,4}},{{1,100},{2,100},{100,3}},{{100,60}});
    const auto d = groups({{0,1},{2},{3}});
    AnnealingLimits limits; limits.proposals = 128; limits.move_nodes = 2;
    AnnealingStats stats;
    const auto result = anneal_assignment(g,d,2,{0,0,0,1},limits,&stats);
    verify_counts(stats);
    require(result.placement_group[0] == result.placement_group[1] &&
            result.placement_group[1] == result.placement_group[2],"writer closure was not transitive over bundles");
    require(stats.skipped_size > 0,"oversized whole writer closure was not skipped");
    for (const auto &candidate : result.candidates)
        require(candidate.assignment[0] == 0 && candidate.assignment[1] == 0 && candidate.assignment[2] == 0,
                "oversized group was partially moved");
    verify(g,2,result,limits);
    bool rejected = false;
    try { anneal_assignment(g,d,2,{0,0,1,1},limits); } catch (const invalid_argument &) { rejected = true; }
    require(rejected,"greedy input that splits writer closure accepted");
    const auto chain = graph({{2,10},{3,5},{2,10}},{{0,1},{1,2}});
    const auto nonconvex = groups({{0,2},{1}});
    limits.move_nodes = 8;
    const auto interleaved = anneal_assignment(chain,nonconvex,2,{0,1,0},limits);
    require(!interleaved.candidates.empty(),"nonconvex co-location group incorrectly contracted into cyclic tasks");
    verify(chain,2,interleaved,limits);
    for (const auto &candidate : interleaved.candidates) {
        SeedOptions options; const Config cfg;
        auto scheduled = operation_plan(chain,2,cfg,options,&candidate.assignment,nullptr);
        scheduled.placement_group = interleaved.placement_group;
        validate(chain,scheduled,2);
        require(scheduled.order == vector<int>({0,1,2}),"group forced adjacent execution instead of DAG order");
    }
}
void swaps_fanout_and_zero_cycles() {
    const auto g = graph({{2,0},{2,0},{2,0},{3,0},{3,0}},
                         {{0,100},{100,1},{100,2},{1,3},{2,4}},{{100,4096}},137);
    AnnealingLimits limits; limits.proposals = 160; limits.swap_percent = 100;
    limits.start_temperature = 5; limits.end_temperature = 0.1;
    AnnealingStats stats;
    const auto result = anneal_assignment(g,groups({{0},{1},{2},{3},{4}}),3,{0,1,1,2,2},limits,&stats);
    verify_counts(stats);
    require(stats.swap_proposals == limits.proposals && stats.evaluated > 0 && stats.accepted > 0,
            "swap neighborhood not exercised");
    require(stats.initial_route_bytes == 8192 + 548,"fanout COPY dedup or direct-edge transfer bytes are wrong");
    require(stats.initial_pipe_bound == 2,"zero-cycle compute operations must use max(1,cycles)");
    verify(g,3,result,limits);
    // Only consumer 1 can move; consumer 2 remains on the original destination.
    // Its shared input COPY must therefore remain after every accepted move.
    const auto sharing = graph({{2,0},{2,0},{2,0},{2,0},{2,0}},{{0,100},{100,1},{100,2}},{{100,4096}});
    limits.swap_percent = 0; limits.move_nodes = 1;
    const auto shared = anneal_assignment(sharing,groups({{0,3},{1},{2,4}}),2,{0,1,1,0,1},limits,&stats);
    verify_counts(stats);
    require(stats.accepted > 0 && stats.initial_route_bytes == 8192,"shared-consumer fixture did not exercise moves");
    for (const auto &candidate : shared.candidates) require(candidate.route_bytes == 8192,"moving one consumer removed another's copy");
    verify(sharing,2,shared,limits);
}
void deterministic_and_invalid_decomposition() {
    const auto g = graph({{2,9},{3,7},{2,5},{3,3}});
    const auto d = groups({{0},{1},{2},{3}});
    AnnealingLimits limits; limits.proposals = 96; limits.retained = 2;
    AnnealingStats a_stats,b_stats;
    const auto a = anneal_assignment(g,d,3,{0,1,2,0},limits,&a_stats);
    const auto b = anneal_assignment(g,d,3,{0,1,2,0},limits,&b_stats);
    verify_counts(a_stats); verify_counts(b_stats);
    require(a.best_assignment == b.best_assignment && a.candidates.size() == b.candidates.size() &&
            a_stats.uphill_accepted == b_stats.uphill_accepted,"fixed-seed run is not deterministic");
    require(a.candidates.size() <= 2,"retained-state cap ignored");
    for (size_t i = 0; i < a.candidates.size(); ++i)
        require(a.candidates[i].assignment == b.candidates[i].assignment && a.candidates[i].energy == b.candidates[i].energy,
                "repeated run candidate differs");
    bool rejected = false;
    try { anneal_assignment(g,groups({{0,1},{1,2,3}}),3,{0,0,0,0},limits); }
    catch (const invalid_argument &) { rejected = true; }
    require(rejected,"duplicate bundle member accepted");
}
} // namespace
int main() {
    try {
        true_annealing_and_greedy_fallback(); rollback_and_budgets(); transitive_writers_and_nonconvex_groups();
        swaps_fanout_and_zero_cycles(); deterministic_and_invalid_decomposition();
        cout << "annealing tests passed\n"; return 0;
    } catch (const exception &e) { cerr << e.what() << '\n'; return 1; }
}
