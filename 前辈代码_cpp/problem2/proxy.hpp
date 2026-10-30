#pragma once
#include "core.hpp"
namespace problem2 {
// Development-selectable candidate screening. Full official scoring still decides
// acceptance. The default "legacy" search path does not construct this index.
struct ProxyIndex;
struct ProxyMetrics {
    double sketch = 0, route = 0, spill = 0, pipe_bound = 0, event_sketch = 0;
    bool memory_warning = false;
    double score(const std::string &mode, double bandwidth) const;
    Json to_json() const;
};
bool valid_screening_proxy(const std::string &mode);
class ScreeningProxy {
    const GraphInfo *graph_;
    std::shared_ptr<ProxyIndex> index_;
public:
    explicit ScreeningProxy(const GraphInfo &graph);
    ProxyMetrics evaluate(const Plan &plan, const Settings &settings, bool events = false) const;
};
} // namespace problem2
