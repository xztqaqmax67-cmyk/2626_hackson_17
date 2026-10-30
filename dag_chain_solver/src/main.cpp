#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include "chain.hpp"

namespace {
struct Options {
    fs::path input, output;
    int cores = 4;
    bool version = false;
};
Options options(const vector<string> &args) {
    Options o;
    set<string> seen;
    for (size_t i = 1; i < args.size(); ++i) {
        string name = args[i], value;
        size_t eq = name.find('=');
        bool attached = eq != string::npos;
        if (attached) {
            value = name.substr(eq + 1);
            name.resize(eq);
        }
        if (name == "--version") {
            if (attached || args.size() != 2)
                throw invalid_argument("--version takes no arguments");
            o.version = true;
            return o;
        }
        if (name == "-n" || name == "--cores" || name == "-o" || name == "--output") {
            string canonical = (name == "-n" || name == "--cores") ? "cores" : "output";
            if (!seen.insert(canonical).second)
                throw invalid_argument("duplicate option: " + name);
            if (!attached) {
                if (++i >= args.size())
                    throw invalid_argument("missing value: " + name);
                value = args[i];
            }
            if (value.empty())
                throw invalid_argument("empty value: " + name);
            if (canonical == "cores") {
                if (value.size() != 1 || value[0] < '2' || value[0] > '5')
                    throw invalid_argument("cores must be 2..5");
                o.cores = value[0] - '0';
            } else {
                if (value[0] == '-')
                    throw invalid_argument("missing output path");
                o.output = fs::u8path(value);
            }
        } else if (!name.empty() && name[0] == '-')
            throw invalid_argument("unknown option: " + name);
        else {
            if (!o.input.empty())
                throw invalid_argument("one graph path is required");
            o.input = fs::u8path(args[i]);
        }
    }
    if (o.input.empty())
        throw invalid_argument("usage: npu_sched <graph.json> -n <2..5> -o <plan.json>");
    if (o.output.empty())
        o.output =
            o.input.parent_path() / fs::u8path(o.input.stem().u8string() + "_multicore_res.json");
    error_code ec;
    const auto input_path = fs::absolute(o.input).lexically_normal();
    const auto output_path = fs::absolute(o.output).lexically_normal();
    const bool same_existing_file = fs::equivalent(input_path, output_path, ec) && !ec;
    if (input_path == output_path || same_existing_file)
        throw invalid_argument("output must not replace the input graph");
    return o;
}
void save(const fs::path &path, const string &text) {
    if (!path.parent_path().empty())
        fs::create_directories(path.parent_path());
    auto temporary = path;
    temporary += ".tmp";
    {
        ofstream out(temporary, ios::binary | ios::trunc);
        if (!out)
            throw runtime_error("cannot create output temporary file");
        out.write(text.data(), streamsize(text.size()));
        out.flush();
        if (!out)
            throw runtime_error("cannot write output");
        out.close();
        if (out.fail())
            throw runtime_error("cannot close output");
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw runtime_error("cannot finalize output");
#else
    fs::rename(temporary, path);
#endif
}
int run(const vector<string> &args) {
    Options opt;
    try {
        opt = options(args);
    } catch (const exception &e) {
        cerr << e.what() << '\n';
        return 2;
    }
    if (opt.version) {
        cout << "dag-chain-solver 0.1.0\n";
        return 0;
    }
    unique_ptr<Graph> graph;
    try {
        graph = make_unique<Graph>(JsonParser(read_text(opt.input)).parse());
    } catch (const exception &e) {
        cerr << "input: " << e.what() << '\n';
        return 3;
    }
    dag_chain::Result result;
    try {
#ifdef CHAIN_BASELINE
        result = dag_chain::solve(*graph, opt.cores, true);
#else
        result = dag_chain::solve(*graph, opt.cores);
#endif
        validate(*graph, result.solution, opt.cores);
    } catch (const exception &e) {
        cerr << "no solution: " << e.what() << '\n';
        return 5;
    }
    try {
        save(opt.output, solution_json(*graph, result.solution, opt.cores));
    } catch (const exception &e) {
        cerr << "output: " << e.what() << '\n';
        return 4;
    }
    return 0;
}
} // namespace
#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
    try {
        vector<string> args;
        for (int i = 0; i < argc; ++i) {
            int n = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
            if (n <= 0)
                throw runtime_error("invalid command line encoding");
            string s(size_t(n), '\0');
            WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, s.data(), n, nullptr, nullptr);
            s.pop_back();
            args.push_back(move(s));
        }
        return run(args);
    } catch (const exception &e) {
        cerr << e.what() << '\n';
        return 1;
    }
}
#else
int main(int argc, char **argv) {
    try {
        return run(vector<string>(argv, argv + argc));
    } catch (const exception &e) {
        cerr << e.what() << '\n';
        return 1;
    }
}
#endif
