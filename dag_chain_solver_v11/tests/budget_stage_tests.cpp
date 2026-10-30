#include "annealing_search.hpp"
#include "communication.hpp"
#include "official_timing.hpp"
#include "v10_search.hpp"

using namespace dag_chain;
namespace {
void require(bool ok,const char *message) {if(!ok)throw runtime_error(message);}
Deadline expired() {
    Deadline d;d.start=Clock::now()-chrono::seconds(1);d.limit=0.001;d.enabled=true;return d;
}
Deadline generous() {
    Deadline d;d.start=Clock::now();d.limit=60;d.enabled=true;return d;
}
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
Result seed(const Graph &g) {
    Result r;r.solution.order=g.topo;r.solution.core.assign(g.size(),0);
    r.solution.label="deadline_fixture";r.solution.placement_group=g.writer_group;
    r.estimate=evaluate_proxy(g,3,Config{},r.solution,0.03,nullptr);
    r.refined=refine_estimate(g,r.solution,3);r.generated_for_cores=3;
    r.candidates=7;r.refined_candidates=3;
    require(r.refined.valid,"fixture refinement failed");
    require(official_timing(g,r.solution,3).valid,"fixture is not officially executable");
    return r;
}
void unchanged(const Graph &g,const Result &original,const Result &result) {
    require(solution_json(g,result.solution,3)==solution_json(g,original.solution,3),
            "expired stage changed or lost its moved-in complete plan");
    require(result.solution.placement_group==original.solution.placement_group,
            "expired stage lost placement constraints");
    require(result.candidates==original.candidates && result.refined_candidates==original.refined_candidates,
            "expired stage counted unfinished work as completed");
    validate(g,result.solution,3);
    require(official_timing(g,result.solution,3).valid,"expired output is not officially executable");
}
void identical(const Graph &g,const Result &a,const Result &b) {
    require(solution_json(g,a.solution,3)==solution_json(g,b.solution,3),
            "unexpired deadline changed the completed search result");
    require(a.solution.placement_group==b.solution.placement_group,
            "unexpired deadline changed same-core constraints");
    validate(g,b.solution,3);
    const auto measured=official_timing(g,b.solution,3);
    require(measured.valid,"unexpired output is not officially executable");
}
void immediately_expired_stages() {
    const auto g=fixture();const auto original=seed(g);
    {
        auto input=original;auto stop=expired();FeedbackStats stats;
        const auto result=improve_spill_order(g,3,move(input),&stats,nullptr,nullptr,&stop);
        unchanged(g,original,result);
        require(stats.deadline_reached && stats.stop_reason=="deadline","feedback missed expired deadline");
        require(stats.traced_candidates==0 && stats.refined_candidates==0 && stats.rejected_trace==0 &&
                stats.rejected_refine==0,"feedback counted cancellation as scoring failure");
    }
    {
        auto input=original;auto stop=expired();CommunicationStats stats;
        const auto result=improve_communication_groups(g,3,move(input),&stats,&stop);
        unchanged(g,original,result);
        require(stats.deadline_reached && stats.generated==0 && stats.traced_candidates==0 &&
                stats.rejected_trace==0 && stats.rejected_refine==0,"communication cancellation audit failed");
    }
    {
        auto input=original;auto stop=expired();V10Stats stats;
        const auto result=improve_v10(g,3,move(input),V10Stage::exact,&stats,&stop);
        unchanged(g,original,result);
        require(stats.deadline_reached && stats.generated==0 && stats.exact_evaluations==0 &&
                stats.exact_invalid==0 && stats.error.empty(),"v10 counted expiration as official invalidity");
    }
    {
        auto input=original;auto stop=expired();AnnealingSearchStats stats;
        const auto result=improve_annealing(g,3,move(input),AnnealingStage::annealed,&stats,&stop);
        unchanged(g,original,result);
        // The final stage intentionally completes one compulsory exact baseline
        // check even after the optional-search deadline, before writing a plan.
        require(stats.deadline_reached && stats.baseline_valid && stats.exact_evaluations==1 &&
                stats.exact_invalid==0 && stats.greedy_generated==0 && stats.sa_generated==0 &&
                stats.runs.empty(),"SA expiration did more than final baseline verification");
        const auto measured=official_timing(g,result.solution,3);
        require(stats.after_cycles==measured.cycles && stats.after_bytes==
            static_cast<unsigned long long>(measured.route_bytes)+static_cast<unsigned long long>(measured.spill_bytes),
            "expired SA exact fields do not describe the returned plan");
    }
}
void generous_deadlines_preserve_search() {
    const auto g=fixture();const auto original=seed(g);
    {
        FeedbackStats fixed,timed;
        const auto a=improve_spill_order(g,3,original,&fixed);
        auto stop=generous();const auto b=improve_spill_order(g,3,original,&timed,nullptr,nullptr,&stop);
        identical(g,a,b);
        require(!timed.deadline_reached && fixed.traced_candidates==timed.traced_candidates &&
                fixed.refined_candidates==timed.refined_candidates,"feedback lost fixed-work prefix with ample time");
    }
    {
        CommunicationStats fixed,timed;
        const auto a=improve_communication_groups(g,3,original,&fixed);
        auto stop=generous();const auto b=improve_communication_groups(g,3,original,&timed,&stop);
        identical(g,a,b);
        require(!timed.deadline_reached && fixed.generated==timed.generated &&
                fixed.refined_candidates==timed.refined_candidates,"communication changed its full candidate set");
    }
    {
        V10Stats fixed,timed;
        const auto a=improve_v10(g,3,original,V10Stage::exact,&fixed);
        auto stop=generous();const auto b=improve_v10(g,3,original,V10Stage::exact,&timed,&stop);
        identical(g,a,b);
        require(!timed.deadline_reached && fixed.exact_evaluations==timed.exact_evaluations &&
                fixed.after_exact_cycles==timed.after_exact_cycles && fixed.after_total_bytes==timed.after_total_bytes,
                "v10 generous deadline changed exact scoring");
    }
    {
        AnnealingSearchStats fixed,timed;
        const auto a=improve_annealing(g,3,original,AnnealingStage::annealed,&fixed);
        auto stop=generous();const auto b=improve_annealing(g,3,original,AnnealingStage::annealed,&timed,&stop);
        identical(g,a,b);
        require(!timed.deadline_reached && fixed.runs.size()==timed.runs.size() &&
                fixed.exact_evaluations==timed.exact_evaluations && fixed.after_cycles==timed.after_cycles &&
                fixed.after_bytes==timed.after_bytes,"SA generous deadline changed fixed-seed exact result");
    }
}
void expired_annealing_preserves_writer_closure() {
    const Graph g(JsonParser(R"({"ops":[
      {"id":0,"op":"COMPUTE","pipe":"PIPE_M","cycles":1},
      {"id":1,"op":"COMPUTE","pipe":"PIPE_V","cycles":2},
      {"id":2,"op":"COMPUTE","pipe":"PIPE_V","cycles":3},
      {"id":3,"op":"COMPUTE","pipe":"PIPE_V","cycles":4}],
      "tensors":[{"id":100,"pos":"UB","size":60}],
      "edges":[{"source":1,"target":100},{"source":2,"target":100},{"source":100,"target":3}]})").parse());
    Decomposition d;d.bundles={{0,1},{2},{3}};
    const vector<int> greedy{0,0,0,1};AnnealingLimits limits;AnnealingStats stats;
    auto stop=expired();const auto result=anneal_assignment(g,d,2,greedy,limits,&stats,&stop);
    require(stats.deadline_reached && stats.proposals==0 && stats.evaluated==0 && stats.touched_nodes==0,
            "expired low-level annealing executed a proposal");
    require(result.best_assignment==greedy && !result.candidates.empty(),"expired SA lost greedy state");
    require(result.placement_group[0]==result.placement_group[1] &&
            result.placement_group[1]==result.placement_group[2],"expired SA lost transitive writer/bundle closure");
    for(const auto &candidate:result.candidates) {
        require(candidate.assignment==greedy,"zero-step SA exported a changed assignment");
        Solution s;s.core=candidate.assignment;s.order=g.topo;s.placement_group=result.placement_group;
        validate(g,s,2);
        require(official_timing(g,s,2).valid,"expired writer-closed plan is not executable");
    }
}
} // namespace
int main() {
    try {
        immediately_expired_stages();generous_deadlines_preserve_search();expired_annealing_preserves_writer_closure();
        cout<<"budget stage tests passed\n";return 0;
    } catch(const exception &error) {cerr<<error.what()<<'\n';return 1;}
}
