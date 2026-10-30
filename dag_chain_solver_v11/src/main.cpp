#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include "chain.hpp"
#include "anytime.hpp"
#include "search_limits.hpp"
#include "spill_feedback.hpp"
#include "communication.hpp"
#include "v10_search.hpp"
#include "annealing_search.hpp"

namespace {
struct Options {
    fs::path input, output;
    int cores = 4;
    bool version = false;
    dag_chain::AnnealingStage ablation = dag_chain::AnnealingStage::annealed;
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
#ifdef CHAIN_EXPERIMENT
        if (name == "--ablation") {
            if(!seen.insert("ablation").second)throw invalid_argument("duplicate --ablation");
            if(!attached){if(++i>=args.size())throw invalid_argument("missing --ablation value");value=args[i];}
            if(value=="baseline")o.ablation=dag_chain::AnnealingStage::baseline;
            else if(value=="greedy_only")o.ablation=dag_chain::AnnealingStage::greedy_only;
            else if(value=="annealed")o.ablation=dag_chain::AnnealingStage::annealed;
            else throw invalid_argument("unknown ablation variant");
            continue;
        }
#endif
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
    // Shared wall-time origin includes parsing. Stages never reset this clock.
    [[maybe_unused]] const auto generation_started = Clock::now();
    struct PhaseTiming { string name; double limit, end, duration; bool expired; };
    vector<PhaseTiming> phases;
    auto elapsed=[&](){return chrono::duration<double>(Clock::now()-generation_started).count();};
    using namespace dag_chain::search_limits;
#ifdef CHAIN_QUICK_TEST
    constexpr double budget_scale=quick_test_seconds/generation_target_seconds;
#else
    constexpr double budget_scale=1.0;
#endif
    auto make_deadline=[&](double limit){Deadline d;d.start=generation_started;
        d.limit=limit*budget_scale;d.enabled=generation_budget_enabled;return d;};
    auto note=[&](const string &name,const Deadline &d,Clock::time_point start,bool stopped=false){
        phases.push_back({name,d.limit,elapsed(),chrono::duration<double>(Clock::now()-start).count(),stopped||d.expired()});};
    Options opt;
    try {
        opt = options(args);
    } catch (const exception &e) {
        cerr << e.what() << '\n';
        return 2;
    }
    if (opt.version) {
        cout << "dag-chain-solver 0.11.1\n";
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
        vector<Solution> feedback_seeds;
        auto phase_started=Clock::now();
        auto seed_deadline=make_deadline(generation_seed_stop);
#ifdef CHAIN_QUICK_TEST
        const double generation_budget = dag_chain::search_limits::quick_test_seconds;
#else
        const double generation_budget = dag_chain::search_limits::total_seconds;
#endif
        if constexpr (generation_budget_enabled)
            result=dag_chain::solve(*graph,opt.cores,false,7,&feedback_seeds,&seed_deadline);
        else if constexpr (dag_chain::search_limits::enable_timed_search)
            result = dag_chain::solve_for_budget(*graph, opt.cores, generation_started, generation_budget);
        else
            result = dag_chain::solve(*graph, opt.cores, false, 7, &feedback_seeds);
        note("seed",seed_deadline,phase_started);
#endif
#ifndef CHAIN_BASELINE
        // 所有種子共用有界回饋配額；精確 Step 2 搬運與估計 cycles 都保護原方案。
        if constexpr (dag_chain::search_limits::feedback_enabled) {
            auto started=Clock::now();auto deadline=make_deadline(generation_feedback_stop);
            dag_chain::FeedbackStats statistics;
            result = dag_chain::improve_spill_order(*graph, opt.cores, std::move(result),
                                                   &statistics, &feedback_seeds,nullptr,
                                                   generation_budget_enabled?&deadline:nullptr);
            note("feedback",deadline,started,statistics.deadline_reached);
            // 診斷只送 stderr，正式方案 JSON 與 CLI 契約不變。
            cerr << "feedback_stats " << dag_chain::feedback_stats_json(statistics) << '\n';
        }
        if constexpr (dag_chain::search_limits::communication_enabled) {
            auto started=Clock::now();auto deadline=make_deadline(generation_communication_stop);
            dag_chain::CommunicationStats statistics;
            result = dag_chain::improve_communication_groups(*graph, opt.cores,
                std::move(result), &statistics,generation_budget_enabled?&deadline:nullptr);
            note("communication",deadline,started,statistics.deadline_reached);
            cerr << "communication_stats " << dag_chain::communication_stats_json(statistics) << '\n';
        }
        {
            auto started=Clock::now();auto deadline=make_deadline(generation_v10_stop);
            dag_chain::V10Stats statistics;
            result = dag_chain::improve_v10(*graph,opt.cores,std::move(result),dag_chain::V10Stage::exact,
                                          &statistics,generation_budget_enabled?&deadline:nullptr);
            note("v10",deadline,started,statistics.deadline_reached);
            cerr << "v10_stats " << dag_chain::v10_stats_json(statistics) << '\n';
        }
        {
            auto started=Clock::now();auto deadline=make_deadline(generation_search_stop);
            dag_chain::AnnealingSearchStats statistics;
            result=dag_chain::improve_annealing(*graph,opt.cores,std::move(result),opt.ablation,&statistics,
                                              generation_budget_enabled?&deadline:nullptr);
            note("annealing",deadline,started,statistics.deadline_reached);
            cerr<<"annealing_stats "<<dag_chain::annealing_search_stats_json(statistics)<<'\n';
            if(!statistics.baseline_valid)throw runtime_error("final exact baseline invalid: "+statistics.error);
        }
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
#ifndef CHAIN_BASELINE
    bool stopped=false;for(const auto &p:phases)stopped=stopped||p.expired;
    cerr<<setprecision(17)<<"budget_stats {\"enabled\":"<<(generation_budget_enabled?"true":"false")
        <<",\"target_seconds\":"<<generation_target_seconds*budget_scale
        <<",\"search_stop_seconds\":"<<generation_search_stop*budget_scale
        <<",\"elapsed_seconds\":"<<elapsed()<<",\"deadline_reached\":"<<(stopped?"true":"false")
        <<",\"phases\":[";
    for(size_t i=0;i<phases.size();++i){const auto &p=phases[i];if(i)cerr<<',';
        cerr<<"{\"name\":\""<<p.name<<"\",\"limit_seconds\":"<<p.limit<<",\"elapsed_end_seconds\":"<<p.end
            <<",\"seconds\":"<<p.duration<<",\"deadline_reached\":"<<(p.expired?"true":"false")<<'}';}
    cerr<<"]}\n";
#endif
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
