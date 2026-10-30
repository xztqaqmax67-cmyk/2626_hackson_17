// 对应 main.py。
// 命令行入口：校验参数，启动受时限约束的搜索进程，再导出最优检查点。
// npu_sched 使用同文件内的正式入口：直接调用原搜索，参数固定，只输出方案。
#include "search.hpp"
#include "batch.hpp"
#include "official.hpp"
#include "artifacts.hpp"
#include "submission_io.hpp"
#include "time_budget.hpp"
#include "utils.hpp"
#include "utf8_entry.hpp"
#include "official_code/evaluation_validation.hpp"
#include "official_code/stub_multicore_cut_and_schedule.hpp"
#include <csignal>
namespace problem2 {
#ifdef NPU_SCHED_ENTRY
int submission_main(int argc, char **argv) {
    // 以《提交要求文档》4.3 为准：单次运行 90 秒，包含读图、搜索、校验和写出。
#ifdef NPU_LOCAL_RUN
    constexpr double submission_time_limit = 30.0;
#else
    constexpr double submission_time_limit = 90.0;
#endif
    constexpr double finalization_reserve = 5.0;
    const double started = monotonic();
    const double finish_deadline = started + submission_time_limit;
    const double search_deadline = finish_deadline - finalization_reserve;
    int failure_code = 2;
    // 每条搜索线程有自己的截止时间；生成器会在交接时同步该值。
    // 正式入口不创建子进程，搜索本身仍按原有的限时规则收尾。
    struct RestoreDeadline {
        double previous = execution_deadline;
        ~RestoreDeadline() {
            execution_deadline = previous;
        }
    } restore;
    try {
        const auto args =
            parse_submission_arguments(std::vector<std::string>(argv + 1, argv + argc));
        if (args.version) {
#ifdef NPU_LOCAL_RUN
            std::cout << "npu_sched_local 1.1.0 (local only, 30 seconds, parallel C++17)\n";
#else
            std::cout << "npu_sched 1.1.0 (team 17, 90 seconds, parallel C++17)\n";
#endif
            return 0;
        }
        if (args.help) {
            std::cout << "Usage: npu_sched <graph.json> [-n 2..5] [-o plan.json]\n"
                         "       --cores=N and --output=PATH are also supported.\n";
            return 0;
        }
        failure_code = 3;
        execution_deadline = search_deadline;
        if (!fs::is_regular_file(args.input)) {
            throw std::invalid_argument("输入计算图不是可读取的文件");
        }
        const GraphInfo graph(read_json_strict(args.input));

        // 正式入口使用扣除输入处理和收尾预留后的全部搜索时间。
        // DEFAULT_CASE_TIME_LIMIT 仅用于开发入口，不限制正式入口的搜索预算。
        failure_code = 1;
        SearchOptions options;
        options.compact = true;
        options.export_results = false;
        options.time_limit = search_deadline - monotonic();
        if (options.time_limit <= 0) {
            std::cerr << "输入处理后剩余时间不足，未生成方案\n";
            return 5;
        }
        const auto plan = solve_in_memory(graph, args.cores, options);
        if (plan.is_null()) {
            std::cerr << "时限内未找到通过完整评估的方案\n";
            return 5;
        }
        // 搜索结束后启用收尾预算，避免在搜索截止时丢弃已经找到的合法方案。
        execution_deadline = finish_deadline;
        // 仍使用原有结构校验，不以简化估价代替官方规则的本地移植。
        validate_task_order(derive_multicore_plan(graph.graph, plan));
        poll_search_deadline();
        failure_code = 4;
        atomic_write(args.output, plan);
        return 0;
    } catch (const SubmissionUsageError &error) {
        std::cerr << safe_message(error.what()) << '\n';
        return 2;
    } catch (const TimeoutError &error) {
        std::cerr << safe_message(error.what()) << '\n';
        return 5;
    } catch (const NoFeasiblePlanError &error) {
        std::cerr << safe_message(error.what()) << '\n';
        return 5;
    } catch (const std::bad_alloc &) {
        std::cerr << "内存分配失败\n";
        return 1;
    } catch (const std::exception &error) {
        std::cerr << safe_message(error.what()) << '\n';
        return failure_code;
    }
}
#else
// 原开发入口完整保留，显式构建 problem2 时仍可使用批量、继承与调试功能。
namespace {
volatile std::sig_atomic_t interrupted = 0;
void interrupt_handler(int) {
    interrupted = 1;
}
bool within(const fs::path &path, const fs::path &directory) {
    auto a = fs::weakly_canonical(path), b = fs::weakly_canonical(directory);
    auto i = a.begin();
    for (auto j = b.begin(); j != b.end(); ++j, ++i) {
        if (i == a.end()) {
            return false;
        }
#ifdef _WIN32
        auto left = i->wstring(), right = j->wstring();
        std::transform(left.begin(), left.end(), left.begin(), ::towlower);
        std::transform(right.begin(), right.end(), right.begin(), ::towlower);
        if (left != right) {
            return false;
        }
#else
        if (*i != *j) {
            return false;
        }
#endif
    }
    return true;
}
struct TemporaryWork {
    fs::path path;
    bool owned = false;
    ~TemporaryWork() {
        if (!owned || path.empty()) {
            return;
        }
        std::error_code ignored;
        auto temp = fs::weakly_canonical(fs::temp_directory_path(), ignored);
        if (!ignored && path != temp && within(path, temp) &&
            path.filename().string().rfind("gmcm_problem2_", 0) == 0) {
            fs::remove_all(path, ignored);
        }
    }
};
} // namespace
int submission_main(int argc, char **argv) {
    double started = monotonic();
    try {
        std::map<std::string, std::string> flags;
        std::vector<std::string> positional;
        bool export_files = false;
        const std::set<std::string> names = {
            "--cores",          "--config",         "--output",
            "--output-dir",     "--time-limit",     "--feasibility-grace",
            "--search-profile", "--work-dir",       "--inherit-checkpoint",
            "--_deadline",      "--_hard-deadline", "--total-time-limit"};
        for (int i = 1; i < argc; ++i) {
            std::string value = argv[i];
            if (value == "--help" || value == "-h") {
                std::cout
                    << "Usage: problem2 GRAPH_OR_DIRECTORY -n CORES [--config FILE] [-o FILE | "
                       "--output-dir DIR]\n  --time-limit / --budget SECONDS   1 < seconds "
                       "<= 1200; single graph default 30\n  --total-time-limit SECONDS          "
                       "batch only; default/max 300\n  "
                       "--feasibility-grace SECONDS       0..120; default 0; batch requires 0\n  "
                       "--search-profile compact|full\n  --export-evaluation\n  --work-dir "
                       "EMPTY_DIR\n  --inherit-checkpoint FILE\n";
                return 0;
            }
            if (value == "--export-evaluation") {
                export_files = true;
                continue;
            }
            if (value == "-n") {
                value = "--cores";
            }
            if (value == "-o") {
                value = "--output";
            }
            if (value == "--budget") {
                value = "--time-limit";
            }
            if (!value.empty() && value.front() == '-') {
                auto equal = value.find('=');
                auto name = value.substr(0, equal);
                if (!names.count(name)) {
                    throw std::invalid_argument("unknown argument: " + name);
                }
                if (equal != std::string::npos) {
                    flags[name] = value.substr(equal + 1);
                } else {
                    if (++i >= argc) {
                        throw std::invalid_argument("missing value for " + name);
                    }
                    flags[name] = argv[i];
                }
            } else {
                positional.push_back(value);
            }
        }
        if (positional.size() != 1 || !flags.count("--cores")) {
            throw std::invalid_argument("需要一个计算图路径和 -n 核心数");
        }
        auto parse_int = [](const std::string &value) {
            std::size_t used;
            Id n = std::stoll(value, &used);
            if (used != value.size()) {
                throw std::invalid_argument("invalid integer: " + value);
            }
            return n;
        };
        auto parse_double = [](const std::string &value) {
            std::size_t used;
            double n = std::stod(value, &used);
            if (used != value.size()) {
                throw std::invalid_argument("invalid number: " + value);
            }
            return n;
        };
        Id cores = parse_int(flags.at("--cores"));
        double limit = flags.count("--time-limit") ? parse_double(flags.at("--time-limit"))
                                                   : DEFAULT_CASE_TIME_LIMIT,
               grace = flags.count("--feasibility-grace")
                           ? parse_double(flags.at("--feasibility-grace"))
                           : 0.;
        if (!std::isfinite(grace) || grace < 0 || grace > 120) {
            throw std::invalid_argument("首解延时必须在 [0, 120] 秒内");
        }
        if (cores < 2 || cores > 5 || !std::isfinite(limit) || limit <= 1 || limit > 1200) {
            throw std::invalid_argument("核心数必须为 2 至 5，时限必须在 (1, 1200] 秒内");
        }
        std::string profile =
            flags.count("--search-profile") ? flags.at("--search-profile") : "compact";
        if (profile != "compact" && profile != "full") {
            throw std::invalid_argument("search-profile must be compact or full");
        }
        fs::path graph = fs::absolute(path_from_utf8(positional[0])).lexically_normal(),
                 config =
                     fs::absolute(flags.count("--config")
                                      ? path_from_utf8(flags.at("--config"))
                                      : (fs::is_directory(graph) ? graph : graph.parent_path()) /
                                            "config.txt")
                         .lexically_normal(),
                 package = fs::weakly_canonical(path_from_utf8(__FILE__).parent_path());
        if (fs::is_directory(graph)) {
            if (flags.count("--output") || flags.count("--inherit-checkpoint") ||
                flags.count("--_deadline") || flags.count("--_hard-deadline") || grace != 0) {
                throw std::invalid_argument(
                    "批量模式使用 --output-dir，不接受单文件检查点、内部截止时间或首解延时");
            }
            if (!fs::is_regular_file(config)) {
                throw std::invalid_argument("配置文件不存在；请使用 --config 指定");
            }
            official::load_settings(config);
            BatchOptions batch;
            batch.cores = cores;
            batch.total_time_limit = flags.count("--total-time-limit")
                                         ? parse_double(flags.at("--total-time-limit"))
                                         : DEFAULT_BATCH_TIME_LIMIT;
            if (!std::isfinite(batch.total_time_limit) || batch.total_time_limit <= 1 ||
                batch.total_time_limit > 300) {
                throw std::invalid_argument("整批时限必须在 (1, 300] 秒内");
            }
            batch.deadline = started + batch.total_time_limit;
            batch.per_case_limit = flags.count("--time-limit") ? limit : 0;
            batch.search_profile = profile;
            batch.export_results = export_files;
            batch.output =
                fs::absolute(flags.count("--output-dir") ? path_from_utf8(flags.at("--output-dir"))
                                                         : graph / "results");
            if (within(batch.output, package) ||
                fs::weakly_canonical(batch.output) == fs::weakly_canonical(graph)) {
                throw std::invalid_argument("批量结果须放入独立输出目录，且不能写入源码目录");
            }
            TemporaryWork work;
            if (flags.count("--work-dir")) {
                work.path = fs::absolute(path_from_utf8(flags.at("--work-dir")));
                if (within(work.path, package) ||
                    (fs::exists(work.path) &&
                     (!fs::is_directory(work.path) || !fs::is_empty(work.path)))) {
                    throw std::invalid_argument("work-dir 必须是源码目录外的空目录");
                }
            } else {
                work.owned = true;
                auto temp = fs::weakly_canonical(fs::temp_directory_path());
                for (Id counter = 0;; ++counter) {
                    work.path =
                        temp / ("gmcm_problem2_batch_" +
                                std::to_string(
                                    std::chrono::steady_clock::now().time_since_epoch().count()) +
                                "_" + key(counter));
                    if (fs::create_directory(work.path)) {
                        break;
                    }
                }
            }
            if (within(batch.output, work.path) || within(work.path, batch.output)) {
                throw std::invalid_argument("批量工作目录和输出目录须分开保存");
            }
            batch.work = work.path;
            auto old_int = std::signal(SIGINT, interrupt_handler),
                 old_term = std::signal(SIGTERM, interrupt_handler);
            auto report = run_batch(fs::absolute(path_from_utf8(argv[0])), graph, config, batch,
                                    []() { return interrupted != 0; });
            std::signal(SIGINT, old_int);
            std::signal(SIGTERM, old_term);
            std::cout << report.dump() << '\n';
            return report.at("status") == "complete" ? 0 : interrupted ? 130 : 1;
        }
        if (flags.count("--total-time-limit")) {
            throw std::invalid_argument("--total-time-limit 仅适用于计算图目录");
        }
        if (!fs::is_regular_file(graph) || !fs::is_regular_file(config)) {
            throw std::invalid_argument("计算图或配置文件不存在；可使用 --config 指定配置");
        }
        SearchOptions options;
        options.time_limit = limit;
        options.feasibility_grace = grace;
        options.compact = profile == "compact";
        options.export_results = false;
        if (flags.count("--inherit-checkpoint")) {
            options.inherited_checkpoint =
                fs::absolute(path_from_utf8(flags.at("--inherit-checkpoint")));
        }
        if (flags.count("--_deadline")) {
            validate_graph(read_json_strict(graph));
            read_evaluation_config(config);
            double deadline = parse_double(flags.at("--_deadline")),
                   hard = parse_double(flags.at("--_hard-deadline")),
                   remaining = deadline - monotonic();
            double extension = std::max(0., hard - std::max(monotonic(), deadline));
            if (remaining <= 0 && extension <= 0) {
                throw TimeoutError("输入校验后剩余搜索时间不足，尚未产生完整方案");
            }
            options.time_limit = std::max(.001, remaining);
            options.feasibility_grace = extension;
            auto record =
                solve(graph, cores, config, path_from_utf8(flags.at("--work-dir")), options);
            return record.value("solution_available", false) ? 0 : 1;
        }
        if (flags.count("--output") && flags.count("--output-dir")) {
            throw std::invalid_argument("output and output-dir are mutually exclusive");
        }
        auto directory = flags.count("--output-dir") ? path_from_utf8(flags.at("--output-dir"))
                                                     : graph.parent_path();
        fs::path target = fs::absolute(flags.count("--output")
                                           ? path_from_utf8(flags.at("--output"))
                                           : directory / path_from_utf8(path_utf8(graph.stem()) +
                                                                        "_multicore_res.json"))
                              .lexically_normal();
        if (target == graph || target == config || within(target, package)) {
            throw std::invalid_argument("方案不能覆盖输入、配置或写入源码目录");
        }
        TemporaryWork work;
        if (flags.count("--work-dir")) {
            work.path = fs::absolute(path_from_utf8(flags.at("--work-dir"))).lexically_normal();
            if (within(work.path, package) ||
                (fs::exists(work.path) &&
                 (!fs::is_directory(work.path) || !fs::is_empty(work.path)))) {
                throw std::invalid_argument("work-dir 必须是源码目录外的空目录");
            }
            if (within(target, work.path)) {
                throw std::invalid_argument("最终方案与工作目录必须分开保存");
            }
            fs::create_directories(work.path);
        } else {
            work.owned = true;
            auto temp = fs::weakly_canonical(fs::temp_directory_path());
            for (Id counter = 0;; ++counter) {
                work.path =
                    temp /
                    ("gmcm_problem2_" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                     "_" + key(counter));
                if (fs::create_directory(work.path)) {
                    break;
                }
            }
        }
        double deadline = started + limit - std::min(2., limit * .02), hard = deadline + grace;
        auto decimal = [](double value) {
            std::ostringstream out;
            out.precision(17);
            out << value;
            return out.str();
        };
        std::vector<std::string> command = {path_utf8(graph),
                                            "-n",
                                            key(cores),
                                            "--config",
                                            path_utf8(config),
                                            "--time-limit",
                                            decimal(limit),
                                            "--work-dir",
                                            path_utf8(work.path),
                                            "--_deadline",
                                            decimal(deadline),
                                            "--_hard-deadline",
                                            decimal(hard),
                                            "--feasibility-grace",
                                            decimal(grace),
                                            "--search-profile",
                                            profile};
        if (!options.inherited_checkpoint.empty()) {
            command.push_back("--inherit-checkpoint");
            command.push_back(path_utf8(options.inherited_checkpoint));
        }
        auto old_int = std::signal(SIGINT, interrupt_handler),
             old_term = std::signal(SIGTERM, interrupt_handler);
        ChildProcess child(fs::absolute(path_from_utf8(argv[0])), command,
                           work.path / "search.log");
        auto waited = wait_for_search(child, deadline, hard, work.path / "best_checkpoint.json",
                                      []() { return interrupted != 0; });
        if (waited.must_kill) {
            child.stop();
        }
        int returncode = child.poll().value_or(1);
        std::signal(SIGINT, old_int);
        std::signal(SIGTERM, old_term);
        double search_seconds = monotonic() - started;
        Json result;
        try {
            if (!fs::is_regular_file(work.path / "best_checkpoint.json")) {
                throw std::runtime_error("时限内未得到经过官方评价的完整方案，未写出方案文件");
            }
            auto checkpoint = read_json_strict(work.path / "best_checkpoint.json");
            const auto &plan = checkpoint.at("plan");
            result = checkpoint.at("official_result");
            double makespan = result.at("makespan").get<double>();
            if (Id(plan.at("core_schedules").size()) != cores ||
                field_id(result, "num_cores") != cores ||
                result.value("scene", std::string("A")) != "B" || !std::isfinite(makespan) ||
                makespan < 0 || checkpoint.at("makespan") != result.at("makespan")) {
                throw std::invalid_argument("检查点的场景、核心数或目标值不一致");
            }
            for (auto i = plan.at("node_to_subgraph").begin();
                 i != plan.at("node_to_subgraph").end(); ++i) {
                if (key(std::stoll(i.key())) != i.key()) {
                    throw std::invalid_argument("操作 ID 必须为十进制整数字符串");
                }
            }
            validate_task_order(derive_multicore_plan(read_json_strict(graph), plan));
            atomic_write(target, plan);
            if (export_files) {
                export_evaluation(target.parent_path(), path_utf8(graph.filename()),
                                  path_utf8(target.filename()), result);
            }
        } catch (...) {
            auto log = read_text(work.path / "search.log");
            if (log.size() > 2500) {
                log = log.substr(log.size() - 2500);
            }
            if (!log.empty()) {
                std::cerr << safe_message(log) << '\n';
            }
            throw;
        }
        bool timed_out = waited.must_kill && !waited.interrupted,
             abnormal = returncode != 0 && !timed_out && !waited.interrupted;
        Json report = {{"problem", 2},
                       {"input", path_utf8(graph.filename())},
                       {"config", path_utf8(config.filename())},
                       {"output", path_utf8(target.filename())},
                       {"cores", cores},
                       {"makespan", result.at("makespan")},
                       {"data_movement_bytes", result.at("data_movement_bytes")},
                       {"search_seconds", search_seconds},
                       {"total_seconds", monotonic() - started},
                       {"time_limit", limit},
                       {"timed_out", timed_out},
                       {"feasibility_grace_seconds", grace},
                       {"feasibility_extension_used", waited.extension_used},
                       {"search_profile", profile},
                       {"worker_returncode", returncode},
                       {"status", waited.interrupted ? "interrupted"
                                  : abnormal         ? "complete_with_error"
                                                     : "complete"}};
        atomic_write(work.path / "submission_report.json", report);
        std::cout << report.dump() << '\n';
        if (abnormal) {
            auto log = read_text(work.path / "search.log");
            std::cerr << safe_message(log.size() > 2500 ? log.substr(log.size() - 2500) : log)
                      << '\n';
        }
        return waited.interrupted ? 130 : abnormal ? 1 : 0;
    } catch (const std::exception &error) {
        std::cerr << "[SOLVER ERROR] " << safe_message(error.what()) << '\n';
        return 1;
    }
}
#endif
} // namespace problem2
#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
    return problem2::utf8_entry(argc, argv, problem2::submission_main);
}
#else
int main(int argc, char **argv) {
    return problem2::utf8_entry(argc, argv, problem2::submission_main);
}
#endif
