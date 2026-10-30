// 对应 official_code/multicore_cut_evaluate_problem_3.py。
// 问题 3：复用场景 B 的执行规则，加入共享只读缓存及缓存带宽。
#include "multicore_cut_evaluate_problem_3.hpp"
#include "evaluation_validation.hpp"
namespace problem2 {
ReadOnlyCache::ReadOnlyCache(Id capacity, double bandwidth)
    : capacity_(capacity), bandwidth_(bandwidth) {
    require_integer(capacity, "cache_capacity_bytes");
    require_number(bandwidth, "cache_bandwidth_bytes_per_cycle", true);
}
std::pair<std::optional<Id>, Id> ReadOnlyCache::tensor_info(const PreparedTask &task, Id op) const {
    auto &row = task.ops.at(op);
    if (!is_copy(row)) {
        return {{}, 0};
    }
    const auto &ids = (row.at("op") == "COPY_IN" ? task.views.outputs : task.views.inputs).at(op);
    for (Id tid : ids) {
        const auto &tensor = task.tensors.at(tid);
        if (tensor.at("pos") != "DDR") {
            return {tensor.value("logical_tid", tid), field_id(tensor, "size")};
        }
    }
    return {{}, 0};
}
bool ReadOnlyCache::issue(const PreparedTask &task, Id core, Id op, double now) {
    auto [tid, size] = tensor_info(task, op);
    if (task.ops.at(op).at("op") != "COPY_IN" || !tid || size <= 0) {
        return false;
    }
    bool hit = sizes_.count(*tid);
    if (hit) {
        ++hits_;
        hit_bytes_ += size;
    } else {
        ++misses_;
        miss_bytes_ += size;
    }
    keys_[{core, op}] = *tid;
    paths_[{core, op}] = hit ? "CACHE_READ" : "DDR";
    events_.push_back({{"time", now},
                       {"event", hit ? "hit" : "miss"},
                       {"tensor_id", *tid},
                       {"size_bytes", size},
                       {"core_id", core},
                       {"op_id", op},
                       {"op", task.ops.at(op).at("op")}});
    return hit;
}
void ReadOnlyCache::retire(const PreparedTask &task, Id core, Id op, double now) {
    if (task.ops.at(op).at("op") != "COPY_IN") {
        return;
    }
    auto [tid, size] = tensor_info(task, op);
    if (!tid || size > capacity_ || sizes_.count(*tid)) {
        return;
    }
    Ids evicted;
    while (!entries_.empty() && used_ + size > capacity_) {
        auto [old, bytes] = entries_.front();
        entries_.pop_front();
        used_ -= bytes;
        sizes_.erase(old);
        evicted.push_back(old);
    }
    entries_.emplace_back(*tid, size);
    sizes_[*tid] = size;
    used_ += size;
    events_.push_back({{"time", now},
                       {"event", "insert"},
                       {"tensor_id", *tid},
                       {"size_bytes", size},
                       {"used_bytes", used_},
                       {"evicted_tensor_ids", evicted},
                       {"core_id", core},
                       {"op_id", op}});
}
void ReadOnlyCache::mark_ddr(Id core, Id op) {
    paths_.emplace(OpKey{core, op}, "DDR");
}
void ReadOnlyCache::annotate(Json &entry, Id core, Id op) const {
    OpKey item = {core, op};
    auto p = paths_.find(item);
    if (entry.at("op") == "COPY_IN" || entry.at("op") == "COPY_OUT") {
        entry["memory_path"] = p == paths_.end() ? "ON_CHIP" : p->second;
    }
    if (keys_.count(item)) {
        entry["cache_hit"] = p != paths_.end() && p->second == "CACHE_READ";
        entry["cache_tensor_id"] = keys_.at(item);
    }
}
void ReadOnlyCache::finish(Json &result) const {
    result["problem"] = 3;
    result["cache_mode"] = "read_only";
    result["cache_capacity_bytes"] = capacity_;
    result["cache_bandwidth_bytes_per_cycle"] = bandwidth_;
    result["cache_stats"] = {{"copy_in_hits", hits_},
                             {"copy_in_misses", misses_},
                             {"hit_bytes", hit_bytes_},
                             {"miss_bytes", miss_bytes_},
                             {"hits", hits_},
                             {"accesses", hits_ + misses_},
                             {"hit_rate", hit_bytes_ + miss_bytes_
                                              ? double(hit_bytes_) / (hit_bytes_ + miss_bytes_)
                                              : 0.}};
    result["cache_events"] = events_;
    result["cache_final_entries"] = array();
    for (auto [tid, size] : entries_) {
        result["cache_final_entries"].push_back({{"tensor_id", tid}, {"size_bytes", size}});
    }
    result["cache_used_bytes_final"] = used_;
}
Json evaluate_problem_3(const Json &graph, const Json &plan, double bandwidth,
                        const Capacity &capacity, double delay, Id cache_capacity,
                        double cache_bandwidth, Id max_iter) {
    ReadOnlyCache cache(cache_capacity, cache_bandwidth);
    return evaluate_scene_b_with_cache(graph, plan, bandwidth, capacity, delay, max_iter, &cache);
}
} // namespace problem2
