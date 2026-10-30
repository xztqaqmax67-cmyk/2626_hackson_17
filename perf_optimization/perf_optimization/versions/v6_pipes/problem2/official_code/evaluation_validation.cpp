// 对应 official_code/evaluation_validation.py。
// 检查输入格式、参数范围、依赖环和子图执行顺序。
#include "evaluation_validation.hpp"

namespace problem2 {
void require_integer(const Json &value, const std::string &name, Id minimum) {
    if (!value.is_number_integer() ||
        (value.is_number_unsigned() && value.get<std::uint64_t>() > std::uint64_t(INT64_MAX)) ||
        number(value) < minimum) {
        throw EvaluationValidationError(name + " must be an integer >= " + key(minimum));
    }
}
void require_number(const Json &value, const std::string &name, bool positive) {
    if (!value.is_number()) {
        throw EvaluationValidationError(name + " must be a number");
    }
    double x = value.get<double>();
    if (!std::isfinite(x) || x < 0 || (positive && x == 0)) {
        throw EvaluationValidationError(name + " must be finite and nonnegative");
    }
}
void validate_capacity(const Json &capacity) {
    if (!capacity.is_object() || capacity.size() != 2 || !capacity.contains("L1") ||
        !capacity.contains("UB")) {
        throw EvaluationValidationError("capacity must contain exactly L1 and UB");
    }
    for (auto name : {"L1", "UB"}) {
        require_integer(capacity.at(name), std::string("capacity.") + name);
    }
}
static std::string trim(std::string text) {
    auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}
Capacity read_integer_settings(const fs::path &path, const std::string &section,
                               const std::set<std::string> &keys) {
    Capacity result;
    if (path.empty() || !fs::exists(path)) {
        return result;
    }
    std::ifstream input(path);
    std::string line, active;
    int line_no = 0;
    const std::set<std::string> sections = {"capacity", "bandwidth", "multicore_scene_b"};
    while (std::getline(input, line)) {
        ++line_no;
        line = trim(line.substr(0, line.find('#')));
        if (line.empty()) {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            active = trim(line.substr(1, line.size() - 2));
            std::transform(active.begin(), active.end(), active.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (!sections.count(active)) {
                throw EvaluationValidationError("unknown section [" + active + "]");
            }
            continue;
        }
        std::istringstream row(line);
        std::string name, value, extra;
        if (!(row >> name >> value) || (row >> extra) || active.empty()) {
            throw EvaluationValidationError("invalid configuration line " +
                                            std::to_string(line_no));
        }
        if (active != section) {
            continue;
        }
        if (!keys.count(name) || result.count(name)) {
            throw EvaluationValidationError("invalid/duplicate setting: " + name);
        }
        try {
            std::size_t used;
            Id parsed = std::stoll(value, &used);
            if (used != value.size()) {
                throw std::invalid_argument("integer");
            }
            result[name] = parsed;
        } catch (const std::exception &) {
            throw EvaluationValidationError(name + " must be an integer");
        }
    }
    return result;
}
Capacity read_required_settings(const fs::path &path, const std::string &section,
                                const std::set<std::string> &keys) {
    auto result = read_integer_settings(path, section, keys);
    for (const auto &name : keys) {
        if (!result.count(name)) {
            throw EvaluationValidationError("[" + section + "] must define " + name);
        }
        require_integer(result.at(name), name);
    }
    return result;
}
Capacity read_capacity_config(const fs::path &path) {
    return read_required_settings(path, "capacity", {"L1", "UB"});
}
Id read_bandwidth_config(const fs::path &path) {
    auto b = read_required_settings(path, "bandwidth", {"bandwidth"}).at("bandwidth");
    require_number(b, "bandwidth", true);
    return b;
}
Settings read_evaluation_config(const fs::path &path) {
    if (path.empty() || !fs::exists(path)) {
        throw EvaluationValidationError("configuration file not found: " + path.string());
    }
    return {read_capacity_config(path), double(read_bandwidth_config(path)), 0};
}
void validate_parameters(double bandwidth, const Capacity &capacity, Id max_iter) {
    require_number(bandwidth, "bandwidth", true);
    validate_capacity(Json(capacity));
    require_integer(max_iter, "max_iter", 1);
}
void check_acyclic(const Ids &nodes, const std::vector<std::tuple<Id, Id, std::string>> &edges,
                   const std::string &context) {
    Adjacency successors;
    std::map<Id, Id> degree;
    for (Id id : nodes) {
        successors[id];
        degree[id] = 0;
    }
    for (const auto &[source, target, reason] : edges) {
        if (!degree.count(source) || !degree.count(target)) {
            throw EvaluationValidationError(context + ": unknown edge endpoint");
        }
        if (successors[source].insert(target).second) {
            ++degree[target];
        }
    }
    std::deque<Id> ready;
    for (Id id : nodes) {
        if (!degree[id]) {
            ready.push_back(id);
        }
    }
    std::size_t count = 0;
    while (!ready.empty()) {
        Id id = ready.front();
        ready.pop_front();
        ++count;
        for (Id target : successors[id]) {
            if (--degree[target] == 0) {
                ready.push_back(target);
            }
        }
    }
    if (count != degree.size()) {
        throw EvaluationValidationError(context + ": dependency cycle");
    }
}
void validate_graph(const Json &graph) {
    if (!graph.is_object()) {
        throw EvaluationValidationError("graph must be an object");
    }
    for (auto field : {"ops", "tensors", "edges"}) {
        if (!graph.contains(field) || !graph.at(field).is_array()) {
            throw EvaluationValidationError(std::string("graph.") + field + " must be a list");
        }
    }
    IdSet ids, ops;
    for (auto field : {"ops", "tensors"}) {
        for (const auto &item : graph.at(field)) {
            if (!item.is_object()) {
                throw EvaluationValidationError("graph entry must be an object");
            }
            require_integer(item.value("id", Json()), "id");
            Id id = field_id(item);
            if (!ids.insert(id).second) {
                throw EvaluationValidationError("duplicate op/tensor id: " + key(id));
            }
            if (std::string(field) == "tensors") {
                require_integer(item.value("size", Json()), "tensor size");
                auto pos = item.value("pos", std::string());
                if (pos != "DDR" && pos != "L1" && pos != "UB") {
                    throw EvaluationValidationError("unsupported memory pos");
                }
            } else {
                ops.insert(id);
                auto pipe = item.value("pipe", std::string());
                if (std::find(PIPES.begin(), PIPES.end(), pipe) == PIPES.end()) {
                    throw EvaluationValidationError("unsupported pipe");
                }
                const auto cycles = item.value("cycles", Json());
                const auto operation = item.value("op", std::string());
                // 下发图的 DDR COPY 使用 -1 表示耗时待计算。实际耗时由张量大小
                // 和带宽决定，Step2/3 的计算规则不变；其他负数仍然是非法输入。
                const bool copy_placeholder = cycles.is_number_integer() && cycles == -1 &&
                                              (operation == "COPY_IN" || operation == "COPY_OUT");
                if (!copy_placeholder) {
                    require_integer(cycles, "op cycles");
                }
            }
        }
    }
    std::set<std::pair<Id, Id>> seen;
    std::vector<std::tuple<Id, Id, std::string>> edges;
    for (const auto &e : graph.at("edges")) {
        if (!e.is_object()) {
            throw EvaluationValidationError("edge must be an object");
        }
        require_integer(e.value("source", Json()), "edge.source");
        require_integer(e.value("target", Json()), "edge.target");
        Id a = field_id(e, "source"), b = field_id(e, "target");
        if (!ids.count(a) || !ids.count(b)) {
            throw EvaluationValidationError("edge references unknown id");
        }
        if (!ops.count(a) && !ops.count(b)) {
            throw EvaluationValidationError("tensor-to-tensor edge is unsupported");
        }
        if (!seen.insert({a, b}).second) {
            throw EvaluationValidationError("duplicate edge");
        }
        edges.emplace_back(a, b, "graph");
    }
    check_acyclic(as_vector(ids), edges, "input graph");
}
void validate_task_order(const Json &view) {
    std::vector<std::tuple<Id, Id, std::string>> edges;
    for (const auto &e : view.at("dependency_pairs")) {
        edges.emplace_back(number(e[0]), number(e[1]), "subgraph dependency");
    }
    for (const auto &order : view.at("core_orders")) {
        for (std::size_t i = 1; i < order.size(); ++i) {
            edges.emplace_back(number(order[i - 1]), number(order[i]), "task order");
        }
    }
    check_acyclic(view.at("subgraph_ids").get<Ids>(), edges, "task schedule");
}
} // namespace problem2
