#include "chain.hpp"
#include "plan_loader.hpp"

int main(int argc, char **argv) {
    try {
        if (argc < 3) throw runtime_error("usage: chain_diagnostics graph.json cores [plan.json]");
        Graph g(JsonParser(read_text(fs::u8path(argv[1]))).parse());
        int k = stoi(argv[2]);
        if (argc == 5 && string(argv[3]) == "--evaluate-plan") {
            auto s = load_singleton_plan(g, fs::u8path(argv[4]), k);
            auto r = dag_chain::refine_estimate(g, s, k);
            cout << setprecision(15) << "{\"valid\":" << (r.valid ? "true" : "false")
                 << ",\"estimated_cycles\":" << r.cycles << ",\"route\":" << r.route
                 << ",\"spill\":" << r.spill << "}\n";
            return 0;
        }
        auto start = Clock::now();
        unsigned extensions = 7;
        if (argc == 6 && string(argv[4]) == "--extensions")
            extensions = unsigned(stoul(argv[5]));
        auto r = dag_chain::solve(g, k, false, extensions);
        double seconds = chrono::duration<double>(Clock::now() - start).count();
        cout << setprecision(15) << "{\"estimated_cycles\":" << r.refined.cycles
             << ",\"estimated_route_bytes\":" << r.refined.route
             << ",\"estimated_spill_bytes\":" << r.refined.spill
             << ",\"coarse_sketch\":" << r.estimate.sketch
             << ",\"coarse_spill\":" << r.estimate.spill
             << ",\"candidates\":" << r.candidates
             << ",\"refined_candidates\":" << r.refined_candidates
             << ",\"region_proposals\":" << r.region_proposals
             << ",\"slack_proposals\":" << r.slack_proposals
             << ",\"spill_proposals\":" << r.spill_proposals
             << ",\"effective_cores\":" << r.generated_for_cores
             << ",\"label\":\"" << r.solution.label << "\",\"seconds\":" << seconds << "}\n";
        if (argc > 3) {
            auto path = fs::u8path(argv[3]);
            if (!path.parent_path().empty()) fs::create_directories(path.parent_path());
            ofstream out(path);
            if (!out) throw runtime_error("cannot create diagnostic plan");
            out << solution_json(g, r.solution, k);
            if (!out) throw runtime_error("cannot write diagnostic plan");
        }
    } catch (const exception &e) {
        cerr << e.what() << '\n';
        return 1;
    }
    return 0;
}
