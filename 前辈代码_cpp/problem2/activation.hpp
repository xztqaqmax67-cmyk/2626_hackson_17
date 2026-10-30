#pragma once
#include "pipeline.hpp"
namespace problem2 {
// 在依赖允许的范围内把部分工作迁入空闲核心；由搜索主流程验证是否确实更优。
NamedPlans activation_plans(const GraphInfo &graph, const Plan &plan, const Settings &settings,
                            double deadline = 0, Id limit = 8);
} // namespace problem2
