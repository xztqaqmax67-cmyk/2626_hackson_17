#pragma once
#include "schedule_step3.hpp"
namespace problem2 {
// Cache 命中不会改变 FIFO 顺序。只在 COPY_IN 完成后填充 Cache。
class ReadOnlyCache {
    using OpKey = std::pair<Id, Id>;
    Id capacity_, used_ = 0;
    double bandwidth_;
    std::deque<std::pair<Id, Id>> entries_;
    std::map<Id, Id> sizes_;
    std::map<OpKey, std::string> paths_;
    std::map<OpKey, Id> keys_;
    Json events_ = array();
    Id hits_ = 0, misses_ = 0, hit_bytes_ = 0, miss_bytes_ = 0;
    std::pair<std::optional<Id>, Id> tensor_info(const PreparedTask &task, Id op) const;

  public:
    ReadOnlyCache(Id capacity, double bandwidth);
    bool issue(const PreparedTask &task, Id core, Id op, double now);
    void retire(const PreparedTask &task, Id core, Id op, double now);
    void mark_ddr(Id core, Id op);
    void annotate(Json &entry, Id core, Id op) const;
    void finish(Json &result) const;
    double bandwidth() const {
        return bandwidth_;
    }
};
Json evaluate_problem_3(const Json &graph, const Json &plan, double bandwidth,
                        const Capacity &capacity, double delay, Id cache_capacity,
                        double cache_bandwidth, Id max_iter = 1000000);
Json evaluate_scene_b_with_cache(const Json &graph, const Json &plan, double bandwidth,
                                 const Capacity &capacity, double delay, Id max_iter,
                                 ReadOnlyCache *cache);
} // namespace problem2
