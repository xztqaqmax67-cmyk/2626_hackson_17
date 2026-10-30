#pragma once
#include "core.hpp"
namespace problem2 {
// 新标准换页估价用于候选粗排，完整评分负责最终接受。
struct ProxyIndex;
struct ProxyMetrics {
    double sketch = 0, route = 0, spill = 0;
    bool memory_warning = false;
    double score(double bandwidth) const;
};
class ScreeningProxy {
    const GraphInfo *graph_;
    std::shared_ptr<const ProxyIndex> index_;

public:
    explicit ScreeningProxy(const GraphInfo &graph);
    ProxyMetrics evaluate(const Plan &plan, const Settings &settings) const;
};
} // namespace problem2
