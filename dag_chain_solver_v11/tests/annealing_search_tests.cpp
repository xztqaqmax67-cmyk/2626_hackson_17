#include "annealing_search.hpp"
#include "official_timing.hpp"
#include "search_limits.hpp"

namespace {
using namespace dag_chain;
void require(bool ok,const char *message){if(!ok)throw runtime_error(message);}
Graph graph() {
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
void portfolio() {
    const auto g=graph();Result base;base.solution.order=g.topo;base.solution.core.assign(g.size(),0);
    base.solution.label="fixture";base.estimate=evaluate_proxy(g,3,Config{},base.solution,0.03,nullptr);
    base.refined=refine_estimate(g,base.solution,3);base.generated_for_cores=3;
    const auto original=official_timing(g,base.solution,3);require(original.valid,"fixture invalid");
    vector<AnnealingEvaluation> prefix;
    pair<i64,unsigned long long> greedy_score;
    for(auto stage:{AnnealingStage::baseline,AnnealingStage::greedy_only,AnnealingStage::annealed}) {
        AnnealingSearchStats stats;const auto result=improve_annealing(g,3,base,stage,&stats);
        const auto measured=official_timing(g,result.solution,3);require(measured.valid,"output not executable");
        const auto bytes=static_cast<unsigned long long>(measured.route_bytes)+static_cast<unsigned long long>(measured.spill_bytes);
        require(stats.baseline_valid && stats.baseline_cycles==original.cycles,"baseline diagnostic mismatch");
        require(stats.after_cycles==measured.cycles && stats.after_bytes==bytes,"selected score does not match output");
        require(stats.after_cycles<=stats.greedy_cycles && stats.greedy_cycles<=stats.baseline_cycles,"stage winner regressed");
        require(stats.exact_evaluations==stats.exact_candidates.size(),"missing exact evaluation audit");
        require(stats.exact_evaluations<=1+search_limits::annealing_variants*search_limits::annealing_order_variants+
            (stage==AnnealingStage::annealed?search_limits::annealing_exact_candidates:0),"exact cap multiplied");
        if(stage==AnnealingStage::baseline) {
            require(solution_json(g,result.solution,3)==solution_json(g,base.solution,3),"baseline changed output");
            require(stats.greedy_generated==0 && stats.runs.empty() && stats.exact_evaluations==1,"baseline stage did extra search");
        } else if(stage==AnnealingStage::greedy_only) {
            require(stats.runs.empty() && stats.sa_generated==0,"greedy-only stage ran SA");
            prefix=stats.exact_candidates;greedy_score={measured.cycles,bytes};
        } else {
            require(stats.runs.size()==search_limits::annealing_variants*search_limits::annealing_restarts,"missing greedy SA starts");
            require(stats.exact_candidates.size()>=prefix.size(),"lost greedy candidates");
            for(size_t i=0;i<prefix.size();++i) {
                const auto &a=prefix[i],&b=stats.exact_candidates[i];
                require(a.phase==b.phase && a.label==b.label && a.cycles==b.cycles && a.route_bytes==b.route_bytes &&
                    a.spill_bytes==b.spill_bytes && a.improved==b.improved && a.error==b.error,"greedy prefix changed across runs");
            }
            require(make_pair(measured.cycles,bytes)<=greedy_score,"SA final gate regressed independent greedy output");
            require(stats.greedy_cycles==greedy_score.first && stats.greedy_bytes==greedy_score.second,"greedy audit mismatch");
            for(const auto &r:stats.runs) {
                require(r.statistics.proposals<=AnnealingLimits{}.proposals,"per-run proposal cap exceeded");
                require(r.statistics.touched_nodes<=AnnealingLimits{}.node_touches,"touch budget exceeded");
                require(r.statistics.uphill_accepted<=r.statistics.accepted,"uphill counter inconsistent");
            }
            const auto repeat=improve_annealing(g,3,base,stage);
            require(solution_json(g,repeat.solution,3)==solution_json(g,result.solution,3),"fixed seed not repeatable");
        }
        require(JsonParser(annealing_search_stats_json(stats)).parse().type==Json::Object,"invalid stats JSON");
    }
}
}
int main(){try{portfolio();cout<<"annealing portfolio passed\n";return 0;}catch(const exception &e){cerr<<e.what()<<'\n';return 1;}}
