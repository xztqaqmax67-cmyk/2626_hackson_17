#include "spill_feedback.hpp"
#include "search_limits.hpp"

namespace {
void require(bool condition, const string &message) {
    if (!condition) throw runtime_error(message);
}
Graph graph(bool forced_overlap, bool relabel) {
    const int count = forced_overlap ? 3 : 4;
    auto id = [&](int v) { return relabel ? 9000 - 17 * v : v; };
    ostringstream json;
    json << "{\"ops\":[";
    for (int v = count - 1; v >= 0; --v) {
        if (v != count - 1) json << ',';
        json << "{\"id\":" << id(v) << ",\"op\":\"COMPUTE\",\"pipe\":\"PIPE_V\",\"cycles\":50}";
    }
    json << "],\"tensors\":[{\"id\":11000,\"pos\":\"UB\",\"size\":65536},"
            "{\"id\":10000,\"pos\":\"UB\",\"size\":65536}],\"edges\":[";
    vector<pair<int,int>> edges{{id(0),10000},{10000,id(2)},{id(1),11000},{11000,id(forced_overlap ? 2 : 3)}};
    for (size_t i = 0; i < edges.size(); ++i) {
        if (i) json << ',';
        json << "{\"source\":" << edges[i].first << ",\"target\":" << edges[i].second << '}';
    }
    json << "]}";
    return Graph(JsonParser(json.str()).parse());
}
Solution seed_for(const Graph &g, bool relabel) {
    Solution seed;
    seed.core.assign(g.size(),0);
    for (int logical=0;logical<g.size();++logical) {
        const i64 id=relabel ? 9000-17*logical : logical;
        auto it=find_if(g.ops.begin(),g.ops.end(),[&](const Op &op){return op.id==id;});
        require(it!=g.ops.end(),"fixture operation missing");
        seed.order.push_back(int(it-g.ops.begin()));
    }
    seed.label="pressure_before_last_consumer";
    return seed;
}
dag_chain::Result incumbent(const Graph &g,const Solution &s,int cores) {
    dag_chain::Result r;
    r.solution=s;
    r.estimate=evaluate_proxy(g,cores,Config{},s,0.03,nullptr);
    r.refined=dag_chain::refine_estimate(g,s,cores);
    r.generated_for_cores=cores;
    require(r.refined.valid,"fixture requires a feasible incumbent estimate");
    return r;
}
void check(bool forced_overlap,bool relabel,int cores) {
    const auto g=graph(forced_overlap,relabel);
    const auto s=seed_for(g,relabel);
    const auto saved=solution_json(g,s,cores);
    const auto before=dag_chain::official_spill_trace(g,s,cores);
    require(before.valid && before.spill_bytes>0,"fixture must have actual Step2 spills");
    const auto base=incumbent(g,s,cores);
    dag_chain::FeedbackStats stats;
    const auto result=dag_chain::improve_spill_order(g,cores,base,&stats);
    const auto after=dag_chain::official_spill_trace(g,result.solution,cores);
    validate(g,result.solution,cores);
    require(result.solution.core==s.core,"feedback changed core assignment");
    require(solution_json(g,s,cores)==saved,"feedback mutated the incumbent input");
    require(after.valid && after.route_bytes==before.route_bytes,"feedback changed boundary traffic");
    require(result.refined.valid && result.refined.cycles<=base.refined.cycles,"feedback accepted estimated slowdown");
    require(stats.initial_trace_valid && stats.before_spill_bytes==before.spill_bytes &&
            stats.after_spill_bytes==after.spill_bytes,"feedback trace statistics disagree with full replays");
    require(stats.rounds<=dag_chain::search_limits::feedback_rounds,"feedback exceeded round limit");
    require(stats.generated_candidates<=stats.rounds*dag_chain::search_limits::feedback_candidates_per_round,
            "feedback exceeded proposal cap");
    if (forced_overlap) {
        require(after.spill_bytes==before.spill_bytes && stats.accepted_rounds==0,
                "joint-input fixture has unavoidable spill under the fixed soft-threshold sequence rule");
        require(solution_json(g,result.solution,cores)==saved,"no strict saving should preserve original solution");
    } else {
        require(after.spill_bytes==0 && stats.accepted_rounds>0,
                "independent last consumer should remove the exact avoidable spill");
    }
    dag_chain::FeedbackStats again_stats;
    const auto again=dag_chain::improve_spill_order(g,cores,base,&again_stats);
    require(solution_json(g,again.solution,cores)==solution_json(g,result.solution,cores),
            "fixed feedback must be deterministic");
}
} // namespace
int main() {
    try {
        for (bool relabel:{false,true})
            for (int cores:{2,5}) {
                check(false,relabel,cores);
                check(true,relabel,cores);
            }
        cout << "actual-victim feedback, strict savings, preserved mapping/dependencies and determinism passed\n";
        return 0;
    } catch (const exception &e) {
        cerr << e.what() << '\n';
        return 1;
    }
}
