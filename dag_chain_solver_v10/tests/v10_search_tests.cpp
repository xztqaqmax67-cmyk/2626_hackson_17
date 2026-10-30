#include "v10_search.hpp"
#include "official_timing.hpp"
#include "search_limits.hpp"

namespace {
void require(bool ok,const char *message){if(!ok)throw runtime_error(message);}
Graph fixture() {
    return Graph(JsonParser(R"({"ops":[
      {"id":0,"op":"COMPUTE","pipe":"PIPE_V","cycles":20000},
      {"id":1,"op":"COMPUTE","pipe":"PIPE_V","cycles":17000},
      {"id":2,"op":"COMPUTE","pipe":"PIPE_M","cycles":21000},
      {"id":3,"op":"COMPUTE","pipe":"PIPE_V","cycles":18000},
      {"id":4,"op":"COMPUTE","pipe":"PIPE_M","cycles":19000},
      {"id":5,"op":"COMPUTE","pipe":"PIPE_V","cycles":22000},
      {"id":6,"op":"COMPUTE","pipe":"PIPE_V","cycles":2000},
      {"id":7,"op":"COMPUTE","pipe":"PIPE_M","cycles":2000}],
      "tensors":[{"id":100,"pos":"UB","size":64000},
      {"id":101,"pos":"UB","size":64000},{"id":102,"pos":"UB","size":64000}],
      "edges":[{"source":0,"target":100},{"source":100,"target":3},
      {"source":100,"target":6},{"source":1,"target":101},{"source":101,"target":4},
      {"source":2,"target":102},{"source":102,"target":5},{"source":102,"target":7}]})").parse());
}
void exercise() {
    const auto g=fixture();
    Solution s;s.order=g.topo;s.core.assign(g.size(),0);s.label="manual_baseline";
    dag_chain::Result base;base.solution=s;base.estimate=evaluate_proxy(g,3,Config{},s,0.03,nullptr);
    base.refined=dag_chain::refine_estimate(g,s,3);base.generated_for_cores=3;
    require(base.refined.valid,"fixture must have valid initial estimate");
    const auto initial_trace=dag_chain::official_spill_trace(g,s,3);
    const auto initial=dag_chain::official_timing(g,s,3);
    require(initial_trace.valid && initial.valid,"fixture must be officially executable");
    const auto json=solution_json(g,s,3);
    vector<dag_chain::V10ExactCandidate> legacy_candidates;
    pair<i64,unsigned long long> legacy_result{-1,0},pool_result{-1,0};
    for(auto stage:{dag_chain::V10Stage::v9,dag_chain::V10Stage::tensor_memory,
                    dag_chain::V10Stage::multi_feedback,dag_chain::V10Stage::exact_legacy,
                    dag_chain::V10Stage::exact_pool,dag_chain::V10Stage::exact}) {
        dag_chain::V10Stats stats;
        const auto result=dag_chain::improve_v10(g,3,base,stage,&stats);
        validate(g,result.solution,3);
        const auto trace=dag_chain::official_spill_trace(g,result.solution,3);
        require(trace.valid,"portfolio produced illegal memory sequence");
        require(stats.refined<=dag_chain::search_limits::v10_refine_candidates+size_t(stats.tail_accepted>0),"candidate refinement budget exceeded");
        require(stats.feedback_replays<=dag_chain::search_limits::v10_feedback_replays,"multi-start replay budget multiplied");
        require(stats.feedback_refinements<=dag_chain::search_limits::v10_feedback_refinements,"multi-start refine budget multiplied");
        const size_t exact_cap=stage<=dag_chain::V10Stage::exact_legacy?dag_chain::search_limits::v10_exact_candidates:
            dag_chain::search_limits::v10_exact_pool_candidates+(stage==dag_chain::V10Stage::exact?
            dag_chain::search_limits::v10_timing_order_candidates+dag_chain::search_limits::v10_exact_spill_candidates:0);
        require(stats.exact_evaluations<=exact_cap,"exact shortlist budget exceeded");
        if(stage<dag_chain::V10Stage::multi_feedback)
            require(stats.feedback_starts==0 && stats.feedback_replays==0 && stats.feedback_refinements==0,
                    "feedback ran before its stage gate");
        if(stage<dag_chain::V10Stage::exact_legacy)
            require(stats.exact_evaluations==0 && stats.exact_candidates.empty() && !stats.exact_baseline_valid,
                    "exact scoring ran before its stage gate");
        if(stage<dag_chain::V10Stage::exact)
            require(stats.tail_generated==0 && stats.tail_evaluations==0 && stats.tail_duplicates==0 && stats.tail_accepted==0,
                    "tail proposals ran before their stage gate");
        if(stage==dag_chain::V10Stage::v9)require(solution_json(g,result.solution,3)==json,"disabled stage changes v9 answer");
        else if(stage<dag_chain::V10Stage::exact_legacy) {
            require(result.refined.cycles<=base.refined.cycles,"approximate stage violates entry cycle guard");
            require(trace.route_bytes+trace.spill_bytes<=initial_trace.route_bytes+initial_trace.spill_bytes,"approximate stage violates traffic guard");
        } else {
            const auto final=dag_chain::official_timing(g,result.solution,3);
            require(stats.exact_baseline_valid && final.valid && final.cycles<=initial.cycles,"exact selection regressed incumbent");
            require(stats.before_exact_cycles==initial.cycles && stats.after_exact_cycles==final.cycles,"exact diagnostics do not describe output");
            require(stats.after_exact_cycles<=stats.pool_exact_cycles && stats.pool_exact_cycles<=stats.legacy_exact_cycles,
                    "expanded search failed to retain previous exact winners");
            require(stats.tail_evaluations<=dag_chain::search_limits::v10_timing_order_candidates+
                    dag_chain::search_limits::v10_exact_spill_candidates,"tail budget multiplied");
            require(stats.exact_candidates.size()==stats.exact_evaluations,"exact audit trail missing evaluations");
            const auto measured=make_pair(final.cycles,
                static_cast<unsigned long long>(trace.route_bytes)+static_cast<unsigned long long>(trace.spill_bytes));
            if(stage==dag_chain::V10Stage::exact_legacy) {
                legacy_candidates=stats.exact_candidates;
                legacy_result=measured;
            } else {
                // Compare independent runs, not merely the current run's
                // internally reported monotonic counters. The complete old
                // candidate list and its tie order must survive as a prefix.
                require(!legacy_candidates.empty() && stats.exact_candidates.size()>=legacy_candidates.size(),
                        "expanded stage lost the independently measured legacy shortlist");
                for(size_t i=0;i<legacy_candidates.size();++i) {
                    const auto &expected=legacy_candidates[i], &actual=stats.exact_candidates[i];
                    require(actual.phase==expected.phase && actual.label==expected.label && actual.error==expected.error &&
                            actual.cycles==expected.cycles && actual.route_bytes==expected.route_bytes &&
                            actual.spill_bytes==expected.spill_bytes && actual.improved==expected.improved,
                            "legacy exact prefix or its tie order changed across independent stages");
                }
                require(stats.legacy_exact_cycles==legacy_result.first && measured<=legacy_result,
                        "expanded stage does not retain the independently measured legacy winner");
                if(stage==dag_chain::V10Stage::exact_pool)pool_result=measured;
                else require(pool_result.first>=0 && stats.pool_exact_cycles==pool_result.first && measured<=pool_result,
                             "tail does not retain the independently measured six-candidate winner");
            }
            const auto repeat=dag_chain::improve_v10(g,3,base,stage);
            require(solution_json(g,result.solution,3)==solution_json(g,repeat.solution,3),"v10 fixed-work search nondeterministic");
        }
        const auto diagnostic=JsonParser(dag_chain::v10_stats_json(stats)).parse();
        require(diagnostic.type==Json::Object,"statistics must be valid JSON");
    }
    dag_chain::FeedbackStats stats;
    const dag_chain::FeedbackBudget budget{1,1,1};
    const auto bounded=dag_chain::improve_spill_order(g,3,base,&stats,nullptr,&budget);
    validate(g,bounded.solution,3);
    require(stats.traced_candidates<=1 && stats.refined_candidates<=1,"per-start small budgets ignored");
}
}
int main(){try{exercise();cout<<"v10 portfolio invariants passed\n";return 0;}catch(const exception &e){cerr<<e.what()<<'\n';return 1;}}
