#include "official_timing.hpp"

using namespace dag_chain;
namespace {
void require(bool condition, const string &message) { if (!condition) throw runtime_error(message); }
const OfficialOpTiming &at(const vector<OfficialOpTiming> &rows, i64 id) {
    for (const auto &row : rows) if (row.id == id) return row;
    throw runtime_error("missing timeline op");
}
OfficialSceneIR empty_scene(int cores = 2) { OfficialSceneIR ir; ir.valid = true; ir.tasks.resize(cores); return ir; }
void integer_sets() {
    // Oracle: list(set(values)), 64-bit CPython 3.11.11. Colliding low bits,
    // resize/reinsertion, duplicate hashes >2^61, negative hash -1 remapping.
    require(official_python_int_set_order({0,8,16,24,32,40,48,56,64,72}) ==
        vector<i64>({0,32,64,8,40,72,16,48,24,56}), "set linear probes/resize");
    const i64 p = 2305843009213693951LL;
    require(official_python_int_set_order({p,0,p+1,1,2*p,numeric_limits<i64>::max()}) ==
        vector<i64>({p,0,2*p,p+1,1,numeric_limits<i64>::max()}), "set integer hash modulus");
    require(official_python_int_set_order({-1,-2,-9,7,15,23,numeric_limits<i64>::min()}) ==
        vector<i64>({7,15,23,-2,-9,numeric_limits<i64>::min(),-1}), "set signed integer hash");
    vector<i64> input{101,37,5,69,133,197,261,325,389,453,517,581,645,709,773,837,901,965,1029,1093};
    vector<i64> expected{5,133,261,389,517,645,773,901,1029,37,69,197,325,453,581,709,837,965,1093,101};
    require(official_python_int_set_order(input) == expected, "set repeated resize");
    input.push_back(101); input.push_back(133);
    require(official_python_int_set_order(input) == expected, "set duplicates");
}
void shared_ddr() {
    auto ir = empty_scene();
    for (int c = 0; c < 2; ++c) {
        auto &g = ir.tasks[c];
        g.ops = {{1,999,-1,0,"COPY_IN"}};
        g.tensors = {{2,60 * (c+1),-1}, {3,60 * (c+1),1}};
        g.edges = {{2,1},{1,3}}; g.sequence = {1};
    }
    const auto r = official_timing(ir);
    require(r.valid, r.error); require(r.cycles == 3, "shared DDR makespan");
    require(r.cores[0].local_cycles == 1 && r.cores[1].local_cycles == 2, "local exclusive bandwidth");
    require(at(r.cores[0].timeline,1).end == 2 && at(r.cores[1].timeline,1).end == 3, "dynamic sharing completion");
}
void cross_core_delay() {
    auto ir = empty_scene();
    ir.tasks[0].ops = {{1,10,-1,2,"COMPUTE"},{2,1,-1,1,"COPY_OUT"}};
    ir.tasks[0].tensors = {{11,60,1},{12,60,-1}};
    ir.tasks[0].edges = {{1,11},{11,2},{2,12}}; ir.tasks[0].sequence = {1,2};
    ir.tasks[1].ops = {{3,1,-1,0,"COPY_IN"},{4,20,-1,3,"COMPUTE"}};
    ir.tasks[1].tensors = {{12,60,-1},{13,60,1}};
    ir.tasks[1].edges = {{12,3},{3,13},{13,4}}; ir.tasks[1].sequence = {3,4};
    ir.cross_links = {{0,1,2,3}};
    const auto r = official_timing(ir); require(r.valid, r.error);
    require(r.cycles == 1032 && at(r.cores[1].timeline,3).start == 1011, "COPY_OUT + 1000 release");
}
void capacity_reuse() {
    auto ir = empty_scene(); auto &g = ir.tasks[0];
    g.ops = {{10,5,-1,2,"COMPUTE"},{11,5,-1,3,"COMPUTE"},{12,5,-1,2,"COMPUTE"},{13,5,-1,3,"COMPUTE"}};
    g.tensors = {{100,100000,1},{101,100000,1}};
    g.edges = {{10,100},{100,11},{12,101},{101,13}}; g.sequence = {10,11,12,13};
    const auto r = official_timing(ir); require(r.valid, r.error);
    require(r.cycles == 20 && r.cores[0].peak[1] == 100000, "capacity gate preserves local order globally");
    require(at(r.cores[0].timeline,12).start == 10, "WAR protects reused capacity");
    const auto &deps = r.cores[0].memory_dependencies;
    require(deps.size() == 1 && deps[0].source == 11 && deps[0].target == 12 &&
        deps[0].reused_bytes == 68928 && deps[0].kind == "WAR", "FIFO byte credits");
}
void internal_copy_and_empty() {
    auto ir = empty_scene(); auto &g = ir.tasks[0];
    g.ops = {{1,999,-1,4,"COPY_IN"}};
    g.tensors = {{2,61,0},{3,61,1}}; g.edges = {{2,1},{1,3}}; g.sequence = {1};
    auto r = official_timing(ir); require(r.valid && r.cycles == 2, "internal COPY duration and FIX pipe");
    require(r.cores[0].pipe_orders[4] == vector<i64>{1}, "do not infer pipe from COPY kind");
    r = official_timing(empty_scene(5)); require(r.valid && r.cycles == 0, "empty cores");
    ir.tasks[0].sequence.clear(); r = official_timing(ir); require(!r.valid, "reject incomplete sequence");
}
void large_timestamps() {
    auto ir = empty_scene(); auto &g = ir.tasks[0];
    const i64 base = (i64(1) << 53) + 4;
    g.ops = {{1,base,-1,2,"COMPUTE"},{2,4,-1,3,"COMPUTE"}};
    g.edges = {{1,2}}; g.sequence = {1,2};
    const auto r = official_timing(ir);
    require(r.valid && r.cycles == base + 4 && at(r.cores[0].timeline,2).start == base,
        "large integer compute timestamps");
}
void graph_api_preserves_spill_trace() {
    const string text = R"({"ops":[{"id":7,"op":"COMPUTE","pipe":"PIPE_V","cycles":10}],"tensors":[{"id":100,"pos":"UB","size":60}],"edges":[{"source":7,"target":100}]})";
    const Graph g(JsonParser(text).parse()); Solution s; s.core = {0}; s.order = {0};
    const auto before = official_spill_trace(g,s,2);
    const auto ir = official_execution_graph(g,s,2);
    const auto r = official_timing(ir);
    const auto after = official_spill_trace(g,s,2);
    require(before.valid && ir.valid && r.valid, r.error);
    require(official_spill_json(before) == official_spill_json(after), "timing changed legacy trace");
    require(r.route_bytes == before.route_bytes && r.spill_bytes == before.spill_bytes && r.cycles == 11, "Graph API physical IR");
}
void repeated_spill_incarnations() {
    for (bool later_writer : {false, true}) {
        // The long-lived 70 KB tensor is evicted by each independent 60 KB
        // branch. Its first reload feeds either a reader or a later writer;
        // its second reload feeds the final reader. Both remain legal below
        // UB's hard capacity, and exercise all three physical incarnations.
        ostringstream json;
        json << "{\"ops\":[";
        for (int i = 0; i < 7; ++i) {
            if (i) json << ',';
            json << "{\"id\":" << 10+i << ",\"op\":\"COMPUTE\",\"pipe\":\"PIPE_V\",\"cycles\":1}";
        }
        json << "],\"tensors\":[{\"id\":100,\"pos\":\"UB\",\"size\":70000},"
                "{\"id\":200,\"pos\":\"UB\",\"size\":60000},"
                "{\"id\":300,\"pos\":\"UB\",\"size\":60000}],\"edges\":[";
        const vector<pair<i64,i64>> edges{{10,100},later_writer ? make_pair<i64,i64>(13,100) : make_pair<i64,i64>(100,13),
                                         {100,16},{11,200},{200,12},{14,300},{300,15}};
        for (size_t i = 0; i < edges.size(); ++i) {
            if (i) json << ',';
            json << "{\"source\":" << edges[i].first << ",\"target\":" << edges[i].second << '}';
        }
        json << "]}";
        const Graph g(JsonParser(json.str()).parse()); Solution s;
        s.core.assign(g.size(), 0); s.order.resize(g.size()); iota(s.order.begin(), s.order.end(), 0);
        const auto trace = official_spill_trace(g,s,2);
        const auto ir = official_execution_graph(g,s,2);
        require(trace.valid && ir.valid, "repeated-spill fixture must be valid");
        vector<const OfficialSpillRecord *> versions;
        for (const auto &r : trace.records) if (r.core == 0 && r.tensor_id == 100) versions.push_back(&r);
        require(versions.size() == 2 && versions[0]->next_use_step < versions[1]->next_use_step,
                "fixture must cross two ordered reload boundaries");
        unordered_map<i64,int> step;
        for (int i = 0; i < int(trace.cores[0].sequence.size()); ++i) step[trace.cores[0].sequence[i].id] = i;
        // Independent oracle is the previous linear prefix rule. Verify every
        // original tensor-use edge, rather than reimplementing upper_bound.
        for (const auto &[source,target] : edges) {
            const i64 op = step.count(source) ? source : target;
            const i64 tid = step.count(source) ? target : source;
            i64 physical = tid;
            for (const auto &r : trace.records) if (r.core == 0 && r.tensor_id == tid) {
                if (step.at(op) < r.next_use_step) break;
                physical = r.to_tid;
            }
            const pair<i64,i64> expected = step.count(source) ? make_pair(op,physical) : make_pair(physical,op);
            require(find(ir.tasks[0].edges.begin(), ir.tasks[0].edges.end(), expected) != ir.tasks[0].edges.end(),
                    "physical incarnation differs from linear-prefix oracle");
            if (physical != tid)
                require(find(ir.tasks[0].edges.begin(), ir.tasks[0].edges.end(), make_pair(source,target)) == ir.tasks[0].edges.end(),
                        "obsolete original tensor-use edge remains");
        }
        const auto timing = official_timing(ir); require(timing.valid, timing.error);
        require(ir.route_bytes == trace.route_bytes && ir.spill_bytes == trace.spill_bytes,
                "incarnation lookup changed Step2 traffic");
    }
}
}
int main() {
    try {
        integer_sets(); shared_ddr(); cross_core_delay(); capacity_reuse(); internal_copy_and_empty(); large_timestamps(); graph_api_preserves_spill_trace(); repeated_spill_incarnations();
        cout << "official timing tests passed\n"; return 0;
    } catch (const exception &e) { cerr << e.what() << '\n'; return 1; }
}
