#include "tensor_groups.hpp"

namespace dag_chain {
namespace {
// An inexpensive reference-string pressure hint. Allocate at first local use,
// free after last local use. It measures overlapping lifetimes, NOT the union
// of all tensors touched by a group. Boundary COPYs, spilling and parallel pipe
// lifetimes require the caller's exact replay and timing model.
double live_pressure(const Graph &g, int cores, const vector<int> &assignment,
                     const vector<int> &order, const Config &cfg) {
    const size_t count=g.tensors.size()*size_t(cores);
    vector<int> first(count,g.size()),last(count,-1);
    for (int i=0;i<g.size();++i) {
        const int v=order[i],c=assignment[v];
        for (int t:g.op_tensor_uses[v]) {
            const size_t key=size_t(t)*cores+c;
            first[key]=min(first[key],i);last[key]=i;
        }
    }
    vector<array<double,2>> allocate(g.size()),release(g.size());
    for (size_t t=0;t<g.tensors.size();++t)
        for (int c=0;c<cores;++c) {
            const size_t key=t*cores+c;
            if (last[key]<0) continue;
            const auto &tensor=g.tensors[t];
            allocate[first[key]][tensor.pos]+=double(tensor.size);
            release[last[key]][tensor.pos]+=double(tensor.size);
        }
    vector<array<double,2>> used(cores),peak(cores);
    for (int i=0;i<g.size();++i) {
        int c=assignment[order[i]];
        for (int m=0;m<2;++m) {
            used[c][m]+=allocate[i][m];peak[c][m]=max(peak[c][m],used[c][m]);
            used[c][m]-=release[i][m];
        }
    }
    double pressure=0;
    for (int c=0;c<cores;++c) for (int m=0;m<2;++m)
        pressure+=max(0.0,peak[c][m]-double(cfg.spill_limit(m)))+
                  0.05*peak[c][m]*peak[c][m]/double(cfg.capacity[m]);
    return pressure;
}
vector<int> rebuild_groups(const Graph &g,const Solution &seed,const vector<int> &selected) {
    DSU groups(g.size());
    vector<unsigned char> moved(g.size());
    for (int v:selected) moved[v]=1;
    for (int v=0;v<g.size();++v) groups.join(v,g.writer_group[v]);
    // Keep each old group's residual members together. Selected writer-closed
    // members detach explicitly and form the new same-core cohort. Co-location
    // groups are metadata, never contracted executable graph vertices.
    if (!seed.placement_group.empty()) {
        vector<int> representative(g.size(),-1);
        for (int v=0;v<g.size();++v) if (!moved[v]) {
            int &first=representative[seed.placement_group[v]];
            if (first<0) first=v; else groups.join(first,v);
        }
    }
    for (size_t i=1;i<selected.size();++i) groups.join(selected[0],selected[i]);
    map<int,int> canonical;
    vector<int> result(g.size());
    for (int v=0;v<g.size();++v) {
        int root=groups.get(v);
        if (!canonical.count(root)) canonical[root]=int(canonical.size());
        result[v]=canonical.at(root);
    }
    return result;
}
struct Move { vector<int> nodes;int target=0,direction=0; };
struct Ranked {
    Move move;
    vector<int> assignment,groups;
    double score=0,route_delta=0,pipe_delta=0,pressure_delta=0;
    size_t changed=0;
};
} // namespace

vector<Solution> tensor_group_candidates(const Graph &g,int cores,const Solution &seed,
    const OfficialSpillTrace *trace,TensorGroupStats *statistics,const TensorGroupLimits &limits) {
    TensorGroupStats stats;
    vector<Solution> result;
    auto finish=[&](){if(statistics)*statistics=stats;return result;};
    if (cores<2 || cores>5) throw invalid_argument("tensor grouping requires 2..5 cores");
    validate(g,seed,cores);
    if (!limits.evaluation_limit || !limits.per_direction || !limits.move_nodes) return finish();
    const Config cfg;
    TrafficState ledger(g,cfg,cores,seed.core);
    const double base_route=ledger.bytes,base_pipe=ledger.pipe_bound();
    const auto base_load=ledger.load;
    const double base_pressure=live_pressure(g,cores,seed.core,seed.order,cfg);
    vector<vector<int>> atoms(g.size());
    for (int v=0;v<g.size();++v) atoms[g.writer_group[v]].push_back(v);
    vector<double> spill(g.tensors.size()),hot_atom(g.size());
    if (trace && trace->valid) for (const auto &event:trace->records) {
        if (event.tensor_index>=0 && event.tensor_index<int(spill.size()))
            spill[event.tensor_index]+=double(event.transfer_bytes());
        if (event.trigger_owner>=0 && event.trigger_owner<g.size())
            hot_atom[g.writer_group[event.trigger_owner]]+=double(event.transfer_bytes());
    }
    array<vector<Move>,3> proposals;
    array<set<pair<vector<int>,int>>,3> seen_moves;
    auto add=[&](int direction,vector<int> roots,int target) {
        vector<int> groups,nodes;
        for (int v:roots) groups.push_back(g.writer_group[v]);
        Graph::unique_vec(groups);
        for (int group:groups) {
            if (nodes.size()+atoms[group].size()>limits.move_nodes) {++stats.rejected_size;return;}
            nodes.insert(nodes.end(),atoms[group].begin(),atoms[group].end());
        }
        Graph::unique_vec(nodes);
        bool changed=false;
        for (int v:nodes) changed|=seed.core[v]!=target;
        if (!changed) {++stats.rejected_noop;return;}
        if (!seen_moves[direction].insert({nodes,target}).second) {++stats.rejected_duplicate;return;}
        proposals[direction].push_back({move(nodes),target,direction});
    };
    vector<int> tensors(g.tensors.size());iota(tensors.begin(),tensors.end(),0);
    stable_sort(tensors.begin(),tensors.end(),[&](int a,int b) {
        // Route counts distinct source/destination cores, including graph
        // inputs/outputs. It is not size multiplied by consumer edge count.
        return make_tuple(-(ledger.tensor_bytes(a)+spill[a]),-double(g.tensors[a].size),g.tensors[a].id)<
               make_tuple(-(ledger.tensor_bytes(b)+spill[b]),-double(g.tensors[b].size),g.tensors[b].id);
    });
    for (int t:tensors) {
        if (stats.examined_tensors>=limits.tensor_limit) break;
        if (g.tensor_users[t].size()<2 || g.tensors[t].size<=0) continue;
        ++stats.examined_tensors;
        vector<vector<int>> consumers(cores),producers(cores);
        for (int v:g.tensors[t].consumers) consumers[seed.core[v]].push_back(v);
        for (int v:g.tensors[t].producers) producers[seed.core[v]].push_back(v);
        for (int target=0;target<cores;++target) add(0,g.tensor_users[t],target);
        for (int source=0;source<cores;++source) if (!consumers[source].empty())
            for (int target=0;target<cores;++target) if (target!=source &&
                (!consumers[target].empty() || !producers[target].empty())) {
                auto roots=consumers[source];
                roots.insert(roots.end(),consumers[target].begin(),consumers[target].end());
                roots.insert(roots.end(),producers[target].begin(),producers[target].end());
                add(1,move(roots),target);
            }
    }
    vector<pair<double,int>> split_rank;
    for (int group=0;group<g.size();++group) if (!atoms[group].empty()) {
        double work=0,core_peak=0;
        for (int v:atoms[group]) work+=g.weight(v);
        for (double value:ledger.load[seed.core[atoms[group][0]]]) core_peak=max(core_peak,value);
        split_rank.push_back({-(work*(1+core_peak/max(1.0,base_pipe))+hot_atom[group]/cfg.bandwidth),group});
    }
    stable_sort(split_rank.begin(),split_rank.end());
    for (size_t i=0;i<min(limits.split_atoms,split_rank.size());++i) {
        const auto &nodes=atoms[split_rank[i].second];
        for (int target=0;target<cores;++target) add(2,nodes,target);
    }
    array<vector<Ranked>,3> ranked;
    set<pair<vector<int>,vector<int>>> seen_states;
    // Round-robin work allocation prevents large fanout tensors consuming the
    // entire evaluation budget before split/load-relief proposals are examined.
    for (size_t offset=0;stats.evaluated_moves<limits.evaluation_limit;++offset) {
        bool any=false;
        for (int direction=0;direction<3 && stats.evaluated_moves<limits.evaluation_limit;++direction) {
            if (offset>=proposals[direction].size()) continue;
            any=true;const auto &proposal=proposals[direction][offset];
            for (int v:proposal.nodes) ledger.move_op(v,proposal.target);
            auto assignment=ledger.core;
            auto groups=rebuild_groups(g,seed,proposal.nodes);
            if (seen_states.insert({assignment,groups}).second) {
                ++stats.evaluated_moves;++stats.evaluated_by_direction[direction];
                const double pressure=live_pressure(g,cores,assignment,seed.order,cfg);
                size_t changed=0;for (int v:proposal.nodes) changed+=assignment[v]!=seed.core[v];
                const double score=ledger.light_score(0.03)+0.25*(pressure-base_pressure)/cfg.bandwidth;
                ranked[direction].push_back({proposal,move(assignment),move(groups),score,
                    ledger.bytes-base_route,ledger.pipe_bound()-base_pipe,pressure-base_pressure,changed});
            } else ++stats.rejected_duplicate;
            for (auto it=proposal.nodes.rbegin();it!=proposal.nodes.rend();++it) ledger.move_op(*it,seed.core[*it]);
            // Counts/core ownership are restored exactly; reset floating totals
            // too so large integer inputs cannot accumulate roundoff by undo.
            ledger.bytes=base_route;ledger.load=base_load;
        }
        if (!any) break;
    }
    for (auto &bucket:ranked) stable_sort(bucket.begin(),bucket.end(),[](const Ranked &a,const Ranked &b) {
        return make_tuple(a.score,a.route_delta,a.pipe_delta,a.assignment,a.groups)<
               make_tuple(b.score,b.route_delta,b.pipe_delta,b.assignment,b.groups);
    });
    for (size_t rank=0;rank<limits.per_direction;++rank)
        for (int direction=0;direction<3;++direction) if (rank<ranked[direction].size()) {
            auto &entry=ranked[direction][rank];
            SeedOptions options;options.mode=direction==2 ? "dfs" : "critical";
            options.window=8;options.memory=1.0;
            auto candidate=operation_plan(g,cores,cfg,options,&entry.assignment,nullptr);
            candidate.placement_group=move(entry.groups);
            candidate.label="tensor_group_"+to_string(direction)+"_"+to_string(rank);
            validate(g,candidate,cores);
            ++stats.generated_by_direction[direction];
            stats.route_delta.push_back(entry.route_delta);stats.pipe_bound_delta.push_back(entry.pipe_delta);
            stats.pressure_delta.push_back(entry.pressure_delta);stats.moved_nodes.push_back(entry.changed);
            stats.directions.push_back(direction);result.push_back(move(candidate));
        }
    return finish();
}
} // namespace dag_chain
