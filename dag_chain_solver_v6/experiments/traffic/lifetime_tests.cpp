#include "lifetime.hpp"

namespace {
void require(bool condition, const string &message) {
    if (!condition) throw runtime_error(message);
}
Graph fixture() {
    return Graph(JsonParser(R"({"ops":[
        {"id":0,"op":"make_a","pipe":"PIPE_V","cycles":100},
        {"id":1,"op":"make_b","pipe":"PIPE_V","cycles":100},
        {"id":2,"op":"consume_a","pipe":"PIPE_V","cycles":100},
        {"id":3,"op":"consume_b","pipe":"PIPE_V","cycles":100},
        {"id":4,"op":"bound_writer","pipe":"PIPE_M","cycles":1}],
        "tensors":[{"id":100,"pos":"UB","size":70000},
                   {"id":101,"pos":"UB","size":70000},
                   {"id":102,"pos":"UB","size":1}],
        "edges":[{"source":0,"target":100},{"source":100,"target":2},
                 {"source":1,"target":101},{"source":101,"target":3},
                 {"source":0,"target":102},{"source":4,"target":102}]})").parse());
}
bool one_insertion(const vector<int> &base, const vector<int> &changed) {
    for (int v : base) {
        auto a=base, b=changed;
        a.erase(find(a.begin(),a.end(),v)); b.erase(find(b.begin(),b.end(),v));
        if (a==b) return true;
    }
    return false;
}
}
int main() {
    try {
        const auto g=fixture();
        Solution base;
        base.core={0,0,0,0,0}; base.order={4,0,1,2,3};
        validate(g,base,2);
        const string before=solution_json(g,base,2);
        const auto candidates=dag_chain::lifetime_candidates(g,2,base);
        require(!candidates.empty() && candidates.size()<=24,"missing or unbounded local candidates");
        const Config cfg;
        const auto original=evaluate_proxy(g,2,cfg,base,0.03,nullptr);
        bool improved=false;
        set<string> seen;
        for (const auto &s:candidates) {
            validate(g,s,2);
            require(s.core==base.core,"lifetime reordering changed ownership");
            require(one_insertion(base.order,s.order),"unrelated operation order changed");
            require(seen.insert(solution_json(g,s,2)).second,"duplicate submitted stream");
            auto next=evaluate_proxy(g,2,cfg,s,0.03,nullptr);
            improved=improved || (!next.memory_warning && next.spill<original.spill);
        }
        require(original.spill>0 && improved,"fixture must reduce real proxy spill, not just its label");
        const auto repeated=dag_chain::lifetime_candidates(g,2,base);
        require(repeated.size()==candidates.size(),"candidate count is not deterministic");
        for(size_t i=0;i<candidates.size();++i)
            require(repeated[i].order==candidates[i].order,"candidate ordering is not deterministic");
        require(dag_chain::lifetime_candidates(g,2,base,nullptr,1).size()==1,"candidate cap ignored");
        require(dag_chain::lifetime_candidates(g,2,base,nullptr,0).empty(),"zero cap ignored");
        Deadline expired;
        expired.start=Clock::now()-chrono::seconds(1); expired.limit=0.001;
        bool caught=false;
        try { (void)dag_chain::lifetime_candidates(g,2,base,&expired); }
        catch(const SearchExpired&) { caught=true; }
        require(caught,"deadline not honored");
        require(solution_json(g,base,2)==before,"parent solution mutated");

        // A dependency chain has no legal freedom to shorten its local spans.
        Graph serial(JsonParser(R"({"ops":[
          {"id":0,"op":"A","pipe":"PIPE_M","cycles":10},
          {"id":1,"op":"B","pipe":"PIPE_M","cycles":10},
          {"id":2,"op":"C","pipe":"PIPE_M","cycles":10}],
          "tensors":[{"id":100,"pos":"UB","size":16}],
          "edges":[{"source":0,"target":1},{"source":1,"target":2},
                   {"source":0,"target":100},{"source":100,"target":2}]})").parse());
        Solution chain; chain.core={0,0,0};chain.order=serial.topo;
        require(dag_chain::lifetime_candidates(serial,2,chain).empty(),"dependency chain was illegally compressed");
        cout << "lifetime legality, fixed ownership, bounded deterministic candidates, spill reduction and cancellation passed\n";
        return 0;
    } catch(const exception &e) { cerr<<e.what()<<'\n';return 1; }
}
