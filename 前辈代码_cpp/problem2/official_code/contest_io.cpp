// 对应 official_code/contest_io.py。
// 官方入口的公共参数解析、配置读取、结果日志与时间线输出。
#include "contest_io.hpp"
#include "evaluation_validation.hpp"
#include "multicore_cut_evaluate_problem_1.hpp"
#include "multicore_cut_evaluate_problem_2.hpp"
#include "multicore_cut_evaluate_problem_3.hpp"
#include "singlecore_evaluate.hpp"
#include "stub_multicore_cut_and_schedule.hpp"
namespace problem2 {
namespace {
std::string scalar(const Json &value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_number_float() && value.get<double>() == std::floor(value.get<double>())) {
        return std::to_string(value.get<Id>());
    }
    return value.dump();
}
struct Arguments {
    std::vector<std::string> positional;
    std::map<std::string, std::string> flags;
    bool help = false;
};
Arguments parse(int argc, char **argv, const std::set<std::string> &allowed) {
    Arguments a;
    for (int i = 1; i < argc; ++i) {
        std::string v = argv[i];
        if (v == "--help" || v == "-h") {
            a.help = true;
            continue;
        }
        if (!v.empty() && v[0] == '-') {
            if (v == "-o") {
                v = "--output";
            }
            if (v == "-n") {
                v = "--num-cores";
            }
            auto equal = v.find('=');
            std::string name = v.substr(0, equal);
            if (!allowed.count(name)) {
                throw std::invalid_argument("unknown argument: " + name);
            }
            if (equal != std::string::npos) {
                a.flags[name] = v.substr(equal + 1);
            } else {
                if (++i >= argc) {
                    throw std::invalid_argument("missing argument value: " + v);
                }
                a.flags[name] = argv[i];
            }
        } else {
            a.positional.push_back(v);
        }
    }
    return a;
}
fs::path option_path(const Arguments &a, const std::string &name, const fs::path &fallback) {
    auto it = a.flags.find(name);
    return it == a.flags.end() ? fallback : path_from_utf8(it->second);
}
} // namespace
std::string format_data_movement_log(const Json &result) {
    auto m = result.value("data_movement_bytes", object()),
         c = result.value("cache_stats", object());
    std::string text =
        "data movement: scheduled_copy_bytes=" + scalar(m.value("scheduled_copy_bytes", Json(0))) +
        ", physical_ddr_bytes=" +
        scalar(m.value("physical_ddr_bytes", m.value("scheduled_copy_bytes", Json(0))));
    if (!c.empty()) {
        text += ", cache_hits=" + scalar(c.value("hits", Json(0))) + "/" +
                scalar(c.value("accesses", Json(0))) +
                ", cache_hit_bytes=" + scalar(c.value("hit_bytes", Json(0)));
    }
    return text;
}
std::string format_multicore_result_log(const std::string &filename, const Json &result) {
    std::string text = "input_graph: " + filename +
                       "\nscene: " + result.value("scene", std::string("A")) +
                       "\nmakespan: " + scalar(result.at("makespan")) +
                       "\nnum_cores: " + scalar(result.at("num_cores")) + "\n" +
                       format_data_movement_log(result) + "\n";
    for (const auto &c : result.value("per_core_timeline", array())) {
        double end = 0;
        auto ops = c.value("ops", array());
        for (const auto &op : ops) {
            end = std::max(end, op.at("end").get<double>());
        }
        text += "core " + scalar(c.at("core_id")) + ": ops=" + std::to_string(ops.size()) +
                ", end=" + scalar(end) + "\n";
    }
    return text;
}
std::string format_trace_json(const std::string &filename, const Json &result) {
    Json events = array();
    std::map<std::string, Id> pipe_index;
    for (std::size_t i = 0; i < PIPES.size(); ++i) {
        pipe_index[PIPES[i]] = Id(i);
    }
    for (const auto &c : result.value("per_core_timeline", array())) {
        Id core = field_id(c, "core_id"), pid = 1000 + core;
        events.push_back({{"name", "process_name"},
                          {"ph", "M"},
                          {"pid", pid},
                          {"tid", 0},
                          {"args", {{"name", "Core " + key(core)}}}});
        for (auto pipe : PIPES) {
            events.push_back({{"name", "thread_name"},
                              {"ph", "M"},
                              {"pid", pid},
                              {"tid", pipe_index.at(pipe)},
                              {"args", {{"name", pipe}}}});
        }
    }
    for (const auto &c : result.value("per_core_timeline", array())) {
        Id core = field_id(c, "core_id"), pid = 1000 + core;
        auto spans = c.value("subgraphs", array());
        if (spans.empty()) {
            Json groups = object();
            for (const auto &op : c.value("ops", array())) {
                auto sg = op.value("subgraph_id", op.value("task_id", Json()));
                if (sg.is_null()) {
                    continue;
                }
                auto k = scalar(sg);
                if (!groups.contains(k)) {
                    groups[k] = {{"subgraph_id", sg},
                                 {"task_id", op.value("task_id", Json())},
                                 {"start", op.at("start")},
                                 {"end", op.at("end")}};
                }
                groups[k]["start"] = std::min(groups[k]["start"], op.at("start"));
                groups[k]["end"] = std::max(groups[k]["end"], op.at("end"));
            }
            for (const auto &row : groups) {
                spans.push_back(row);
            }
        }
        for (const auto &span : spans) {
            Id start = field_id(span, "start"), end = field_id(span, "end");
            if (end < start) {
                continue;
            }
            auto sg = span.value("subgraph_id", span.value("task_id", Json()));
            events.push_back({{"name", "Subgraph " + scalar(sg)},
                              {"cat", "SUBGRAPH"},
                              {"ph", "X"},
                              {"ts", start},
                              {"dur", end - start},
                              {"pid", pid},
                              {"tid", 100},
                              {"args",
                               {{"core_id", core},
                                {"task_id", span.value("task_id", Json())},
                                {"subgraph_id", sg},
                                {"start", start},
                                {"end", end}}}});
        }
        for (const auto &op : c.value("ops", array())) {
            auto pipe = field_string(op, "pipe");
            events.push_back({{"name", field_string(op, "op") + " #" + scalar(op.at("op_id"))},
                              {"cat", pipe},
                              {"ph", "X"},
                              {"ts", op.at("start")},
                              {"dur", op.at("duration")},
                              {"pid", pid},
                              {"tid", pipe_index.count(pipe) ? pipe_index.at(pipe) : 9},
                              {"args",
                               {{"core_id", core},
                                {"task_id", op.value("task_id", Json())},
                                {"subgraph_id", op.value("subgraph_id", Json())},
                                {"op_id", op.at("op_id")},
                                {"pipe", pipe}}}});
        }
    }
    return Json{
        {"traceEvents", events},
        {"displayTimeUnit", "cycles"},
        {"otherData",
         {{"input_graph", filename},
          {"scene", result.value("scene", Json())},
          {"makespan", result.at("makespan")},
          {"track_layout", "one Perfetto process group per core; pipe threads are named PIPE_*"},
          {"subgraph_spans", "X events named Subgraph <id> carry start/end in args"}}}}
        .dump(2);
}
int run_problem_cli(int problem, int argc, char **argv) {
    try {
        auto args = parse(argc, argv, {"--config", "--output", "--trace-output", "--log-output"});
        if (args.help) {
            std::cout << "Usage: evaluator GRAPH [PLAN] [--config FILE] [-o FILE] [--trace-output "
                         "FILE] [--log-output FILE]\n";
            return 0;
        }
        if (args.positional.empty() || args.positional.size() > std::size_t(problem == 0 ? 1 : 2)) {
            throw std::invalid_argument("expected graph and optional plan path");
        }
        fs::path graph_path = path_from_utf8(args.positional[0]), stem = graph_path;
        stem.replace_extension();
        std::string name = problem == 0 ? "singlecore" : "problem_" + std::to_string(problem);
        auto suffix = [&](std::string s) {
            auto path = stem;
            path += s;
            return path;
        };
        auto config = option_path(args, "--config", graph_path.parent_path() / "config.txt"),
             output = option_path(args, "--output", suffix("_" + name + "_res.json")),
             trace = option_path(args, "--trace-output", suffix("_" + name + "_trace.json")),
             log = option_path(args, "--log-output", suffix("_" + name + "_log.txt"));
        auto graph = read_json_strict(graph_path);
        auto settings = read_evaluation_config(config);
        auto plan_path = args.positional.size() > 1 ? path_from_utf8(args.positional[1])
                                                    : suffix("_multicore_res.json");
        Json result;
        if (problem == 0 || problem == 1) {
            auto waits = read_required_settings(
                config, "multicore_scene_a",
                {"task_cross_core_wait_cycles", "task_same_core_wait_cycles"});
            if (problem == 0) {
                result = evaluate_singlecore(graph, settings.bandwidth, settings.capacity,
                                             waits.at("task_cross_core_wait_cycles"),
                                             waits.at("task_same_core_wait_cycles"));
            } else {
                result =
                    evaluate_scene_a(graph, read_json_strict(plan_path), settings.bandwidth,
                                     settings.capacity, waits.at("task_cross_core_wait_cycles"),
                                     waits.at("task_same_core_wait_cycles"));
            }
        } else {
            auto delay = read_required_settings(config, "multicore_scene_b",
                                                {"cross_core_copy_delay_cycles"})
                             .at("cross_core_copy_delay_cycles");
            if (problem == 2) {
                result = evaluate_scene_b(graph, read_json_strict(plan_path), settings.bandwidth,
                                          settings.capacity, delay);
            } else if (problem == 3) {
                auto cache = read_required_settings(
                    config, "problem_3",
                    {"cache_capacity_bytes", "cache_bandwidth_bytes_per_cycle"});
                result =
                    evaluate_problem_3(graph, read_json_strict(plan_path), settings.bandwidth,
                                       settings.capacity, delay, cache.at("cache_capacity_bytes"),
                                       cache.at("cache_bandwidth_bytes_per_cycle"));
            } else {
                throw std::invalid_argument("problem must be 1, 2 or 3");
            }
        }
        auto trace_text = format_trace_json(path_utf8(graph_path.filename()), result);
        result["input_graph"] = path_utf8(graph_path.filename());
        if (problem != 0) {
            result["input_plan"] = path_utf8(plan_path.filename());
        }
        write_text(output, result.dump(2) + "\n");
        write_text(trace, trace_text + "\n");
        write_text(log, format_multicore_result_log(path_utf8(graph_path.filename()), result));
        std::cout << format_data_movement_log(result) << "\nOK: " << name
                  << " makespan=" << scalar(result.at("makespan"))
                  << "; result=" << path_utf8(output) << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "[EVALUATION ERROR] " << error.what() << '\n';
        return 1;
    }
}
int run_multicore_stub_cli(int argc, char **argv) {
    try {
        auto args = parse(
            argc, argv,
            {"--num-cores", "--seed", "--min-subgraph-size", "--max-subgraph-size", "--output"});
        if (args.help) {
            std::cout << "Usage: stub GRAPH [-n CORES] [--seed N] [--min-subgraph-size N] "
                         "[--max-subgraph-size N] [-o FILE]\n";
            return 0;
        }
        if (args.positional.size() != 1) {
            throw std::invalid_argument("expected graph path");
        }
        auto integer = [&](std::string name, Id fallback) {
            return args.flags.count(name) ? std::stoll(args.flags.at(name)) : fallback;
        };
        fs::path path = path_from_utf8(args.positional[0]), stem = path;
        stem.replace_extension();
        stem += "_multicore_res.json";
        Id seed = integer("--seed", 0);
        auto plan = generate_multicore_plan(
            read_json_strict(path), integer("--num-cores", 4),
            seed < 0 ? std::uint64_t(-(seed + 1)) + 1 : std::uint64_t(seed),
            integer("--min-subgraph-size", 50), integer("--max-subgraph-size", 100));
        auto output = option_path(args, "--output", stem);
        write_text(output, plan.dump(2) + "\n");
        std::cout << "OK: output=" << path_utf8(output) << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "[EVALUATION ERROR] " << error.what() << '\n';
        return 1;
    }
}
} // namespace problem2
