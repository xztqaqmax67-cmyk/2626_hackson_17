// 对应 artifacts.py。
// 导出标准方案、官方评估结果和时间线，并从已验证检查点恢复文件。
#include "artifacts.hpp"
#include "utils.hpp"
#include "official_code/contest_io.hpp"
namespace problem2 {
std::string export_evaluation(const fs::path &dir, const std::string &input,
                              const std::string &plan, const Json &result) {
    auto stem = path_utf8(path_from_utf8(input).stem());
    auto trace = format_trace_json(input, result);
    auto standard = result;
    standard["input_graph"] = input;
    standard["input_plan"] = plan;
    atomic_write(dir / path_from_utf8(stem + "_problem_2_res.json"), standard);
    atomic_write(dir / path_from_utf8(stem + "_problem_2_trace.json"), trace + "\n");
    atomic_write(dir / path_from_utf8(stem + "_problem_2_log.txt"),
                 format_multicore_result_log(input, standard));
    return trace;
}
Json recover(const fs::path &dir, const std::string &name, Id cores) {
    auto payload = read_json(dir / "best_checkpoint.json");
    if (payload.empty()) {
        return Json();
    }
    const auto &result = payload.at("official_result");
    const auto &plan = payload.at("plan");
    double makespan = result.at("makespan").get<double>();
    if (result.value("scene", std::string()) != "B" || !std::isfinite(makespan) || makespan < 0 ||
        field_id(result, "num_cores") != cores || Id(plan.at("core_schedules").size()) != cores ||
        result.at("makespan") != payload.at("makespan")) {
        throw std::invalid_argument("inconsistent checkpoint");
    }
    atomic_write(dir / "plan.json", plan);
    atomic_write(dir / path_from_utf8(name + "_multicore_res.json"), plan);
    atomic_write(dir / "official_result.json", result);
    auto trace = export_evaluation(dir, name + ".json", name + "_multicore_res.json", result);
    atomic_write(dir / "trace.json", trace);
    return payload;
}
} // namespace problem2
