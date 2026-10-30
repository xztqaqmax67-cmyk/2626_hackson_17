#include "official_timing.hpp"
#include "plan_loader.hpp"

int main(int argc, char **argv) {
    try {
        if (argc != 4)
            throw runtime_error("usage: timing_diagnostics graph.json plan.json timing.json");
        Graph graph(JsonParser(read_text(fs::u8path(argv[1]))).parse());
        const auto plan_path = fs::u8path(argv[2]);
        const auto plan = JsonParser(read_text(plan_path)).parse();
        const auto &mapping = plan.at("node_to_subgraph");
        if (mapping.type != Json::Object || mapping.fields.size() != size_t(graph.size()))
            throw runtime_error("timing diagnostic accepts complete singleton plans only");
        for (const auto &op : graph.ops)
            if (mapping.at(to_string(op.id)).integer() != op.id)
                throw runtime_error("timing diagnostic accepts singleton subgraphs only");
        const int cores = int(plan.at("core_schedules").array().size());
        const auto solution = load_singleton_plan(graph, plan_path, cores);
        const auto result = dag_chain::official_timing(graph, solution, cores);
        const auto target = fs::u8path(argv[3]);
        if (!target.parent_path().empty()) fs::create_directories(target.parent_path());
        ofstream output(target, ios::binary);
        output << dag_chain::official_timing_json(result);
        if (!output) throw runtime_error("cannot write timing trace");
        if (!result.valid) { cerr << result.error << '\n'; return 2; }
        return 0;
    } catch (const exception &error) {
        cerr << error.what() << '\n';
        return 1;
    }
}
