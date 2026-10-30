// Independently test the real CLI boundary with a deliberately failing solver.
// Compile this file alone; do not link chain_algorithm or change the production CLI.
#define main submission_main_for_test
#define wmain submission_wmain_for_test
#include "../src/main.cpp"
#undef main
#undef wmain

namespace {
enum class Failure { none, no_candidate, internal, allocation, invalid_solution };
Failure failure = Failure::none;
void require(bool condition, const string &message) {
    if (!condition) throw runtime_error(message);
}
}

namespace dag_chain {
Result solve_fixed_work(const Graph &graph, int, TimedSearchStats *) {
    switch (failure) {
    case Failure::no_candidate: throw NoCandidate();
    case Failure::internal: throw runtime_error("injected internal failure");
    case Failure::allocation: throw bad_alloc();
    case Failure::invalid_solution: return {};
    case Failure::none: break;
    }
    Result result;
    result.solution.core.assign(graph.size(), 0);
    result.solution.order = graph.topo;
    return result;
}
}

int main(int argc, char **argv) {
    try {
        require(argc == 2, "pass an output directory for this test");
        const auto root = fs::absolute(fs::u8path(argv[1]));
        fs::create_directories(root);
        fs::path work;
        for (unsigned i = 0; i < 1024; ++i) {
            auto candidate = root / ("cli_error_tests_" + to_string(i));
            if (fs::create_directory(candidate)) { work = move(candidate); break; }
        }
        require(!work.empty(), "cannot reserve test directory");
        const auto source = work / "input.json", output = work / "plan.json";
        const string graph = R"({"tensors":[],"ops":[{"id":11,"op":"ADD","pipe":"PIPE_V","cycles":4}],"edges":[]})";
        { ofstream out(source, ios::binary); out << graph; }
        const vector<string> args{"npu_sched", source.u8string(), "-n", "2", "-o", output.u8string()};
        for (const auto &test : vector<pair<Failure, int>>{{Failure::no_candidate, 5},
                {Failure::internal, 1}, {Failure::allocation, 1}, {Failure::invalid_solution, 1}}) {
            failure = test.first;
            require(run(args) == test.second, "wrong classification of solver failure");
            require(!fs::exists(output), "failure wrote an output plan");
        }
        failure = Failure::none;
        require(run(args) == 0, "successful stub failed");
        require(fs::is_regular_file(output), "success did not write a plan");
        require(read_text(source) == graph, "input changed");
        auto input_collision = output; input_collision += ".tmp";
        auto file_collision = output; file_collision += ".tmp.0";
        auto directory_collision = output; directory_collision += ".tmp.1";
        { ofstream out(input_collision, ios::binary); out << graph; }
        { ofstream out(file_collision, ios::binary); out << "preserved file"; }
        fs::create_directory(directory_collision);
        { ofstream out(directory_collision / "plan.json", ios::binary); out << "preserved directory"; }
        auto collision_args = args;
        collision_args[1] = input_collision.u8string();
        require(run(collision_args) == 0, "temporary path collision prevented valid output");
        require(read_text(input_collision) == graph, "temporary path overwrote input");
        require(read_text(file_collision) == "preserved file", "existing temporary file changed");
        require(read_text(directory_collision / "plan.json") == "preserved directory",
                "existing temporary directory changed");
        auto reservation = output; reservation += ".tmp.2";
        require(!fs::exists(reservation), "successful output left a temporary reservation");
        collision_args.back() = directory_collision.u8string();
        require(run(collision_args) == 4, "directory target was not an output error");
        auto failed_reservation = directory_collision; failed_reservation += ".tmp.0";
        require(!fs::exists(failed_reservation), "failed output left a temporary reservation");
        cout << "CLI error classification passed: NoCandidate=5; internal/allocation/invalid solution=1\n";
        cout << "Temporary input/file/directory collision and cleanup checks passed\n";
        return 0;
    } catch (const exception &error) {
        cerr << error.what() << '\n';
        return 1;
    }
}
