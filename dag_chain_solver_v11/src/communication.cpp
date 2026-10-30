#include "communication.hpp"
#include "search_limits.hpp"
#include <optional>

namespace dag_chain {
namespace {
using Bytes = unsigned long long;
Bytes total(const OfficialSpillTrace &t) { return Bytes(t.route_bytes) + Bytes(t.spill_bytes); }
struct Link { int from, to; double cost; };

vector<Link> communication_links(const Graph &g) {
    map<pair<int, int>, double> bytes;
    for (const auto &tensor : g.tensors)
        for (int producer : tensor.producers)
            for (int consumer : tensor.consumers)
                if (producer != consumer) bytes[{producer, consumer}] += double(tensor.size);
    for (const auto &edge : g.direct) bytes[{edge.a, edge.b}] += double(edge.size);
    const Config config;
    vector<Link> links;
    for (int v : g.topo)
        for (int u : g.ops[v].pred) {
            const auto it = bytes.find({u,v});
            const double size = it == bytes.end() ? 0 : it->second;
            // Local affinity hint only. Shared destination-core copies cannot
            // be valued independently per edge; full trace checks final bytes.
            links.push_back({u,v,config.delay + 2.0 * size / config.bandwidth});
        }
    stable_sort(links.begin(), links.end(), [&](const Link &a, const Link &b) {
        return make_tuple(-a.cost,g.ops[a.from].id,g.ops[a.to].id) <
               make_tuple(-b.cost,g.ops[b.from].id,g.ops[b.to].id);
    });
    return links;
}

vector<int> stream_key(const Solution &s, int cores) {
    vector<vector<int>> streams(cores);
    for (int v : s.order) streams[s.core[v]].push_back(v);
    vector<int> key;
    key.reserve(s.order.size()+size_t(cores));
    for (const auto &row : streams) { key.insert(key.end(),row.begin(),row.end());key.push_back(-1); }
    return key;
}
} // namespace

static Decomposition communication_cover(const Graph &g, vector<double> &incoming_cost) {
    const int n=g.size();
    auto links = communication_links(g);
    Decomposition d;
    d.parent.assign(n,-1); d.next.assign(n,-1);
    d.earliest_finish.assign(n,0); // Intentionally unused by this placement policy.
    incoming_cost.assign(n,0);
    // Every edge competes using the same communication-only priority. One
    // incoming/outgoing retained edge per node; the original DAG prevents cycles.
    // This is a maximal greedy cover, not an optimal weighted path cover claim.
    for (const auto &edge : links)
        if (d.next[edge.from] < 0 && d.parent[edge.to] < 0) {
            d.next[edge.from] = edge.to; d.parent[edge.to] = edge.from;
            incoming_cost[edge.to] = edge.cost;
        }
    for (int root : g.topo) if (d.parent[root] < 0) {
        vector<int> path;
        for (int v=root;v>=0;v=d.next[v]) path.push_back(v);
        d.paths.push_back(move(path));
    }
    return d;
}

static Decomposition communication_segment(const Graph &g, int cores, double scale,
                                            const Decomposition &cover,
                                            const vector<double> &incoming_cost,
                                            bool footprint_guard) {
    if (cores < 2 || cores > 5 || !isfinite(scale) || scale <= 0)
        throw invalid_argument("invalid communication decomposition parameters");
    const int n=g.size();
    const Config config;
    Decomposition d=cover;
    Row work_total{}, largest{}, limit{};
    for (int v : g.topo) {
        const int p=g.ops[v].pipe;
        work_total[p]+=g.weight(v);largest[p]=max(largest[p],g.weight(v));
    }
    for (size_t p=0;p<limit.size();++p)
        limit[p]=max(largest[p],work_total[p]/cores*scale);
    vector<Link> retain;
    vector<int> seen_tensor(g.tensors.size(),-1);
    int stamp=0;
    for (const auto &path : d.paths) {
        const size_t count=path.size();
        vector<double> dp(count+1,INF);
        vector<size_t> segments(count+1,numeric_limits<size_t>::max()),previous(count+1);
        dp[0]=0;segments[0]=0;
        for (size_t end=1;end<=count;++end) {
            Row work{};array<double,2> footprint{};
            // One stamp per endpoint, at most n endpoints across all paths.
            ++stamp;
            const size_t window=min(search_limits::communication_group_nodes,
                                    search_limits::communication_cut_window);
            const size_t first=end>window ? end-window : 0;
            for (size_t begin=end;begin>first;) {
                --begin;int v=path[begin];work[g.ops[v].pipe]+=g.weight(v);
                for (int t : g.op_tensor_uses[v]) if (seen_tensor[t]!=stamp) {
                    seen_tensor[t]=stamp;footprint[g.tensors[t].pos]+=double(g.tensors[t].size);
                }
                bool fits=true;
                for(size_t p=0;p<work.size();++p) if(work[p]>limit[p])fits=false;
                // Co-location is not simultaneous residency. Keep this union
                // bound only in the conservative variant; exact replay and
                // refinement still check the unguarded variant's actual order.
                if(footprint_guard)
                    for(int p=0;p<2;++p) if(footprint[p]>double(config.spill_limit(p)))fits=false;
                // Individual operations are not rejected because of a search
                // heuristic. Actual memory feasibility is checked by the models.
                if(end-begin>1 && !fits)break;
                const double cut=begin ? incoming_cost[path[begin]] : 0;
                if(make_pair(dp[begin]+cut,segments[begin]+1)<make_pair(dp[end],segments[end])) {
                    dp[end]=dp[begin]+cut;segments[end]=segments[begin]+1;previous[end]=begin;
                }
            }
        }
        for(size_t end=count;end>0;) {
            const size_t begin=previous[end];
            for(size_t i=begin+1;i<end;++i)retain.push_back({path[i-1],path[i],incoming_cost[path[i]]});
            end=begin;
        }
    }
    // Writer atoms may span paths. Join them first, then retain DP-selected
    // edges in cost order only while the combined group fits the same caps.
    // Mandatory writer atoms may themselves exceed heuristic caps; never split
    // them or reject an input solely for this reason. No quotient DAG is run.
    DSU groups(n);
    for(int v=0;v<n;++v)groups.join(v,g.writer_group[v]);
    vector<Row> group_work(n);
    vector<array<double,2>> group_bytes(n);
    vector<set<int>> tensors(n);
    for(int v=0;v<n;++v) {
        int root=groups.get(v);group_work[root][g.ops[v].pipe]+=g.weight(v);
        for(int t:g.op_tensor_uses[v])if(tensors[root].insert(t).second)
            group_bytes[root][g.tensors[t].pos]+=double(g.tensors[t].size);
    }
    stable_sort(retain.begin(),retain.end(),[](const Link&a,const Link&b){
        return make_tuple(-a.cost,a.from,a.to)<make_tuple(-b.cost,b.from,b.to);
    });
    for(const auto &edge:retain) {
        int a=groups.get(edge.from),b=groups.get(edge.to);if(a==b)continue;
        if(size_t(groups.sz[a])+size_t(groups.sz[b])>search_limits::communication_group_nodes)continue;
        Row joined{};bool fits=true;
        for(size_t p=0;p<joined.size();++p) {
            joined[p]=group_work[a][p]+group_work[b][p];if(joined[p]>limit[p])fits=false;
        }
        if(!fits)continue;
        if(tensors[a].size()<tensors[b].size())swap(a,b);
        auto bytes=group_bytes[a];
        for(int t:tensors[b])if(!tensors[a].count(t))bytes[g.tensors[t].pos]+=double(g.tensors[t].size);
        if(footprint_guard)
            for(int p=0;p<2;++p)if(bytes[p]>double(config.spill_limit(p)))fits=false;
        if(!fits)continue;
        // Choose the larger tensor set as the root for bounded small-to-large merging.
        groups.p[b]=a;groups.sz[a]+=groups.sz[b];group_work[a]=joined;group_bytes[a]=bytes;
        tensors[a].insert(tensors[b].begin(),tensors[b].end());tensors[b].clear();
    }
    d.bundle_of.resize(n);
    map<int,int> index;
    for(int v:g.topo) {
        int root=groups.get(v);
        if(!index.count(root)){index[root]=int(d.bundles.size());d.bundles.push_back({});}
        int b=index[root];d.bundle_of[v]=b;d.bundles[b].push_back(v);
    }
    return d;
}

Decomposition communication_decompose(const Graph &g, int cores, double scale,
                                      bool footprint_guard) {
    vector<double> incoming_cost;
    auto cover=communication_cover(g,incoming_cost);
    return communication_segment(g,cores,scale,cover,incoming_cost,footprint_guard);
}

vector<Solution> communication_candidates(const Graph &g, int cores, CommunicationStats *statistics,
                                          Deadline *deadline) {
    CommunicationStats discarded;auto &stats=statistics ? *statistics : discarded;
    vector<Solution> result;
    if(!search_limits::communication_enabled)return result;
    auto check=[&]() {if(deadline)deadline->check();};
    check();
    const array<double,2> scales{0.5,1.0};
    // Cover affinity depends only on the graph, not the core budget or cut scale.
    vector<double> incoming_cost;
    const auto cover=communication_cover(g,incoming_cost);
    check();
    set<vector<int>> seen;
    for(int k=2;k<=cores;++k)
        for(size_t variant=0;variant<search_limits::communication_variant_limit;++variant) {
            check();
            // Same bounded portfolio size: small conservative groups, then
            // communication-led groups without a union-residency assumption.
            auto d=communication_segment(g,k,scales[variant],cover,incoming_cost,variant==0);
            check();
            auto assignment=pack_decomposition(g,d,k,variant ? 4.0 : 2.0);
            check();
            for(const string &mode:{string("critical"),string("dfs")}) {
                SeedOptions opt;opt.mode=mode;opt.window=8;opt.memory=0.5;
                auto candidate=operation_plan(g,k,Config{},opt,&assignment,deadline);
                candidate.placement_group=d.bundle_of;
                candidate.label="communication_k"+to_string(k)+"_v"+to_string(variant)+"_"+mode;
                validate(g,candidate,cores);++stats.generated;
                if(seen.insert(stream_key(candidate,cores)).second)result.push_back(move(candidate));
            }
        }
    stats.unique_candidates+=result.size();
    return result;
}

Result improve_communication_groups(const Graph &g,int cores,Result incumbent,CommunicationStats *statistics,
                                    Deadline *deadline) {
    CommunicationStats stats;
    stats.before_cycles=stats.after_cycles=incumbent.refined.cycles;
    stats.selected_label=incumbent.solution.label;
    auto finish=[&]() {
        incumbent.candidates+=stats.generated;
        incumbent.refined_candidates+=stats.refined_candidates;
        if(stats.feedback_runs) {
            incumbent.candidates+=stats.feedback.traced_candidates;
            incumbent.refined_candidates+=stats.feedback.refined_candidates;
        }
        if(stats.selected_new) {
            map<int,size_t> sizes;
            for(int group:incumbent.solution.placement_group)++sizes[group];
            stats.selected_groups=sizes.size();
            for(const auto &item:sizes)stats.selected_max_group_nodes=max(stats.selected_max_group_nodes,item.second);
        }
        if(statistics)*statistics=stats;
        return incumbent;
    };
    if(!search_limits::communication_enabled)return finish();
    auto check=[&]() {if(deadline)deadline->check();};
    try {
    check();
    validate(g,incumbent.solution,cores);
    auto baseline=official_spill_trace(g,incumbent.solution,cores);
    stats.baseline_trace_valid=baseline.valid;
    if(!baseline.valid)return finish();
    stats.before_route_bytes=stats.after_route_bytes=baseline.route_bytes;
    stats.before_spill_bytes=stats.after_spill_bytes=baseline.spill_bytes;
    check();
    const Bytes byte_limit=total(baseline);const double cycle_limit=incumbent.refined.cycles;
    Bytes best_bytes=byte_limit;
    struct Candidate {Solution solution;Metrics proxy;};
    vector<Candidate> pool;
    const auto incumbent_key=stream_key(incumbent.solution,cores);
    for(auto &s:communication_candidates(g,cores,&stats,deadline)) {
        check();
        if(stream_key(s,cores)==incumbent_key)continue;
        auto proxy=evaluate_proxy(g,cores,Config{},s,0.03,deadline);
        pool.push_back({move(s),move(proxy)});
    }
    vector<size_t> by_time(pool.size()),by_traffic(pool.size());
    iota(by_time.begin(),by_time.end(),size_t(0));by_traffic=by_time;
    stable_sort(by_time.begin(),by_time.end(),[&](size_t a,size_t b){
        return make_tuple(pool[a].proxy.predicted,pool[a].proxy.route+pool[a].proxy.spill,a)<
               make_tuple(pool[b].proxy.predicted,pool[b].proxy.route+pool[b].proxy.spill,b);
    });
    stable_sort(by_traffic.begin(),by_traffic.end(),[&](size_t a,size_t b){
        return make_tuple(pool[a].proxy.route+pool[a].proxy.spill,pool[a].proxy.predicted,a)<
               make_tuple(pool[b].proxy.route+pool[b].proxy.spill,pool[b].proxy.predicted,b);
    });
    vector<size_t> shortlist;vector<unsigned char> chosen(pool.size());
    for(size_t offset=0;offset<pool.size() && shortlist.size()<search_limits::communication_refine_candidates;++offset)
        for(size_t i:{by_time[offset],by_traffic[offset]})
            if(!chosen[i] && shortlist.size()<search_limits::communication_refine_candidates) {
                chosen[i]=1;shortlist.push_back(i);
            }
    optional<Result> feedback_seed;
    Bytes feedback_bytes=0;
    auto offer=[&](const Result &candidate,const OfficialSpillTrace &trace) {
        if(total(trace)>byte_limit){++stats.rejected_traffic;return;}
        if(candidate.refined.cycles>cycle_limit){++stats.rejected_cycles;return;}
        if(make_pair(candidate.refined.cycles,total(trace))<make_pair(incumbent.refined.cycles,best_bytes)) {
            incumbent.solution=candidate.solution;incumbent.estimate=candidate.estimate;incumbent.refined=candidate.refined;
            incumbent.generated_for_cores=candidate.generated_for_cores;
            best_bytes=total(trace);stats.selected_new=true;stats.selected_label=incumbent.solution.label;
            stats.after_cycles=incumbent.refined.cycles;
            stats.after_route_bytes=trace.route_bytes;stats.after_spill_bytes=trace.spill_bytes;
        }
    };
    for(size_t i:shortlist) {
        check();
        auto trace=official_spill_trace(g,pool[i].solution,cores);++stats.traced_candidates;
        check();
        if(!trace.valid){++stats.rejected_trace;continue;}
        // A new partition may initially spill more because of its order. Let
        // the fixed shortlist reach refinement/one feedback run before the
        // final traffic guard; exploratory states never replace the incumbent.
        if(total(trace)>byte_limit)++stats.oversized_traffic_explored;
        auto refined=refine_estimate(g,pool[i].solution,cores,deadline);++stats.refined_candidates;
        if(!refined.valid){++stats.rejected_refine;continue;}
        Result result;result.solution=move(pool[i].solution);result.estimate=move(pool[i].proxy);
        result.refined=move(refined);
        result.generated_for_cores=result.solution.core.empty()?cores:
            *max_element(result.solution.core.begin(),result.solution.core.end())+1;
        offer(result,trace);
        // A currently slower candidate can still be a useful bounded feedback
        // start. Final acceptance is always guarded against the original entry.
        if(!feedback_seed || make_pair(result.refined.cycles,total(trace))<make_pair(feedback_seed->refined.cycles,feedback_bytes)) {
            feedback_seed=move(result);feedback_bytes=total(trace);
        }
        check();
    }
    if(search_limits::communication_post_feedback && feedback_seed) {
        check();
        ++stats.feedback_runs;
        auto refined=improve_spill_order(g,cores,move(*feedback_seed),&stats.feedback,nullptr,nullptr,deadline);
        if(deadline && deadline->expired() && stats.feedback.initial_trace_valid) {
            // Nested feedback already completed the trace for its accepted
            // winner. Reuse its exact byte summary when no replay time remains.
            OfficialSpillTrace completed;completed.valid=true;
            completed.route_bytes=stats.feedback.after_route_bytes;
            completed.spill_bytes=stats.feedback.after_spill_bytes;
            offer(refined,completed);
        }
        check();
        auto trace=official_spill_trace(g,refined.solution,cores);++stats.traced_candidates;
        if(trace.valid)offer(refined,trace);else ++stats.rejected_trace;
        check();
    }
    validate(g,incumbent.solution,cores);
    } catch(const SearchExpired &) {
        // A partially generated/refined candidate never replaces this owned
        // complete incumbent. Nested feedback shares the same absolute limit.
        stats.deadline_reached=true;
    }
    return finish();
}

string communication_stats_json(const CommunicationStats &s) {
    ostringstream out;out<<setprecision(17)<<'{';bool first=true;
    auto field=[&](const char *key,auto value){if(!first)out<<',';first=false;out<<'"'<<key<<"\":"<<value;};
    auto cycles=[&](const char *key,double value){if(isfinite(value))field(key,value);else field(key,"null");};
    field("generated",s.generated);field("unique_candidates",s.unique_candidates);
    field("refined_candidates",s.refined_candidates);field("traced_candidates",s.traced_candidates);
    field("rejected_refine",s.rejected_refine);field("rejected_trace",s.rejected_trace);
    field("rejected_traffic",s.rejected_traffic);field("rejected_cycles",s.rejected_cycles);
    field("oversized_traffic_explored",s.oversized_traffic_explored);
    field("feedback_runs",s.feedback_runs);field("selected_groups",s.selected_groups);
    field("selected_max_group_nodes",s.selected_max_group_nodes);
    field("baseline_trace_valid",s.baseline_trace_valid?"true":"false");field("selected_new",s.selected_new?"true":"false");
    field("deadline_reached",s.deadline_reached?"true":"false");
    cycles("before_cycles",s.before_cycles);cycles("after_cycles",s.after_cycles);
    field("before_route_bytes",s.before_route_bytes);field("before_spill_bytes",s.before_spill_bytes);
    field("after_route_bytes",s.after_route_bytes);field("after_spill_bytes",s.after_spill_bytes);
    out<<",\"selected_label\":\""<<s.selected_label<<"\",\"feedback\":"<<feedback_stats_json(s.feedback)<<'}';
    return out.str();
}
} // namespace dag_chain
