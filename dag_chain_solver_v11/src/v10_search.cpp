#include "v10_search.hpp"
#include "tensor_groups.hpp"
#include "official_timing.hpp"
#include "timing_feedback.hpp"
#include "search_limits.hpp"
#include <optional>

namespace dag_chain {
namespace {
using Bytes = unsigned long long;
string json_string(const string &value) {
    ostringstream out;out<<'"';
    for(unsigned char c:value) {
        if(c=='"' || c=='\\')out<<'\\'<<char(c);
        else if(c<32)out<<"\\u"<<hex<<setw(4)<<setfill('0')<<int(c)<<dec;
        else out<<char(c);
    }
    out<<'"';return out.str();
}
Bytes total(const OfficialSpillTrace &s) { return Bytes(s.route_bytes)+Bytes(s.spill_bytes); }
vector<int> key(const Solution &s, int cores) {
    vector<vector<int>> streams(cores);
    for (int v:s.order) streams[s.core[v]].push_back(v);
    vector<int> result;
    for (const auto &stream:streams) {result.insert(result.end(),stream.begin(),stream.end());result.push_back(-1);}
    return result;
}
vector<int> state_key(const Graph &g,const Solution &s,int cores) {
    auto result=key(s,cores);result.push_back(-2);
    DSU groups(g.size());
    for(int v=0;v<g.size();++v)groups.join(v,g.writer_group[v]);
    map<int,int> first;
    if(!s.placement_group.empty())for(int v=0;v<g.size();++v) {
        auto [it,inserted]=first.emplace(s.placement_group[v],v);
        if(!inserted)groups.join(v,it->second);
    }
    map<int,int> canonical;
    for(int v=0;v<g.size();++v) {
        const int root=groups.get(v);
        if(!canonical.count(root))canonical[root]=int(canonical.size());
        result.push_back(canonical.at(root));
    }
    return result;
}
double elapsed(Clock::time_point t) {return chrono::duration<double>(Clock::now()-t).count();}
struct Entry {Result result; OfficialSpillTrace trace;};
// Alternate low-time, low-spill and low-boundary-traffic starts. Each list uses
// deterministic full-stream dedup; these are search priorities, not exact scores.
vector<size_t> diverse(const vector<Entry> &pool, size_t limit, bool include_baseline) {
    array<vector<size_t>,3> orders;
    for (auto &order:orders) for(size_t i=include_baseline?0:1;i<pool.size();++i)order.push_back(i);
    for(size_t mode=0;mode<orders.size();++mode)
        stable_sort(orders[mode].begin(),orders[mode].end(),[&](size_t a,size_t b){
            const auto &x=pool[a], &y=pool[b];
            auto rank=[&](const Entry &e,size_t i) {
                const double first=mode==0?e.result.refined.cycles:
                    mode==1?double(e.trace.spill_bytes):double(e.trace.route_bytes);
                return make_tuple(first,e.result.refined.cycles,total(e.trace),i);
            };
            return rank(x,a)<rank(y,b);
        });
    vector<size_t> answer; vector<unsigned char> used(pool.size());
    for(size_t r=0;r<pool.size() && answer.size()<limit;++r)
        for(const auto &order:orders) if(r<order.size() && !used[order[r]] && answer.size()<limit) {
            used[order[r]]=1;answer.push_back(order[r]);
        }
    return answer;
}
} // namespace

Result improve_v10(const Graph &g,int cores,Result incumbent,V10Stage stage,V10Stats *statistics,
                   Deadline *deadline) {
    V10Stats stats;
    stats.stage=stage==V10Stage::v9?"v9":stage==V10Stage::tensor_memory?"tensor_memory":
        stage==V10Stage::multi_feedback?"multi_feedback":stage==V10Stage::exact_legacy?"exact_legacy":
        stage==V10Stage::exact_pool?"exact_pool":"exact";
    stats.before_estimated_cycles=stats.after_estimated_cycles=incumbent.refined.cycles;
    stats.selected_label=incumbent.solution.label;
    const auto incumbent_key=key(incumbent.solution,cores);
    const size_t previous_candidates=incumbent.candidates, previous_refined=incumbent.refined_candidates;
    auto finish=[&]() {
        stats.selected_new=key(incumbent.solution,cores)!=incumbent_key;
        stats.selected_label=incumbent.solution.label;
        incumbent.candidates=previous_candidates+stats.generated+stats.feedback_replays+stats.tail_generated;
        incumbent.refined_candidates=previous_refined+stats.refined+stats.feedback_refinements;
        if(statistics)*statistics=stats;
        validate(g,incumbent.solution,cores);
        return incumbent;
    };
    if(stage==V10Stage::v9)return finish();
    auto started=Clock::now();
    double *active_seconds=&stats.candidate_seconds;
    optional<Clock::time_point> tail_started;
    auto check=[&]() {if(deadline)deadline->check();};
    try {
    check();
    auto baseline=official_spill_trace(g,incumbent.solution,cores);
    if(!baseline.valid){stats.error=baseline.error;return finish();}
    stats.before_total_bytes=stats.after_total_bytes=total(baseline);
    check();
    vector<Entry> pool;
    pool.push_back({incumbent,baseline});
    auto checkpoint_conservative=[&](const Entry &entry) {
        // Before exact selection, retain only a complete candidate satisfying
        // the original estimate/traffic guard. No-deadline ordering is unchanged.
        if(deadline && total(entry.trace)<=total(pool[0].trace) &&
           entry.result.refined.cycles<=pool[0].result.refined.cycles &&
           make_pair(entry.result.refined.cycles,total(entry.trace))<
           make_pair(incumbent.refined.cycles,stats.after_total_bytes)) {
            incumbent=entry.result;stats.after_total_bytes=total(entry.trace);
            stats.after_estimated_cycles=incumbent.refined.cycles;
        }
    };
    set<vector<int>> evaluated{state_key(g,incumbent.solution,cores)};
    TensorGroupLimits limits;
    limits.tensor_limit=search_limits::v10_tensor_limit;
    limits.evaluation_limit=search_limits::v10_group_evaluations;
    limits.per_direction=search_limits::v10_group_keep_per_direction;
    limits.move_nodes=search_limits::v10_group_move_nodes;
    limits.split_atoms=search_limits::v10_split_atoms;
    auto candidates=tensor_group_candidates(g,cores,incumbent.solution,&baseline,nullptr,limits);
    check();
    auto orders=spill_feedback_candidates(g,cores,incumbent.solution,baseline);
    check();
    for(size_t i=0;i<min(orders.size(),search_limits::v10_order_candidates);++i)
        candidates.push_back(move(orders[i*orders.size()/min(orders.size(),search_limits::v10_order_candidates)]));
    stats.generated=candidates.size();
    struct Cheap {Solution s;Metrics proxy;};
    vector<Cheap> cheap;
    set<vector<int>> seen{state_key(g,incumbent.solution,cores)};
    for(auto &s:candidates) if(seen.insert(state_key(g,s,cores)).second) {
        check();
        validate(g,s,cores);
        auto proxy=evaluate_proxy(g,cores,Config{},s,0.03,deadline);
        cheap.push_back({move(s),move(proxy)});
    }
    stats.unique=cheap.size();
    array<vector<size_t>,3> rankings;
    for(auto &r:rankings){r.resize(cheap.size());iota(r.begin(),r.end(),size_t(0));}
    for(size_t mode=0;mode<rankings.size();++mode)
        stable_sort(rankings[mode].begin(),rankings[mode].end(),[&](size_t a,size_t b){
            auto rank=[&](size_t i){const auto &p=cheap[i].proxy;
                return make_tuple(mode==0?p.predicted:mode==1?p.spill:p.route,p.predicted,p.route+p.spill,i);};
            return rank(a)<rank(b);
        });
    vector<size_t> selected;vector<unsigned char> chosen(cheap.size());
    for(size_t r=0;r<cheap.size() && selected.size()<search_limits::v10_refine_candidates;++r)
        for(const auto &ranking:rankings)if(!chosen[ranking[r]] && selected.size()<search_limits::v10_refine_candidates){
            chosen[ranking[r]]=1;selected.push_back(ranking[r]);
        }
    for(size_t i:selected) {
        check();
        auto trace=official_spill_trace(g,cheap[i].s,cores);++stats.traced;
        check();
        if(!trace.valid)continue;
        auto refined=refine_estimate(g,cheap[i].s,cores,deadline);++stats.refined;
        if(!refined.valid)continue;
        Result result;result.solution=move(cheap[i].s);result.estimate=move(cheap[i].proxy);result.refined=move(refined);
        result.generated_for_cores=cores;
        evaluated.insert(state_key(g,result.solution,cores));
        pool.push_back({move(result),move(trace)});
        checkpoint_conservative(pool.back());
        check();
    }
    stats.candidate_seconds=elapsed(started);
    active_seconds=nullptr;
    if(stage>=V10Stage::multi_feedback) {
        started=Clock::now();
        active_seconds=&stats.feedback_seconds;
        const auto starts=diverse(pool,search_limits::v10_feedback_starts,false);
        size_t remaining_replays=search_limits::v10_feedback_replays;
        size_t remaining_refinements=search_limits::v10_feedback_refinements;
        for(size_t j=0;j<starts.size();++j) {
            check();
            const size_t remaining_starts=starts.size()-j;
            FeedbackBudget budget{remaining_replays/remaining_starts,remaining_refinements/remaining_starts,2};
            if(!budget.replays || !budget.refinements)break;
            FeedbackStats feedback;
            auto result=improve_spill_order(g,cores,pool[starts[j]].result,&feedback,nullptr,&budget,deadline);
            remaining_replays-=feedback.traced_candidates;remaining_refinements-=feedback.refined_candidates;
            ++stats.feedback_starts;stats.feedback_replays+=feedback.traced_candidates;
            stats.feedback_refinements+=feedback.refined_candidates;stats.feedback.push_back(move(feedback));
            if(deadline && deadline->expired() && stats.feedback.back().initial_trace_valid) {
                OfficialSpillTrace completed;completed.valid=true;
                completed.route_bytes=stats.feedback.back().after_route_bytes;
                completed.spill_bytes=stats.feedback.back().after_spill_bytes;
                checkpoint_conservative(Entry{result,move(completed)});
            }
            check();
            if(evaluated.insert(state_key(g,result.solution,cores)).second) {
                auto trace=official_spill_trace(g,result.solution,cores);++stats.traced;
                if(trace.valid) {
                    pool.push_back({move(result),move(trace)});
                    checkpoint_conservative(pool.back());
                }
                check();
            }
        }
        stats.feedback_seconds=elapsed(started);
        active_seconds=nullptr;
    }
    size_t conservative=0;
    for(size_t i=1;i<pool.size();++i)
        if(total(pool[i].trace)<=total(pool[0].trace) && pool[i].result.refined.cycles<=pool[0].result.refined.cycles &&
           make_pair(pool[i].result.refined.cycles,total(pool[i].trace))<
           make_pair(pool[conservative].result.refined.cycles,total(pool[conservative].trace))) conservative=i;
    size_t winner=conservative;
    optional<Result> tail_winner;
    if(stage>=V10Stage::exact_legacy) {
        started=Clock::now();
        active_seconds=&stats.exact_seconds;
        check();
        // Always compare to the freshly generated v9 answer. This final gate
        // minimizes official cycles; traffic is a tie-breaker, not legality.
        vector<size_t> finalists{0};
        set<vector<int>> final_streams{key(pool[0].result.solution,cores)};
        if(conservative && finalists.size()<search_limits::v10_exact_candidates &&
           final_streams.insert(key(pool[conservative].result.solution,cores)).second)finalists.push_back(conservative);
        for(size_t i:diverse(pool,pool.size(),false)) {
            if(finalists.size()>=search_limits::v10_exact_candidates)break;
            if(final_streams.insert(key(pool[i].result.solution,cores)).second)finalists.push_back(i);
        }
        // Preserve the complete old shortlist (including tie order), not just
        // its approximate winner. Every new stage compares to its exact winner.
        const size_t legacy_size=finalists.size();
        if(stage>=V10Stage::exact_pool)
            for(size_t i:diverse(pool,pool.size(),false)) {
                if(finalists.size()>=search_limits::v10_exact_pool_candidates)break;
                if(final_streams.insert(key(pool[i].result.solution,cores)).second)finalists.push_back(i);
            }
        auto note=[&](const string &phase,const Solution &s,const OfficialTimingResult &t,bool improved) {
            stats.exact_candidates.push_back({phase,s.label,t.error,t.valid?t.cycles:-1,
                                              t.route_bytes,t.spill_bytes,improved});
            if(!t.valid){++stats.exact_invalid;stats.exact_errors.push_back(s.label+": "+t.error);}
        };
        check();
        auto original=official_timing(g,pool[0].result.solution,cores);++stats.exact_evaluations;
        note("legacy",pool[0].result.solution,original,false);
        stats.exact_baseline_valid=original.valid;
        winner=0;
        if(original.valid) {
            stats.before_exact_cycles=stats.after_exact_cycles=original.cycles;
            stats.legacy_exact_cycles=original.cycles;
            if(deadline) {
                incumbent=pool[0].result;
                stats.after_total_bytes=total(pool[0].trace);
                stats.after_estimated_cycles=incumbent.refined.cycles;
            }
            auto best=make_pair(original.cycles,total(pool[0].trace));
            auto best_timing=move(original);
            check();
            for(size_t j=1;j<finalists.size();++j) {
                check();
                const size_t i=finalists[j];
                auto measured=official_timing(g,pool[i].result.solution,cores);++stats.exact_evaluations;
                const bool improved=measured.valid && make_pair(measured.cycles,total(pool[i].trace))<best;
                note(j<legacy_size?"legacy":"pool",pool[i].result.solution,measured,improved);
                if(improved) {
                    best={measured.cycles,total(pool[i].trace)};winner=i;
                    stats.after_exact_cycles=measured.cycles;
                    best_timing=move(measured);
                    if(deadline) {
                        incumbent=pool[i].result;stats.after_total_bytes=best.second;
                        stats.after_estimated_cycles=incumbent.refined.cycles;
                    }
                }
                if(j<legacy_size)stats.legacy_exact_cycles=best.first;
                check();
            }
            stats.pool_exact_cycles=best.first;
            if(stage==V10Stage::exact) {
                tail_started=Clock::now();
                check();
                // Both neighborhoods use the SAME exact-scored seed. No
                // approximate-cycle or traffic-only rejection is applied here.
                auto timeline_moves=timing_feedback_candidates(g,cores,pool[winner].result.solution,
                    best_timing,search_limits::v10_timing_order_candidates);
                check();
                auto spill_moves=spill_feedback_candidates(g,cores,pool[winner].result.solution,pool[winner].trace);
                check();
                if(spill_moves.size()>search_limits::v10_exact_spill_candidates)
                    spill_moves.resize(search_limits::v10_exact_spill_candidates);
                stats.tail_generated=timeline_moves.size()+spill_moves.size();
                const size_t rounds=max(timeline_moves.size(),spill_moves.size());
                for(size_t j=0;j<rounds;++j)for(size_t kind=0;kind<2;++kind) {
                    check();
                    auto &moves=kind==0?timeline_moves:spill_moves;
                    if(j>=moves.size())continue;
                    auto &s=moves[j];
                    if(!final_streams.insert(key(s,cores)).second){++stats.tail_duplicates;continue;}
                    validate(g,s,cores);
                    auto measured=official_timing(g,s,cores);++stats.exact_evaluations;++stats.tail_evaluations;
                    const Bytes bytes=Bytes(measured.route_bytes)+Bytes(measured.spill_bytes);
                    const bool improved=measured.valid && make_pair(measured.cycles,bytes)<best;
                    note(kind==0?"timeline":"spill_exact",s,measured,improved);
                    if(improved) {
                        best={measured.cycles,bytes};++stats.tail_accepted;
                        if(!tail_winner)tail_winner=pool[winner].result;
                        tail_winner->solution=move(s);
                        if(deadline) {
                            incumbent=*tail_winner;stats.after_exact_cycles=best.first;
                            stats.after_total_bytes=best.second;stats.diagnostics_current=false;
                        }
                    }
                    check();
                }
                if(tail_winner) {
                    // Keep the public Result diagnostics about the selected
                    // output, but this estimate never vetoes an exact gain.
                    check();
                    tail_winner->estimate=evaluate_proxy(g,cores,Config{},tail_winner->solution,0.03,deadline);
                    tail_winner->refined=refine_estimate(g,tail_winner->solution,cores,deadline);++stats.refined;
                    stats.diagnostics_current=true;
                }
                stats.after_exact_cycles=best.first;
                stats.after_total_bytes=best.second;
                stats.tail_seconds=elapsed(*tail_started);
                tail_started.reset();
            }
        } else stats.error=original.error;
        stats.exact_seconds=elapsed(started);
        active_seconds=nullptr;
    }
    if(tail_winner)incumbent=move(*tail_winner);
    else {incumbent=move(pool[winner].result);stats.after_total_bytes=total(pool[winner].trace);}
    stats.after_estimated_cycles=incumbent.refined.cycles;
    } catch(const SearchExpired &) {
        stats.deadline_reached=true;
        if(active_seconds)*active_seconds=elapsed(started);
        if(tail_started)stats.tail_seconds=elapsed(*tail_started);
        // Only completed candidates are checkpointed. A tail exact winner may
        // outlive an interrupted diagnostics refresh; its exact fields remain
        // authoritative and diagnostics_current explicitly marks stale estimates.
        stats.after_estimated_cycles=stats.diagnostics_current?incumbent.refined.cycles:INF;
    }
    return finish();
}

string v10_stats_json(const V10Stats &s) {
    ostringstream o;o<<setprecision(17)<<'{';bool first=true;
    auto f=[&](const char *name,auto value){if(!first)o<<',';first=false;o<<'"'<<name<<"\":"<<value;};
    auto finite=[&](const char *name,double value){if(isfinite(value))f(name,value);else f(name,"null");};
    f("generated",s.generated);f("unique",s.unique);f("traced",s.traced);f("refined",s.refined);
    f("feedback_starts",s.feedback_starts);f("feedback_replays",s.feedback_replays);f("feedback_refinements",s.feedback_refinements);
    f("exact_evaluations",s.exact_evaluations);f("exact_invalid",s.exact_invalid);
    f("tail_generated",s.tail_generated);f("tail_evaluations",s.tail_evaluations);
    f("tail_duplicates",s.tail_duplicates);f("tail_accepted",s.tail_accepted);
    f("selected_new",s.selected_new?"true":"false");f("exact_baseline_valid",s.exact_baseline_valid?"true":"false");
    f("deadline_reached",s.deadline_reached?"true":"false");
    f("diagnostics_current",s.diagnostics_current?"true":"false");
    finite("before_estimated_cycles",s.before_estimated_cycles);finite("after_estimated_cycles",s.after_estimated_cycles);
    if(s.before_exact_cycles>=0)f("before_exact_cycles",s.before_exact_cycles);else f("before_exact_cycles","null");
    if(s.after_exact_cycles>=0)f("after_exact_cycles",s.after_exact_cycles);else f("after_exact_cycles","null");
    if(s.legacy_exact_cycles>=0)f("legacy_exact_cycles",s.legacy_exact_cycles);else f("legacy_exact_cycles","null");
    if(s.pool_exact_cycles>=0)f("pool_exact_cycles",s.pool_exact_cycles);else f("pool_exact_cycles","null");
    f("before_total_bytes",s.before_total_bytes);f("after_total_bytes",s.after_total_bytes);
    f("candidate_seconds",s.candidate_seconds);f("feedback_seconds",s.feedback_seconds);f("exact_seconds",s.exact_seconds);
    f("tail_seconds",s.tail_seconds);
    o<<",\"stage\":"<<json_string(s.stage)<<",\"selected_label\":"<<json_string(s.selected_label)
     <<",\"error\":"<<json_string(s.error);
    o<<",\"exact_errors\":[";
    for(size_t i=0;i<s.exact_errors.size();++i){if(i)o<<',';o<<json_string(s.exact_errors[i]);}
    o<<"],\"exact_candidates\":[";
    for(size_t i=0;i<s.exact_candidates.size();++i){
        if(i)o<<',';
        const auto &c=s.exact_candidates[i];
        o<<"{\"phase\":"<<json_string(c.phase)<<",\"label\":"<<json_string(c.label)<<",\"cycles\":";
        if(c.cycles>=0)o<<c.cycles;else o<<"null";
        o<<",\"route_bytes\":"<<c.route_bytes<<",\"spill_bytes\":"<<c.spill_bytes
         <<",\"improved\":"<<(c.improved?"true":"false")<<",\"error\":"<<json_string(c.error)<<'}';
    }
    o<<"],\"feedback\":[";
    for(size_t i=0;i<s.feedback.size();++i){if(i)o<<',';o<<feedback_stats_json(s.feedback[i]);}
    o<<"]}";return o.str();
}
} // namespace dag_chain
