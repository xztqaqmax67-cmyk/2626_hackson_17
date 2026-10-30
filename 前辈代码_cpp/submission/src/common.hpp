#pragma once
// 公共数据类型。算法内部用明确的整数 ID、邻接表；JSON 仅承载题目的记录格式。
#include "json.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace problem2 {
using Json = nlohmann::ordered_json;
using Id = std::int64_t;
using Ids = std::vector<Id>;
using IdSet = std::set<Id>;
using Adjacency = std::map<Id, IdSet>;
using Records = std::map<Id, Json>;
using Capacity = std::map<std::string, Id>;
namespace fs = std::filesystem;
// JSON、命令行及日志统一使用 UTF-8；文件系统路径由标准库转换为平台原生编码。
inline fs::path path_from_utf8(std::string_view text) {
    return fs::u8path(text.begin(), text.end());
}
inline std::string path_utf8(const fs::path &path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}
// Hackathon 确认 PIPE_FIX 是独立流水线；保持原四条的优先顺序，新增项放在末尾。
// 所有核内/多核模拟、搜索估算及时间线导出共享此表，每条仍只允许一个在飞指令。
inline const std::array<std::string, 5> PIPES = {"PIPE_MTE2", "PIPE_MTE3", "PIPE_M", "PIPE_V",
                                                 "PIPE_FIX"};
inline constexpr double DEFAULT_CASE_TIME_LIMIT = 25.0;
inline constexpr double DEFAULT_BATCH_TIME_LIMIT = 300.0;
inline std::string key(Id id) {
    return std::to_string(id);
}
inline Id number(const Json &value) {
    return value.get<Id>();
}
inline Id field_id(const Json &row, const char *field = "id") {
    return number(row.at(field));
}
inline std::string field_string(const Json &row, const char *field) {
    return row.at(field).get<std::string>();
}
inline IdSet as_set(const Ids &values) {
    return {values.begin(), values.end()};
}
template <class T> std::vector<T> as_vector(const std::set<T> &values) {
    return {values.begin(), values.end()};
}
template <class T> void append(std::vector<T> &target, const std::vector<T> &source) {
    target.insert(target.end(), source.begin(), source.end());
}
inline void append_json(Json &target, const Json &source) {
    for (const auto &row : source) {
        target.push_back(row);
    }
}
inline Records index_records(const Json &rows) {
    Records result;
    for (const auto &row : rows) {
        result[field_id(row)] = row;
    }
    return result;
}
inline IdSet record_ids(const Records &rows) {
    IdSet result;
    for (const auto &[id, row] : rows) {
        result.insert(id);
    }
    return result;
}
// These hooks count deterministic operations; their legacy argument is unused.
struct TimeoutError : std::runtime_error { using std::runtime_error::runtime_error; };
inline std::size_t work_visits = 0;
inline constexpr std::size_t WORK_LIMIT = 150000;
inline void check_deadline(double = 0) {
    if (++work_visits > WORK_LIMIT) throw TimeoutError("deterministic work limit");
}
inline void poll_search_deadline() { check_deadline(); }
struct Settings {
    Capacity capacity;
    double bandwidth = 0;
    double cross_core_copy_delay = 0;
};
struct GraphViews {
    std::map<Id, Ids> inputs, outputs;
    Adjacency preds, succs, producers, consumers;
};
GraphViews build_graph_views(const Json &ops, const Json &edges);
inline Id copy_duration(Id size, double bandwidth) {
    return std::max<Id>(1, static_cast<Id>(std::ceil(size / bandwidth)));
}
inline Id max_record_id(const Json &graph) {
    Id result = 0;
    for (auto field : {"ops", "tensors"}) {
        for (const auto &row : graph.at(field)) {
            result = std::max(result, field_id(row));
        }
    }
    return result;
}
inline bool is_copy(const Json &op) {
    return op.at("op") == "COPY_IN" || op.at("op") == "COPY_OUT";
}
inline Json edge(Id source, Id target) {
    return {{"source", source}, {"target", target}, {"data_size", 0}};
}
inline Json object() {
    return Json::object();
}
inline Json array() {
    return Json::array();
}
template <class T> Json id_map_json(const std::map<Id, T> &values) {
    Json j = object();
    for (const auto &[k, v] : values) {
        j[key(k)] = v;
    }
    return j;
}
} // namespace problem2
