#pragma once
#include "../common.hpp"
namespace problem2 {
struct Step3SchedulingError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
Id op_duration(const Json &op, const GraphViews &views, const Records &tensors, double bandwidth);
bool uses_ddr_bandwidth(const Json &op, const GraphViews &views, const Records &tensors);
Json step3_simulation(const Json &graph, const Capacity &capacity, double bandwidth,
                      Id max_iter = 100000);
struct PreparedTask {
    Json graph, step3;
    Json raw_graph, spill_result;
    Ids raw_sequence;
    std::map<Id, Id> raw_op_subgraph;
    Ids sequence, subgraphs;
    Records ops, tensors;
    GraphViews views;
    std::map<Id, Id> sequence_position, op_subgraph;
    std::map<std::string, Ids> pipe_ops;
};
PreparedTask prepare_step3_execution(const Json &graph, const Capacity &capacity, double bandwidth);

// DDR 公平共享：work 表示独占带宽时剩余的周期数。
// 用同一个模板服务核内 ID 和跨核 (core, op) ID，避免两份公式发生漂移。
template <class Key> void advance_ddr_work(std::map<Key, double> &work, double &last, double now) {
    double elapsed = now - last;
    while (elapsed > 1e-9) {
        std::vector<Key> active;
        double minimum = std::numeric_limits<double>::infinity();
        for (auto [id, value] : work) {
            if (value > 1e-9) {
                active.push_back(id);
                minimum = std::min(minimum, value);
            }
        }
        if (active.empty()) {
            break;
        }
        double finish = minimum * active.size();
        if (finish >= elapsed - 1e-9) {
            for (auto id : active) {
                work[id] = std::max(0., work[id] - elapsed / active.size());
            }
            break;
        }
        for (auto id : active) {
            work[id] = std::max(0., work[id] - minimum);
        }
        elapsed -= finish;
    }
    last = now;
}
template <class Key>
std::map<Key, double> project_ddr_ends(const std::map<Key, double> &work, double now) {
    std::vector<std::pair<double, Key>> ordered;
    for (auto [id, value] : work) {
        ordered.emplace_back(std::max(0., value), id);
    }
    std::sort(ordered.begin(), ordered.end());
    std::map<Key, double> projected;
    double cursor = now, previous = 0;
    std::size_t active = ordered.size(), i = 0;
    while (i < ordered.size()) {
        double value = ordered[i].first;
        cursor += (value - previous) * active;
        std::size_t j = i;
        while (j < ordered.size() && std::abs(ordered[j].first - value) <= 1e-9) {
            projected[ordered[j++].second] = std::ceil(cursor - 1e-9);
        }
        active -= j - i;
        previous = value;
        i = j;
    }
    return projected;
}
} // namespace problem2
