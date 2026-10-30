// 对应 official_code/singlecore_evaluate.py。
// 把全部非 COPY 算子放到一个核心，使用官方场景 A 规则评分。
#include "singlecore_evaluate.hpp"
#include "multicore_cut_evaluate_problem_1.hpp"
namespace problem2 {
Json build_singlecore_plan(const Json &graph) {
    IdSet eligible;
    for (const auto &op : graph.at("ops")) {
        if (!is_copy(op)) {
            eligible.insert(field_id(op));
        }
    }
    Json mapping = object();
    for (Id op : eligible) {
        mapping[key(op)] = 0;
    }
    return {{"node_to_subgraph", mapping},
            {"core_schedules", Json::array({eligible.empty() ? array() : Json::array({0})})}};
}
Json evaluate_singlecore(const Json &graph, double bandwidth, const Capacity &capacity,
                         double cross_wait, double same_wait) {
    auto result = evaluate_scene_a(graph, build_singlecore_plan(graph), bandwidth, capacity,
                                   cross_wait, same_wait);
    result["execution_mode"] = "singlecore";
    result["input_plan"] = nullptr;
    return result;
}
} // namespace problem2
