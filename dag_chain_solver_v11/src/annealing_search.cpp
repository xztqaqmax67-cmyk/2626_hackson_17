#include "annealing_search.hpp"
#include "communication.hpp"
#include "official_timing.hpp"
#include "search_limits.hpp"

namespace dag_chain {
namespace {
using Bytes = unsigned long long;
double seconds(Clock::time_point start) { return chrono::duration<double>(Clock::now()-start).count(); }
vector<int> stream_key(const Solution &s,int cores) {
    vector<vector<int>> streams(cores);
    for(int v:s.order)streams[s.core[v]].push_back(v);
    vector<int> key;
    for(const auto &part:streams){key.insert(key.end(),part.begin(),part.end());key.push_back(-1);}
    return key;
}
string quoted(const string &s) {
    ostringstream o;o<<'"';
    for(unsigned char c:s) {
        if(c=='"'||c=='\\')o<<'\\'<<char(c);
        else if(c<32)o<<"\\u"<<hex<<setw(4)<<setfill('0')<<int(c)<<dec;
        else o<<char(c);
    }
    o<<'"';return o.str();
}
struct Seed { Decomposition groups; vector<int> greedy; size_t variant=0; };
struct Proposal { Solution solution; Metrics proxy; };
} // namespace

Result improve_annealing(const Graph &g,int cores,Result incumbent,
                         AnnealingStage stage,AnnealingSearchStats *statistics,Deadline *deadline) {
    const auto total_started=Clock::now();
    AnnealingSearchStats stats;
    stats.stage=stage==AnnealingStage::baseline?"baseline":stage==AnnealingStage::greedy_only?"greedy_only":"annealed";
    auto stopped=[&]() {
        if(deadline && deadline->expired()){stats.deadline_reached=true;return true;}
        return false;
    };
    const auto original_key=stream_key(incumbent.solution,cores);
    const auto original_assignment=incumbent.solution.core;
    const auto before_candidates=incumbent.candidates, before_refined=incumbent.refined_candidates;
    auto finish=[&]() {
        stats.selected_new=stream_key(incumbent.solution,cores)!=original_key;
        stats.assignment_changed=incumbent.solution.core!=original_assignment;
        stats.selected_label=incumbent.solution.label;
        stats.total_seconds=seconds(total_started);
        incumbent.candidates=before_candidates+stats.greedy_generated+stats.sa_generated;
        incumbent.refined_candidates=before_refined+size_t(stats.diagnostics_refreshed);
        validate(g,incumbent.solution,cores);
        if(statistics)*statistics=stats;
        return incumbent;
    };
    auto started=Clock::now();
    auto baseline=official_timing(g,incumbent.solution,cores);
    stats.baseline_seconds=seconds(started);++stats.exact_evaluations;
    stats.baseline_valid=baseline.valid;
    stats.exact_candidates.push_back({"baseline",incumbent.solution.label,baseline.error,
        baseline.valid?baseline.cycles:-1,baseline.route_bytes,baseline.spill_bytes,false});
    if(!baseline.valid){++stats.exact_invalid;stats.error=baseline.error;return finish();}
    stats.baseline_cycles=stats.greedy_cycles=stats.after_cycles=baseline.cycles;
    stats.baseline_bytes=stats.greedy_bytes=stats.after_bytes=Bytes(baseline.route_bytes)+Bytes(baseline.spill_bytes);
    if(stage==AnnealingStage::baseline)return finish();
    auto best=make_pair(stats.after_cycles,stats.after_bytes);
    set<vector<int>> evaluated{original_key};
    auto offer=[&](Solution &solution,const string &phase) {
        if(stopped())return;
        if(!evaluated.insert(stream_key(solution,cores)).second)return;
        auto measured=official_timing(g,solution,cores);++stats.exact_evaluations;
        const auto score=make_pair(measured.cycles,Bytes(measured.route_bytes)+Bytes(measured.spill_bytes));
        const bool improved=measured.valid && score<best;
        stats.exact_candidates.push_back({phase,solution.label,measured.error,
            measured.valid?measured.cycles:-1,measured.route_bytes,measured.spill_bytes,improved});
        if(!measured.valid){++stats.exact_invalid;return;}
        if(improved){best=score;incumbent.solution=solution;}
    };
    Deadline *schedule_deadline=deadline;
    auto schedule=[&](const vector<int> &assignment,const vector<int> &groups,
                      size_t mode,const string &label) {
        if(schedule_deadline)schedule_deadline->check();
        SeedOptions opt;opt.mode=mode==0?"critical":"dfs";opt.window=8;opt.memory=0.5;
        auto s=operation_plan(g,cores,Config{},opt,&assignment,schedule_deadline);
        if(s.core!=assignment)throw runtime_error("annealing schedule changed fixed writer-closed assignment");
        s.placement_group=groups;s.label=label+"_"+opt.mode;
        validate(g,s,cores);return s;
    };
    started=Clock::now();
    const array<double,2> scales{0.5,1.0};
    vector<Seed> seeds;vector<Solution> greedy;
    set<vector<int>> known_streams{original_key};
    try { for(size_t variant=0;variant<search_limits::annealing_variants;++variant) {
        if(deadline)deadline->check();
        auto groups=communication_decompose(g,cores,scales[variant],variant==0);
        auto assignment=pack_decomposition(g,groups,cores,variant==0?2.0:4.0);
        for(size_t mode=0;mode<search_limits::annealing_order_variants;++mode) {
            auto s=schedule(assignment,groups.bundle_of,mode,"sa_greedy_v"+to_string(variant));
            ++stats.greedy_generated;
            if(known_streams.insert(stream_key(s,cores)).second)greedy.push_back(move(s));
        }
        seeds.push_back({move(groups),move(assignment),variant});
    }} catch(const SearchExpired &){stats.deadline_reached=true;}
    stats.greedy_unique=greedy.size();
    stats.candidate_seconds+=seconds(started);
    started=Clock::now();
    for(auto &s:greedy){if(stopped())break;offer(s,"greedy");}
    stats.greedy_exact_seconds=seconds(started);
    stats.greedy_cycles=best.first;stats.greedy_bytes=best.second;

    if(stage==AnnealingStage::annealed && !stopped()) {
        started=Clock::now();
        vector<Proposal> proposals;
        // Reserve part of the REMAINING shared budget for complete exact scoring.
        // This deadline has the same generation start, never a fresh 18 seconds.
        Deadline candidate_deadline;
        Deadline *proposal_deadline=nullptr;
        if(deadline && deadline->enabled) {
            candidate_deadline=*deadline;
            const double now=deadline->elapsed();
            candidate_deadline.limit=now+max(0.0,deadline->limit-now)*0.55;
            proposal_deadline=&candidate_deadline;
        }
        schedule_deadline=proposal_deadline;
        try { for(const auto &seed:seeds) {
            // A restart begins at the SAME greedy assignment; accepted uphill
            // moves are internal search states, never automatically output.
            set<vector<int>> assignments{seed.greedy};
            for(size_t restart=0;restart<search_limits::annealing_restarts;++restart) {
                if(proposal_deadline)proposal_deadline->check();
                AnnealingLimits limits;
                limits.seed+=uint64_t(cores)*0xd1b54a32d192ed03ULL+
                    uint64_t(seed.variant*search_limits::annealing_restarts+restart+1)*0x9e3779b97f4a7c15ULL;
                AnnealingStats run;
                Deadline run_deadline;
                Deadline *run_stop=nullptr;
                if(proposal_deadline) {
                    run_deadline=*proposal_deadline;
                    const double remaining=max(0.0,run_deadline.limit-run_deadline.elapsed());
                    // A stopped run must still have time to schedule its best map.
                    run_deadline.limit-=min(0.25,remaining*0.20);
                    run_stop=&run_deadline;
                }
                const auto sa_started=Clock::now();
                auto states=anneal_assignment(g,seed.groups,cores,seed.greedy,limits,&run,run_stop);
                stats.deadline_reached=stats.deadline_reached||run.deadline_reached;
                stats.annealing_seconds+=seconds(sa_started);
                stats.runs.push_back({seed.variant,restart,limits.seed,run});
                for(const auto &state:states.candidates)if(assignments.insert(state.assignment).second)
                    for(size_t mode=0;mode<search_limits::annealing_order_variants;++mode) {
                        auto s=schedule(state.assignment,states.placement_group,mode,
                            "sa_v"+to_string(seed.variant)+"_r"+to_string(restart)+"_"+state.origin);
                        ++stats.sa_generated;
                        if(!known_streams.insert(stream_key(s,cores)).second)continue;
                        auto proxy=evaluate_proxy(g,cores,Config{},s,0.03,proposal_deadline);++stats.proxy_evaluations;
                        proposals.push_back({move(s),move(proxy)});
                    }
            }
        }} catch(const SearchExpired &){stats.deadline_reached=true;}
        schedule_deadline=deadline;
        stats.sa_unique=proposals.size();
        // A small diverse shortlist: estimated time, total bytes (including a
        // cheap spill estimate), and compute-pipe bound. No proxy is legality.
        array<vector<size_t>,3> rank;
        for(auto &order:rank){order.resize(proposals.size());iota(order.begin(),order.end(),size_t(0));}
        for(size_t mode=0;mode<rank.size();++mode)
            stable_sort(rank[mode].begin(),rank[mode].end(),[&](size_t a,size_t b){
                auto score=[&](size_t i){const auto &m=proposals[i].proxy;
                    const double first=mode==0?m.score:mode==1?m.route+m.spill:m.pipe_bound;
                    return make_tuple(m.memory_warning,first,m.score,i);};
                return score(a)<score(b);
            });
        vector<size_t> shortlist;vector<unsigned char> chosen(proposals.size());
        for(size_t offset=0;offset<proposals.size() && shortlist.size()<search_limits::annealing_exact_candidates;++offset)
            for(const auto &order:rank)if(!chosen[order[offset]] && shortlist.size()<search_limits::annealing_exact_candidates) {
                chosen[order[offset]]=1;shortlist.push_back(order[offset]);
            }
        stats.candidate_seconds+=seconds(started);
        started=Clock::now();
        for(size_t i:shortlist){if(stopped())break;offer(proposals[i].solution,"annealed");}
        stats.sa_exact_seconds=seconds(started);
    }
    stats.after_cycles=best.first;stats.after_bytes=best.second;
    if(stream_key(incumbent.solution,cores)!=original_key && (!deadline || !deadline->enabled)) {
        // Refresh diagnostics only after exact acceptance; an approximate
        // model cannot veto an officially executable lower-cycle plan.
        incumbent.estimate=evaluate_proxy(g,cores,Config{},incumbent.solution,0.03,nullptr);
        incumbent.refined=refine_estimate(g,incumbent.solution,cores);
        incumbent.generated_for_cores=cores;
        stats.diagnostics_refreshed=true;
    }
    if(stream_key(incumbent.solution,cores)!=original_key)incumbent.generated_for_cores=cores;
    stopped(); // Mark an atomic exact replay that crossed the stage boundary.
    return finish();
}

string annealing_search_stats_json(const AnnealingSearchStats &s) {
    ostringstream o;o<<setprecision(17)<<'{';bool first=true;
    auto f=[&](const char *name,auto value){if(!first)o<<',';first=false;o<<'"'<<name<<"\":"<<value;};
    auto time=[&](const char *name,i64 value){if(value>=0)f(name,value);else f(name,"null");};
    f("stage",quoted(s.stage));f("selected_label",quoted(s.selected_label));f("error",quoted(s.error));
    f("baseline_valid",s.baseline_valid?"true":"false");f("selected_new",s.selected_new?"true":"false");
    f("assignment_changed",s.assignment_changed?"true":"false");
    f("deadline_reached",s.deadline_reached?"true":"false");
    f("diagnostics_refreshed",s.diagnostics_refreshed?"true":"false");
    time("baseline_cycles",s.baseline_cycles);time("greedy_cycles",s.greedy_cycles);time("after_cycles",s.after_cycles);
    f("baseline_bytes",s.baseline_bytes);f("greedy_bytes",s.greedy_bytes);f("after_bytes",s.after_bytes);
    f("greedy_generated",s.greedy_generated);f("greedy_unique",s.greedy_unique);
    f("sa_generated",s.sa_generated);f("sa_unique",s.sa_unique);f("proxy_evaluations",s.proxy_evaluations);
    f("exact_evaluations",s.exact_evaluations);f("exact_invalid",s.exact_invalid);
    f("baseline_seconds",s.baseline_seconds);f("candidate_seconds",s.candidate_seconds);
    f("annealing_seconds",s.annealing_seconds);f("greedy_exact_seconds",s.greedy_exact_seconds);
    f("sa_exact_seconds",s.sa_exact_seconds);f("total_seconds",s.total_seconds);
    o<<",\"runs\":[";
    for(size_t i=0;i<s.runs.size();++i) {
        if(i)o<<',';
        const auto &run=s.runs[i];const auto &v=run.statistics;
        o<<"{\"variant\":"<<run.variant<<",\"restart\":"<<run.restart<<",\"seed\":"<<run.seed;
        auto r=[&](const char *name,auto value){o<<",\""<<name<<"\":"<<value;};
        r("deadline_reached",v.deadline_reached?"true":"false");
        r("proposals",v.proposals);r("evaluated",v.evaluated);r("move_proposals",v.move_proposals);r("swap_proposals",v.swap_proposals);
        r("accepted",v.accepted);r("uphill_accepted",v.uphill_accepted);r("rejected",v.rejected);
        r("skipped_size",v.skipped_size);r("skipped_budget",v.skipped_budget);r("skipped_noop",v.skipped_noop);
        r("touched_nodes",v.touched_nodes);r("undo_nodes",v.undo_nodes);r("rebasings",v.rebasings);r("bundles",v.bundles);
        r("initial_energy",v.initial_energy);r("best_energy",v.best_energy);r("final_energy",v.final_energy);
        r("initial_temperature",v.initial_temperature);r("final_temperature",v.final_temperature);
        r("initial_route_bytes",v.initial_route_bytes);r("best_route_bytes",v.best_route_bytes);
        r("initial_pipe_bound",v.initial_pipe_bound);r("best_pipe_bound",v.best_pipe_bound);o<<'}';
    }
    o<<"],\"exact_candidates\":[";
    for(size_t i=0;i<s.exact_candidates.size();++i) {
        if(i)o<<',';
        const auto &v=s.exact_candidates[i];
        o<<"{\"phase\":"<<quoted(v.phase)<<",\"label\":"<<quoted(v.label)<<",\"cycles\":";
        if(v.cycles>=0)o<<v.cycles;else o<<"null";
        o<<",\"route_bytes\":"<<v.route_bytes<<",\"spill_bytes\":"<<v.spill_bytes
         <<",\"improved\":"<<(v.improved?"true":"false")<<",\"error\":"<<quoted(v.error)<<'}';
    }
    o<<"]}";return o.str();
}
} // namespace dag_chain
