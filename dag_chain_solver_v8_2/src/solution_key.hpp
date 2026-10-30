#pragma once
#include "model.hpp"

namespace dag_chain {
namespace detail {
// 數值按值保存，方案向量只借用。保持原 tuple 的完整字典序和平手規則，
// 避免每次比較都複製整份核心分配／順序；回傳值不能比被借用的 Solution 活得久。
inline auto solution_key(double primary, double movement, const Solution &solution) {
    return tuple<double, double, const vector<int> &, const vector<int> &>{
        primary, movement, solution.core, solution.order};
}

inline auto solution_key(double primary, double movement, int core_budget,
                         const Solution &solution) {
    return tuple<double, double, int, const vector<int> &, const vector<int> &>{
        primary, movement, core_budget, solution.core, solution.order};
}
} // namespace detail
} // namespace dag_chain

