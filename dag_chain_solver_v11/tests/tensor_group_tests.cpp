#include "tensor_groups.hpp"

namespace {
void require(bool value,const string &message) {if(!value)throw runtime_error(message);}
Graph graph(int n,const vector<pair<i64,i64>> &tensors,const vector<pair<int,int>> &edges,i64 cycles=100) {
    ostringstream s;s<<"{\"ops\":[";
    for(int v=0;v<n;++v){if(v)s<<',';s<<"{\"id\":"<<v<<",\"op\":\"COMPUTE\",\"pipe\":\"PIPE_V\",\"cycles\":"<<cycles<<'}';}
    s<<"],\"tensors\":[";
    for(size_t i=0;i<tensors.size();++i){if(i)s<<',';s<<"{\"id\":"<<tensors[i].first<<",\"size\":"<<tensors[i].second<<",\"pos\":\"UB\"}";}
    s<<"],\"edges\":[";
    for(size_t i=0;i<edges.size();++i){if(i)s<<',';s<<"{\"source\":"<<edges[i].first<<",\"target\":"<<edges[i].second<<'}';}
    s<<"]}";return Graph(JsonParser(s.str()).parse());
}
Solution seed(const Graph &g,vector<int> cores) {Solution s;s.core=move(cores);s.order=g.topo;return s;}
void compare_route_deltas(const Graph &g,int cores,const Solution &base,const vector<Solution> &plans,
                          const dag_chain::TensorGroupStats &stats) {
    const auto before=dag_chain::official_spill_trace(g,base,cores);
    require(before.valid,"test entry trace must be valid");
    require(plans.size()==stats.route_delta.size(),"diagnostic route delta count mismatch");
    for(size_t i=0;i<plans.size();++i) {
        validate(g,plans[i],cores);
        const auto after=dag_chain::official_spill_trace(g,plans[i],cores);
        require(after.valid,"tensor grouping cannot be replayed");
        require(double(after.route_bytes-before.route_bytes)==stats.route_delta[i],
                "marginal COPY must count actual destination-core sets, all affected tensors and direct edges");
    }
}
void fanout_and_shared_input() {
    // Two consumers already on one destination core share a single COPY pair.
    const auto g=graph(3,{{100,4096}},{{0,100},{100,1},{100,2}});
    const auto s=seed(g,{0,1,1});
    dag_chain::TensorGroupStats stats;
    const auto plans=dag_chain::tensor_group_candidates(g,3,s,nullptr,&stats);
    compare_route_deltas(g,3,s,plans,stats);
    bool branching_group=false;
    for(size_t i=0;i<plans.size();++i)
        if(plans[i].core[0]==plans[i].core[1] && plans[i].core[1]==plans[i].core[2]) {
            require(stats.route_delta[i]==-8192,"fanout gather incorrectly counted each consumer edge separately");
            require(plans[i].placement_group[0]==plans[i].placement_group[1] &&
                    plans[i].placement_group[1]==plans[i].placement_group[2],"branching group not represented");
            branching_group=true;
        }
    require(branching_group,"whole-Tensor gather must express a producer with multiple consumers");
    // No producer: each destination core independently imports this graph input.
    const auto input=graph(2,{{100,4096}},{{100,0},{100,1}});
    const auto inseed=seed(input,{0,1});
    const auto gathered=dag_chain::tensor_group_candidates(input,2,inseed,nullptr,&stats);
    compare_route_deltas(input,2,inseed,gathered,stats);
    bool saved_input=false;
    for(size_t i=0;i<gathered.size();++i) if(gathered[i].core[0]==gathered[i].core[1]) {
        require(stats.route_delta[i]==-4096,"shared input should save one read, not a write/read pair");saved_input=true;
    }
    require(saved_input,"shared graph-input consumers were never gathered");
}
void nonconvex_group_is_not_atomic() {
    const auto g=graph(3,{{100,6000}},{{0,100},{100,2},{0,1},{1,2}});
    const auto s=seed(g,{0,1,1});
    const auto plans=dag_chain::tensor_group_candidates(g,2,s);
    bool interleaved=false;
    for(const auto &p:plans) if(p.placement_group[0]==p.placement_group[2] &&
                              p.placement_group[0]!=p.placement_group[1]) {
        require(p.order==vector<int>({0,1,2}),"co-location contracted a group-level cycle into an atomic job");
        validate(g,p,2);interleaved=true;
    }
    require(interleaved,"non-convex A,C group should allow external B in A->B->C");
}
void split_restores_placement_freedom() {
    const auto g=graph(2,{}, {},10000);
    auto s=seed(g,{0,0});s.placement_group={0,0};
    dag_chain::TensorGroupStats stats;
    const auto plans=dag_chain::tensor_group_candidates(g,2,s,nullptr,&stats);
    require(!plans.empty(),"overloaded group produced no split direction");
    bool split=false;
    for(size_t i=0;i<plans.size();++i) if(stats.directions[i]==2 && plans[i].core[0]!=plans[i].core[1]) {
        require(plans[i].placement_group[0]!=plans[i].placement_group[1],"split move retained stale hard co-location metadata");
        require(stats.pipe_bound_delta[i]==-10000 && stats.route_delta[i]==0,"split load/traffic diagnostic incorrect");
        split=true;
    }
    require(split,"split direction did not restore usable core parallelism");
    const auto writers=graph(4,{{100,1024}},{{0,100},{2,100},{100,3}},1000);
    auto ws=seed(writers,{0,0,0,0});ws.placement_group={0,0,0,0};
    const auto trace=dag_chain::official_spill_trace(writers,ws,2);
    const auto moved=dag_chain::tensor_group_candidates(writers,2,ws,&trace,&stats);
    compare_route_deltas(writers,2,ws,moved,stats);
    for(const auto &p:moved) require(p.core[0]==p.core[2],"group splitting broke mandatory joint writers");
    dag_chain::TensorGroupLimits tiny;tiny.move_nodes=1;
    const auto bounded=dag_chain::tensor_group_candidates(writers,2,ws,&trace,&stats,tiny);
    for(const auto &p:bounded) require(p.core[0]==0 && p.core[2]==0,"oversized writer atom was partially moved");
}
void bounds_and_determinism() {
    const auto g=graph(6,{{100,8192},{101,4096}},{{0,100},{100,2},{100,3},{1,101},{101,4},{101,5}},1000);
    auto s=seed(g,{0,0,1,2,1,2});s.placement_group={0,1,2,3,4,5};
    const auto original=solution_json(g,s,3);const auto groups=s.placement_group;
    dag_chain::TensorGroupLimits limits;limits.evaluation_limit=7;limits.per_direction=2;
    dag_chain::TensorGroupStats a,b;
    const auto plans=dag_chain::tensor_group_candidates(g,3,s,nullptr,&a,limits);
    const auto again=dag_chain::tensor_group_candidates(g,3,s,nullptr,&b,limits);
    require(!plans.empty() && plans.size()==again.size() && plans.size()<=3*limits.per_direction,
            "tensor candidate count exceeds bound or is nondeterministic");
    require(a.evaluated_moves<=limits.evaluation_limit && a.evaluated_moves==b.evaluated_moves,
            "evaluation budget not bounded/deterministic");
    require(a.evaluated_by_direction[0] && a.evaluated_by_direction[2],"merge monopolized split evaluation quota");
    for(size_t i=0;i<plans.size();++i)
        require(solution_json(g,plans[i],3)==solution_json(g,again[i],3) &&
                plans[i].placement_group==again[i].placement_group,"candidate ordering is nondeterministic");
    require(solution_json(g,s,3)==original && s.placement_group==groups,"candidate generation mutated the input seed");
    compare_route_deltas(g,3,s,plans,a);
    limits.evaluation_limit=0;
    require(dag_chain::tensor_group_candidates(g,3,s,nullptr,nullptr,limits).empty(),"zero evaluation limit must disable proposals");
}
} // namespace
int main() {
    try {
        fanout_and_shared_input();nonconvex_group_is_not_atomic();split_restores_placement_freedom();bounds_and_determinism();
        cout<<"tensor destination-core marginal COPY, branching groups, non-atomic scheduling, writer-safe splits and bounded deterministic candidates passed\n";
        return 0;
    } catch(const exception &e) {cerr<<e.what()<<'\n';return 1;}
}
