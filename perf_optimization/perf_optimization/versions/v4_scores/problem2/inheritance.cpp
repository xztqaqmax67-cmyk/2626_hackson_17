// 对应 inheritance.py。
// 验证检查点来源，将较低核数方案嵌入更多核心，并要求重新评分。
#include "inheritance.hpp"
#include "utils.hpp"
#include "official_manifest.hpp"
namespace problem2 {
Json instance_fingerprint(const fs::path &graph, const fs::path &config) {
    // 使用编译时记录的源码指纹，避免把运行目录或源码安装位置写入身份信息。
    return {{"graph_sha256", sha256(graph)},
            {"config_sha256", sha256(config)},
            {"official_code", Json::parse(official_manifest)}};
}
Plan lift_plan(const GraphInfo &g, const Plan &p, Id cores) {
    if (cores < p.ncores || cores > 5) {
        throw std::invalid_argument("lifting requires source cores <= target cores <= 5");
    }
    p.validate(g);
    auto lifted = p;
    lifted.ncores = cores;
    lifted.validate(g);
    return lifted;
}
std::pair<Plan, Json> load_inherited(const GraphInfo &g, const fs::path &path, Id cores,
                                     const Json &instance, bool same) {
    auto payload = read_json_strict(path);
    // 允许继承本次移植所依据的原版 Python 检查点，但必须逐项匹配已记录的
    // 原版源码、输入和配置指纹。调用方随后重新评估，不直接信任旧评分。
    Json original_instance = instance;
    original_instance["official_code"] = Json::parse(original_official_manifest);
    auto source_instance = payload.value("problem_instance", Json());
    if (source_instance != instance && source_instance != original_instance) {
        throw std::invalid_argument(
            "inherited checkpoint input/config/official fingerprint mismatch or missing");
    }
    const auto &result = payload.at("official_result");
    const auto &value = payload.at("makespan");
    Id source = Id(payload.at("plan").at("core_schedules").size());
    if (result.value("scene", std::string()) != "B" || result.at("num_cores") != source ||
        source < 1 || source > cores || (source == cores && !same) || !value.is_number() ||
        !std::isfinite(value.get<double>()) || value.get<double>() < 0 ||
        result.at("makespan") != value) {
        throw std::invalid_argument("inconsistent or non-smaller-core inherited checkpoint");
    }
    auto plan = Plan::from_dict(g, payload.at("plan"));
    Json metadata = {
        {"status", "pending"},
        {"checkpoint", path_utf8(path.filename())},
        {"checkpoint_sha256", sha256(path)},
        {"source_cores", source},
        {"source_makespan", value},
        {"source_added_copy_bytes", result.at("data_movement_bytes").at("added_copy_bytes")},
        {"source_checkpoint_seconds", payload.value("elapsed_seconds", Json())}};
    return {lift_plan(g, plan, cores), metadata};
}
} // namespace problem2
