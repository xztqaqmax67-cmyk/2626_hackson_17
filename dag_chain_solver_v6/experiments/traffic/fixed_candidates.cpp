#include "chain.hpp"
#include "lifetime.hpp"
#include "tensor_neighborhood.hpp"

namespace {
double seconds(Clock::time_point t) { return chrono::duration<double>(Clock::now()-t).count(); }
string quote(const string &s) {
    string out = "\"";
    for (unsigned char c : s) {
        if (c == '\\' || c == '"') { out += '\\'; out += char(c); }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 32) throw runtime_error("unsupported control character");
        else out += char(c);
    }
    return out + '"';
}
void save(const fs::path &p, const string &s) {
    ofstream f(p, ios::binary); if (!f) throw runtime_error("cannot write output");
    f << s; if (!f) throw runtime_error("output failed");
}
Solution load_plan(const Graph &g, const fs::path &path, int k) {
    Json json = JsonParser(read_text(path)).parse();
    map<i64,int> ids;
    for (int v=0;v<g.size();++v) ids[g.ops[v].id]=v;
    Solution s; s.core.assign(g.size(),-1); s.label="baseline";
    vector<vector<int>> edges(g.size()); vector<int> degree(g.size());
    for (int v=0;v<g.size();++v) edges[v]=g.ops[v].succ;
    const auto &streams=json.at("core_schedules").array();
    if (int(streams.size())!=k) throw runtime_error("wrong core count");
    for (int c=0;c<k;++c) {
        int previous=-1;
        for (const auto &item:streams[c].array()) {
            int v=ids.at(item.integer());
            if (s.core[v]>=0) throw runtime_error("duplicate op");
            s.core[v]=c;
            if (previous>=0) edges[previous].push_back(v);
            previous=v;
        }
    }
    for (auto &row:edges) { Graph::unique_vec(row); for (int v:row) ++degree[v]; }
    priority_queue<int,vector<int>,greater<int>> ready;
    for (int v=0;v<g.size();++v) if (!degree[v]) ready.push(v);
    while (!ready.empty()) {
        int v=ready.top();ready.pop();s.order.push_back(v);
        for (int w:edges[v]) if (!--degree[w]) ready.push(w);
    }
    validate(g,s,k); return s;
}
}

int main(int argc,char **argv) {
    try {
        if (argc<5 || argc>7) throw runtime_error("usage: fixed_candidates graph.json cores saved_plan.json outdir [lifetime|memory|consumer|all] [limit_per_family]");
        auto started=Clock::now();
        Graph g(JsonParser(read_text(fs::u8path(argv[1]))).parse());
        int k=stoi(argv[2]);
        if (k<2 || k>5) throw runtime_error("cores must be 2..5");
        const auto base=load_plan(g,fs::u8path(argv[3]),k);
        const fs::path outdir=fs::u8path(argv[4]);
        fs::create_directories(outdir);
        string family=argc>=6 ? argv[5] : "lifetime";
        size_t limit=argc>=7 ? size_t(stoul(argv[6])) : 24;
        if (!limit || limit>128) throw runtime_error("limit must be 1..128");
        if (family!="lifetime" && family!="memory" && family!="consumer" && family!="all") throw runtime_error("unknown family");
        const double preprocessing=seconds(started);
        auto construction_started=Clock::now();
        vector<Solution> proposals{base};
        if (family=="lifetime" || family=="all") {
            auto candidates=dag_chain::lifetime_candidates(g,k,base,nullptr,limit);
            for (auto &s:candidates) proposals.push_back(move(s));
        }
        if (family=="memory" || family=="all") {
            size_t produced=0;
            for (const string &mode: {string("critical"),string("dfs"),string("breadth"),string("id")})
                for (int window:{8,32}) for (double memory:{1.0,4.0}) {
                    if (produced++>=limit) continue;
                    SeedOptions opt;opt.mode=mode;opt.window=window;opt.memory=memory;
                    auto s=operation_plan(g,k,Config{},opt,&base.core,nullptr);
                    s.label="memory_"+mode+"_w"+to_string(window)+"_m"+to_string(int(memory));
                    proposals.push_back(move(s));
                }
        }
        if (family=="consumer" || family=="all") {
            dag_chain::TensorNeighborhoods space(g);
            size_t produced=0;
            for (uint64_t i=0;i<space.count(k) && produced<limit;++i) {
                Solution s;
                if (space.propose(g,base,k,i,nullptr,s)) { proposals.push_back(move(s));++produced; }
            }
        }
        const double construction=seconds(construction_started);
        set<string> seen;
        double evaluation=0,serialization=0;
        size_t duplicate=0,index=0;
        ostringstream rows;
        rows<<setprecision(15);
        for (const auto &s:proposals) {
            validate(g,s,k);
            auto writing_started=Clock::now();
            const string plan=solution_json(g,s,k);
            serialization+=seconds(writing_started);
            if (!seen.insert(plan).second) { ++duplicate;continue; }
            auto evaluation_started=Clock::now();
            const auto r=dag_chain::refine_estimate(g,s,k);
            const double elapsed=seconds(evaluation_started); evaluation+=elapsed;
            if (index) rows<<',';
            rows<<"{\"id\":"<<index<<",\"label\":"<<quote(s.label)<<",\"valid\":"<<(r.valid?"true":"false")
                <<",\"evaluation_seconds\":"<<elapsed;
            if (r.valid) {
                const string name="candidate_"+to_string(index)+".json";
                writing_started=Clock::now();save(outdir/name,plan);serialization+=seconds(writing_started);
                rows<<",\"plan\":"<<quote(name)<<",\"estimated_cycles\":"<<r.cycles
                    <<",\"route_bytes\":"<<r.route<<",\"spill_bytes\":"<<r.spill
                    <<",\"copy_bytes\":"<<(r.route+r.spill);
            }
            rows<<'}';++index;
        }
        ostringstream report;
        report<<setprecision(15)<<"{\"experiment\":\"warm_start_fixed_candidates\",\"not_end_to_end_search\":true,\"family\":"<<quote(family)
              <<",\"cores\":"<<k<<",\"limit_per_family\":"<<limit<<",\"proposed\":"<<proposals.size()-1
              <<",\"duplicates\":"<<duplicate<<",\"preprocessing_seconds\":"<<preprocessing
              <<",\"construction_seconds\":"<<construction<<",\"evaluation_seconds\":"<<evaluation
              <<",\"serialization_seconds\":"<<serialization<<",\"process_seconds\":"<<seconds(started)
              <<",\"rows\":["<<rows.str()<<"]}\n";
        save(outdir/"screen.json",report.str());
        cout<<report.str();
    } catch (const exception &e) { cerr<<e.what()<<'\n';return 1; }
    return 0;
}
