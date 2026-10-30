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
    require(solution_json(g,s,cores)==saved,"feedback mutated the incumbent input");
    require(after.valid && after.route_bytes+after.spill_bytes<=before.route_bytes+before.spill_bytes,
            "feedback increased exact total transfers");
    require(result.refined.valid && result.refined.cycles<=base.refined.cycles,"feedback accepted estimated slowdown");
    require(stats.initial_trace_valid && stats.before_spill_bytes==before.spill_bytes &&
            stats.after_spill_bytes==after.spill_bytes,"feedback trace statistics disagree with full replays");
    require(stats.rounds<=dag_chain::search_limits::feedback_rounds,"feedback exceeded round limit");
    require(stats.generated_candidates<=stats.rounds*dag_chain::search_limits::feedback_beam*
            dag_chain::search_limits::feedback_candidates_per_round,
            "feedback exceeded proposal cap");
    require(stats.traced_candidates<=dag_chain::search_limits::feedback_replay_budget &&
            stats.refined_candidates<=dag_chain::search_limits::feedback_refine_budget,
            "feedback exceeded shared replay/refine budget");
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

Graph fixture(int count, const vector<i64> &sizes, const vector<pair<int,int>> &edges) {
    ostringstream out; out<<"{\"ops\":[";
    for(int i=0;i<count;++i){if(i)out<<',';out<<"{\"id\":"<<i<<",\"op\":\"COMPUTE\",\"pipe\":\"PIPE_V\",\"cycles\":50}";}
    out<<"],\"tensors\":[";
    for(size_t i=0;i<sizes.size();++i){if(i)out<<',';out<<"{\"id\":"<<10000+i<<",\"pos\":\"UB\",\"size\":"<<sizes[i]<<'}';}
    out<<"],\"edges\":[";
    for(size_t i=0;i<edges.size();++i){if(i)out<<',';out<<"{\"source\":"<<edges[i].first<<",\"target\":"<<edges[i].second<<'}';}
    out<<"]}"; return Graph(JsonParser(out.str()).parse());
}
Solution consecutive(const Graph &g) {
    Solution s;s.core.assign(g.size(),0);s.order.resize(g.size());iota(s.order.begin(),s.order.end(),0);return s;
}
void coverage_and_local_distance() {
    const auto g=fixture(304,{65536,65536},{{0,10000},{10000,2},{1,10001},{10001,3}});
    auto s=consecutive(g);
    s.order={0,1};for(int v=4;v<304;++v){s.order.push_back(v);s.core[v]=1;}s.order.push_back(2);s.order.push_back(3);
    const auto trace=dag_chain::official_spill_trace(g,s,2);
    require(trace.valid&&!trace.records.empty(),"distance fixture must spill");
    dag_chain::FeedbackStats stats;
    auto candidates=dag_chain::spill_feedback_candidates(g,2,s,trace,nullptr,&stats);
    bool pull=false,producer=false;
    for(const auto &q:candidates){validate(g,q,2);require(q.core==s.core,"reorder changed mapping");
        if(q.label.find("spill_feedback_0_")==0)pull=true;
        if(q.label.find("spill_feedback_4_")==0)producer=true;
    }
    require(pull,"300 operations on another core must not consume the local distance allowance");
    require(producer,"a real producer should be eligible for the late-production strategy");

    // Feed eight costly unmovable anchors ahead of a valid event. This tests
    // scanning/legality backfill independently of the official victim policy.
    auto crafted=trace; crafted.records.clear();
    auto valid=trace.records.front();valid.tensor_id=10000;valid.tensor_index=0;
    valid.trigger_owner=1;valid.next_owner=2;valid.trigger_step=1;valid.next_use_step=2;
    for(int i=0;i<8;++i){auto bad=valid;bad.size=1000000+i;bad.next_owner=bad.trigger_owner;crafted.records.push_back(bad);}
    crafted.records.push_back(valid);
    size_t examined=0;dag_chain::FeedbackStats backfill;
    auto recovered=dag_chain::spill_feedback_candidates(g,2,s,crafted,&examined,&backfill);
    require(examined>8&&!recovered.empty()&&backfill.rejected_anchor>=8,
            "unmovable top-eight records must be replaced by later legal records");
}
void neutral_then_saving() {
    // General small DAG: no one-step generated order reduces exact spill, but
    // moving the short 2->3 chain and then delaying writer 0 does. No public
    // case IDs, graph sizes or input-specific policy occur in the algorithm.
    const auto g=fixture(9,{32768,32768,98304,65536,32768},
        {{0,10002},{2,10003},{4,10000},{6,10004},{7,10001},
         {10000,5},{10001,8},{10002,5},{10002,8},{10003,3},{10004,7}});
    const auto s=consecutive(g);
    const auto before=dag_chain::official_spill_trace(g,s,2);
    require(before.valid&&before.spill_bytes==294912,"neutral fixture initial trace changed");
    bool has_neutral=false,has_second_gain=false;
    for(const auto &one:dag_chain::spill_feedback_candidates(g,2,s,before)){
        const auto trace=dag_chain::official_spill_trace(g,one,2);
        require(trace.valid&&trace.spill_bytes>=before.spill_bytes,"fixture must have no immediate spill gain");
        if(trace.spill_bytes!=before.spill_bytes)continue;
        has_neutral=true;
        for(const auto &two:dag_chain::spill_feedback_candidates(g,2,one,trace)){
            const auto after=dag_chain::official_spill_trace(g,two,2);
            if(after.valid&&after.spill_bytes<before.spill_bytes)has_second_gain=true;
        }
    }
    require(has_neutral&&has_second_gain,"fixture must require an equal-spill intermediate state");
    dag_chain::FeedbackStats stats;
    const auto improved=dag_chain::improve_spill_order(g,2,incumbent(g,s,2),&stats);
    const auto after=dag_chain::official_spill_trace(g,improved.solution,2);
    require(after.valid&&after.spill_bytes<before.spill_bytes&&stats.best_neutral_steps>0,
            "bounded beam failed to keep and improve a neutral intermediate state");
    require(stats.traced_candidates<=dag_chain::search_limits::feedback_replay_budget&&
            stats.refined_candidates<=dag_chain::search_limits::feedback_refine_budget,
            "two-step search exceeded its shared budget");
    auto parsed=JsonParser(dag_chain::feedback_stats_json(stats)).parse();
    require(parsed.at("best_neutral_steps").integer()>0,"stats must serialize actual neutral path evidence");
}
} // namespace
int main() {
    try {
        for (bool relabel:{false,true})
            for (int cores:{2,5}) {
                check(false,relabel,cores);
                check(true,relabel,cores);
            }
        coverage_and_local_distance();
        neutral_then_saving();
        cout << "spill feedback, legality backfill, local distance, late producers, neutral beam and shared budgets passed\n";
        return 0;
    } catch (const exception &e) {
        cerr << e.what() << '\n';
        return 1;
    }
}
