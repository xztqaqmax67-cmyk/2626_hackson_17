#include "communication.hpp"
#include "spill_mapping.hpp"
#include "search_limits.hpp"

namespace {
void require(bool ok, const string &message) {
    if (!ok) throw runtime_error(message);
}
Graph direct_graph(const vector<i64> &cycles, const vector<tuple<int,int,i64>> &edges) {
    ostringstream out;
    out << "{\"ops\":[";
    for (size_t i=0;i<cycles.size();++i) {
        if (i) out << ',';
        out << "{\"id\":" << i << ",\"op\":\"COMPUTE\",\"pipe\":\"PIPE_V\",\"cycles\":" << cycles[i] << '}';
    }
    out << "],\"tensors\":[],\"edges\":[";
    for (size_t i=0;i<edges.size();++i) {
        if (i) out << ',';
        auto [a,b,size]=edges[i];
        out << "{\"source\":" << a << ",\"target\":" << b << ",\"data_size\":" << size << '}';
    }
    out << "]}";
    return Graph(JsonParser(out.str()).parse());
}
Graph writers_graph(int n=5) {
    ostringstream out;
    out << "{\"ops\":[";
    for (int v=0;v<n;++v) {
        if (v) out << ',';
        out << "{\"id\":" << v << ",\"op\":\"COMPUTE\",\"pipe\":\"PIPE_V\",\"cycles\":50}";
    }
    out << R"(],"tensors":[{"id":1000,"size":65536,"pos":"UB"},{"id":1001,"size":65536,"pos":"UB"}],
      "edges":[{"source":0,"target":1000},{"source":2,"target":1000},{"source":1000,"target":3},
      {"source":1,"target":1001},{"source":1001,"target":4}]})";
    return Graph(JsonParser(out.str()).parse());
}
Solution consecutive(const Graph &g, int core=0) {
    Solution s;
    s.core.assign(g.size(),core);s.order=g.topo;
    return s;
}
void rejects_groups(const Graph &g, const Solution &s) {
    bool rejected=false;
    try {validate(g,s,2);} catch (const runtime_error &) {rejected=true;}
    require(rejected,"malformed or split placement groups must be rejected");
}
void transfer_priority() {
    // The slow predecessor has only a tiny edge. Parent selection must use
    // transfer cost, not finish time, for this communication-only policy.
    const auto g=direct_graph({10000,1,1},{{0,2,32},{1,2,65536}});
    const auto d=dag_chain::communication_decompose(g,2,1.0);
    require(d.parent[2]==1 && d.next[1]==2 && d.next[0]<0,
            "communication parent ignored the larger transfer");
    const auto fan=direct_graph({1,1,1,1},{{0,1,100},{0,2,200},{3,1,150}});
    const auto f=dag_chain::communication_decompose(fan,2,2.0);
    require(f.next[0]==2 && f.parent[2]==0 && f.next[3]==1 && f.parent[1]==3,
            "fan-out and alternative incoming edge must use the same transfer priority");
    vector<int> uses(fan.size());
    for (const auto &path:f.paths)
        for (int v:path) ++uses[v];
    require(all_of(uses.begin(),uses.end(),[](int count){return count==1;}),
            "communication paths do not partition all operations exactly once");
}
void cheap_cuts() {
    const auto g=direct_graph({10,10,10,10,10},{{0,1,6000},{1,2,60},{2,3,6000},{3,4,6000}});
    const auto d=dag_chain::communication_decompose(g,2,1.2);
    require(d.bundle_of[0]==d.bundle_of[1] && d.bundle_of[1]!=d.bundle_of[2] &&
            d.bundle_of[2]==d.bundle_of[3] && d.bundle_of[3]==d.bundle_of[4],
            "cut must prefer the cheap feasible boundary over a maximal left prefix");
}
void groups_are_not_atomic() {
    // Contracting group {A,C} into an indivisible operation would create a
    // group-level cycle through B. The original DAG A->B->C is perfectly legal.
    const auto g=direct_graph({10,10,10},{{0,1,1},{1,2,1},{0,2,10000}});
    const auto d=dag_chain::communication_decompose(g,2,2.0);
    require(d.bundle_of[0]==d.bundle_of[2] && d.bundle_of[0]!=d.bundle_of[1],
            "interleaving fixture must retain the strong A-C edge only");
    vector<int> assignment(g.size(),0);
    SeedOptions options;options.mode="critical";options.window=8;options.memory=0.5;
    auto s=operation_plan(g,2,Config{},options,&assignment,nullptr);
    s.placement_group=d.bundle_of;
    validate(g,s,2);
    require(s.order==vector<int>({0,1,2}),"group incorrectly forced contiguous execution");
    require(dag_chain::official_spill_trace(g,s,2).valid,"interleaved group failed exact replay");
    const auto encoded=JsonParser(solution_json(g,s,2)).parse();
    require(encoded.fields.size()==2 && !encoded.find("placement_group"),
            "internal placement groups leaked into the official plan contract");
    auto bad=s;bad.core[2]=1;rejects_groups(g,bad);
    bad=s;bad.placement_group.pop_back();rejects_groups(g,bad);
    bad=s;bad.placement_group[2]=g.size();rejects_groups(g,bad);
    bad=s;bad.placement_group[0]=-1;rejects_groups(g,bad);
}
void streaming_group_reuses_memory() {
    // Three 48-KiB tensors exceed UB when summed, but the chain only needs
    // its current input and output simultaneously (96 KiB). A co-location
    // group is not required to reserve the union of all its future tensors.
    const Graph g(JsonParser(R"({"ops":[
        {"id":0,"op":"COMPUTE","pipe":"PIPE_V","cycles":10},
        {"id":1,"op":"COMPUTE","pipe":"PIPE_V","cycles":10},
        {"id":2,"op":"COMPUTE","pipe":"PIPE_V","cycles":10},
        {"id":3,"op":"COMPUTE","pipe":"PIPE_V","cycles":10}],
        "tensors":[{"id":1000,"size":49152,"pos":"UB"},
                   {"id":1001,"size":49152,"pos":"UB"},
                   {"id":1002,"size":49152,"pos":"UB"}],
        "edges":[{"source":0,"target":1000},{"source":1000,"target":1},
                 {"source":1,"target":1001},{"source":1001,"target":2},
                 {"source":2,"target":1002},{"source":1002,"target":3}]})").parse());
    const Config config;
    require(3*49152>config.capacity[1] && 2*49152<config.spill_limit(1),
            "streaming fixture must exceed cumulative capacity but fit live memory");
    const auto guarded=dag_chain::communication_decompose(g,2,2.0,true);
    const auto relaxed=dag_chain::communication_decompose(g,2,2.0,false);
    require(guarded.bundles.size()>1 && relaxed.bundles.size()==1,
            "optional cumulative-footprint heuristic must not forbid streaming co-location");
    vector<int> assignment(g.size(),0);
    SeedOptions options;options.mode="critical";options.window=8;options.memory=0.5;
    auto plan=operation_plan(g,2,config,options,&assignment,nullptr);
    plan.placement_group=relaxed.bundle_of;validate(g,plan,2);
    const auto trace=dag_chain::official_spill_trace(g,plan,2);
    require(trace.valid && trace.spill_bytes==0 && trace.cores[0].peak[1]<=2*49152,
            "streaming group must reuse UB without spills in exact Step2 replay");
    const auto timing=dag_chain::refine_estimate(g,plan,2);
    require(timing.valid && timing.cycles==40,
            "streaming group should execute all original operations with bounded live memory");
}
void writer_and_group_closure() {
    const auto g=writers_graph();
    auto s=consecutive(g);
    // 0 and 2 are joint writers. The two placement groups overlap that writer
    // atom transitively, so a legal move of any of {0,1,2,4} moves all four.
    s.placement_group={0,0,2,3,2};validate(g,s,2);
    const auto trace=dag_chain::official_spill_trace(g,s,2);
    require(trace.valid && trace.spill_bytes>0,"closure fixture must have actual spill");
    bool moved=false;
    const auto candidates=dag_chain::spill_mapping_candidates(g,2,s,trace);
    require(!candidates.empty(),"closure fixture produced no mapping proposals");
    for (const auto &candidate:candidates) {
        validate(g,candidate,2);
        require(candidate.placement_group==s.placement_group && candidate.order==s.order,
                "mapping discarded placement constraints or changed DAG order");
        require(candidate.core[0]==candidate.core[1] && candidate.core[1]==candidate.core[2] &&
                candidate.core[2]==candidate.core[4],"mapping failed transitive group/writer closure");
        moved |= candidate.core[0]!=s.core[0];
    }
    require(moved,"closure test did not exercise moving the joined atom");
    const auto d=dag_chain::communication_decompose(g,2,0.5);
    require(d.bundle_of[0]==d.bundle_of[2],"communication grouping split mandatory writers");
    const auto large=writers_graph(int(dag_chain::search_limits::feedback_mapping_nodes)+1);
    auto all=consecutive(large);all.placement_group.assign(large.size(),0);
    const auto overflow=dag_chain::official_spill_trace(large,all,2);
    require(overflow.valid && !overflow.records.empty(),"large closure fixture needs spills");
    require(dag_chain::spill_mapping_candidates(large,2,all,overflow).empty(),
            "oversized group must be skipped whole, never truncated to fit move cap");
}
void tensor_fanout_traffic() {
    const Graph g(JsonParser(R"({"ops":[{"id":0,"op":"COMPUTE","pipe":"PIPE_V","cycles":10},
        {"id":1,"op":"COMPUTE","pipe":"PIPE_V","cycles":10},{"id":2,"op":"COMPUTE","pipe":"PIPE_V","cycles":10}],
        "tensors":[{"id":100,"size":4096,"pos":"L1"}],
        "edges":[{"source":0,"target":100},{"source":100,"target":1},{"source":100,"target":2}]})").parse());
    auto same=consecutive(g);same.core={0,1,1};
    const auto one=dag_chain::official_spill_trace(g,same,3);
    auto separate=same;separate.core[2]=2;
    const auto two=dag_chain::official_spill_trace(g,separate,3);
    require(one.valid && two.valid && one.spill_bytes==0 && two.spill_bytes==0,
            "fanout trace fixture invalid or spilled unexpectedly");
    require(one.route_bytes==8192 && two.route_bytes==16384,
            "same destination-core users must share a COPY pair");
}
void candidate_and_acceptance_guards() {
    const auto g=direct_graph({1000,1000,1000,1000,1000,1000},
                              {{0,2,1000},{1,3,8000},{2,4,2000},{3,5,8000}});
    dag_chain::CommunicationStats stats,again_stats;
    const auto candidates=dag_chain::communication_candidates(g,3,&stats);
    const auto again=dag_chain::communication_candidates(g,3,&again_stats);
    require(!candidates.empty() && candidates.size()==again.size(),"candidate generation empty or nondeterministic");
    require(stats.generated<=4*dag_chain::search_limits::communication_variant_limit,
            "communication candidate generation exceeded its bound");
    for (size_t i=0;i<candidates.size();++i) {
        validate(g,candidates[i],3);
        require(candidates[i].placement_group.size()==size_t(g.size()),"candidate lost its co-location groups");
        require(solution_json(g,candidates[i],3)==solution_json(g,again[i],3) &&
                candidates[i].placement_group==again[i].placement_group,"candidate repeat changed");
    }
    const auto original=dag_chain::solve(g,3);
    const auto before=dag_chain::official_spill_trace(g,original.solution,3);
    require(before.valid,"guard fixture original trace invalid");
    const auto improved=dag_chain::improve_communication_groups(g,3,original,&stats);
    const auto after=dag_chain::official_spill_trace(g,improved.solution,3);
    validate(g,improved.solution,3);
    require(after.valid && improved.refined.valid && improved.refined.cycles<=original.refined.cycles,
            "communication selection accepted estimated slowdown");
    require(after.route_bytes+after.spill_bytes<=before.route_bytes+before.spill_bytes,
            "communication selection accepted higher exact total traffic");
    require(stats.refined_candidates<=dag_chain::search_limits::communication_refine_candidates &&
            stats.feedback_runs<=1,"communication selection exceeded its refinement/feedback caps");
    const auto parsed=JsonParser(dag_chain::communication_stats_json(stats)).parse();
    require(parsed.find("selected_new"),"communication diagnostics are not structured JSON");
}
} // namespace
int main() {
    try {
        transfer_priority();cheap_cuts();groups_are_not_atomic();streaming_group_reuses_memory();writer_and_group_closure();
        tensor_fanout_traffic();candidate_and_acceptance_guards();
        cout << "communication-only grouping, cheap cuts, non-atomic interleaving, transitive closures and guarded deterministic selection passed\n";
        return 0;
    } catch (const exception &error) {
        cerr << error.what() << '\n';return 1;
    }
}
