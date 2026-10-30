#include "batch.hpp"
#include "time_budget.hpp"
#include "utils.hpp"
namespace problem2 {
Json run_batch(const fs::path &executable, const fs::path &input_directory, const fs::path &config,
               const BatchOptions &options, const std::function<bool()> &interrupted) {
    std::vector<fs::path> graphs;
    for (const auto &entry : fs::directory_iterator(input_directory)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            graphs.push_back(entry.path());
        }
    }
    std::sort(graphs.begin(), graphs.end());
    if (graphs.empty()) {
        throw std::invalid_argument("输入目录中没有 JSON 计算图");
    }
    fs::create_directories(options.output);
    fs::create_directories(options.work);
    Json report = {{"problem", 2},
                   {"scene", "B"},
                   {"cores", options.cores},
                   {"total_time_limit", options.total_time_limit},
                   {"case_count", graphs.size()},
                   {"completed", 0},
                   {"cases", array()},
                   {"status", "running"}};
    const double started = options.deadline - options.total_time_limit;
    auto save = [&]() {
        report["total_seconds"] = monotonic() - started;
        atomic_write(options.output / "batch_report.json", report);
    };
    save();
    for (std::size_t index = 0; index < graphs.size(); ++index) {
        const auto &graph = graphs[index];
        Json row = {{"input", path_utf8(graph.filename())}, {"solution_available", false}};
        // 为汇总落盘留出总预算的一小部分。某例提前结束，余时自动分给后续用例。
        double remaining =
            options.deadline - monotonic() - std::min(3., options.total_time_limit * .02);
        double slot = remaining / double(graphs.size() - index);
        if (options.per_case_limit > 0) {
            slot = std::min(slot, options.per_case_limit);
        }
        slot = std::min(slot, 1200.);
        if (interrupted() || slot <= 1.1) {
            row["status"] = interrupted() ? "interrupted" : "budget_exhausted";
            report["cases"].push_back(row);
            continue;
        }
        auto work = options.work / ("case_" + std::to_string(index));
        auto target =
            options.output / path_from_utf8(path_utf8(graph.stem()) + "_multicore_res.json");
        // 单例入口在搜索进程退出后还会读取检查点、校验输入并原子写出方案。
        // 给这部分工作保留独立额度，外层不能在搜索刚结束时就杀掉负责导出的入口。
        const double export_reserve = std::min(10., slot * .25);
        const double search_limit = std::max(1.01, slot - export_reserve);
        std::vector<std::string> arguments = {path_utf8(graph),
                                              "-n",
                                              key(options.cores),
                                              "--config",
                                              path_utf8(config),
                                              "--output",
                                              path_utf8(target),
                                              "--work-dir",
                                              path_utf8(work),
                                              "--time-limit",
                                              std::to_string(search_limit),
                                              "--feasibility-grace",
                                              "0",
                                              "--search-profile",
                                              options.search_profile};
        if (options.export_results) {
            arguments.push_back("--export-evaluation");
        }
        std::cerr << "[batch " << index + 1 << '/' << graphs.size() << "] "
                  << path_utf8(graph.filename()) << "; budget=" << slot << "s\n";
        double began = monotonic();
        ChildProcess process(executable, arguments,
                             options.work / ("case_" + std::to_string(index) + ".log"));
        // 子入口负责保存已验证方案；外层进程同时约束整个单例，避免延时叠加。
        const double deadline = std::min(options.deadline - .1, began + slot);
        while (!process.poll() && !interrupted() && monotonic() < deadline) {
            process.wait(std::min(.1, std::max(0., deadline - monotonic())));
        }
        bool killed = !process.poll();
        if (killed) {
            process.stop();
        }
        row["seconds"] = monotonic() - began;
        row["allocated_seconds"] = slot;
        row["search_time_limit"] = search_limit;
        row["returncode"] = process.poll().value_or(1);
        row["status"] = interrupted() ? "interrupted" : killed ? "time_limit" : "failed";
        // 只认本次工作目录中新生成的报告，不能把上一次残留的输出误算作成功。
        if (fs::is_regular_file(work / "submission_report.json") && fs::is_regular_file(target)) {
            auto completed = read_json_strict(work / "submission_report.json");
            if (completed.at("input") == path_utf8(graph.filename()) &&
                completed.at("cores") == options.cores) {
                row["solution_available"] = true;
                row["status"] = completed.at("status");
                row["output"] = path_utf8(target.filename());
                row["makespan"] = completed.at("makespan");
                row["data_movement_bytes"] = completed.at("data_movement_bytes");
                report["completed"] = field_id(report, "completed") + 1;
            }
        }
        report["cases"].push_back(row);
        save();
    }
    report["status"] = interrupted()                                        ? "interrupted"
                       : field_id(report, "completed") == Id(graphs.size()) ? "complete"
                                                                            : "incomplete";
    save();
    return report;
}
} // namespace problem2
