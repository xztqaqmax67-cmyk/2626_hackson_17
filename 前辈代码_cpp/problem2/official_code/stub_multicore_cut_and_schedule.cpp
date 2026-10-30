// 对应 official_code/stub_multicore_cut_and_schedule.py。
// 生成官方示例随机方案；复现 Python 整数种子的随机数序列。
#include "stub_multicore_cut_and_schedule.hpp"
#include "evaluation_validation.hpp"
namespace problem2 {
std::pair<Adjacency, Adjacency> contract_excluded_copy_nodes(const Ids &eligible,
                                                             const Adjacency &successors) {
    IdSet selected = as_set(eligible);
    Adjacency preds, succs;
    for (Id source : eligible) {
        preds[source];
        succs[source];
    }
    for (Id source : eligible) {
        Ids stack = as_vector(successors.at(source));
        IdSet visited;
        while (!stack.empty()) {
            Id target = stack.back();
            stack.pop_back();
            if (selected.count(target)) {
                if (source != target) {
                    preds[target].insert(source);
                    succs[source].insert(target);
                }
                continue;
            }
            if (!visited.insert(target).second) {
                continue;
            }
            if (successors.count(target)) {
                append(stack, as_vector(successors.at(target)));
            }
        }
    }
    return {preds, succs};
}
Json derive_multicore_plan(const Json &graph, const Json &plan) {
    validate_graph(graph);
    if (!plan.is_object() || plan.size() != 2 || !plan.contains("node_to_subgraph") ||
        !plan.contains("core_schedules")) {
        throw MulticoreCutError("plan must contain exactly node_to_subgraph and core_schedules");
    }
    IdSet eligible;
    for (const auto &op : graph.at("ops")) {
        if (!is_copy(op)) {
            eligible.insert(field_id(op));
        }
    }
    const auto &supplied = plan.at("node_to_subgraph");
    if (!supplied.is_object()) {
        throw MulticoreCutError("node_to_subgraph must be an object");
    }
    std::map<Id, Id> mapping;
    Json mapping_json = object();
    for (auto it = supplied.begin(); it != supplied.end(); ++it) {
        if (it.key().empty() || !std::all_of(it.key().begin(), it.key().end(), [](unsigned char c) {
                return c >= '0' && c <= '9';
            })) {
            throw MulticoreCutError("node keys must be integer op ids");
        }
        Id id;
        try {
            id = std::stoll(it.key());
        } catch (...) {
            throw MulticoreCutError("node id out of range");
        }
        require_integer(it.value(), "subgraph id");
        if (mapping.count(id)) {
            throw MulticoreCutError("duplicate integer op ids");
        }
        mapping[id] = number(it.value());
        mapping_json[key(id)] = it.value();
    }
    IdSet mapped;
    for (auto [op, b] : mapping) {
        mapped.insert(op);
    }
    if (mapped != eligible) {
        throw MulticoreCutError("node_to_subgraph must exactly cover non-COPY ops");
    }
    const auto &schedules = plan.at("core_schedules");
    if (!schedules.is_array() || schedules.empty()) {
        throw MulticoreCutError("core_schedules must be a non-empty list");
    }
    IdSet blocks, scheduled;
    std::map<Id, Id> core_of, position;
    Json core_orders = object(), nodes = object();
    for (auto [op, b] : mapping) {
        blocks.insert(b);
    }
    for (Id b : blocks) {
        nodes[key(b)] = array();
    }
    for (auto [op, b] : mapping) {
        nodes[key(b)].push_back(op);
    }
    for (std::size_t c = 0; c < schedules.size(); ++c) {
        const auto &order = schedules[c];
        if (!order.is_array()) {
            throw MulticoreCutError("core schedule must be a list");
        }
        core_orders[key(Id(c))] = order;
        for (std::size_t i = 0; i < order.size(); ++i) {
            require_integer(order[i], "schedule id");
            Id b = number(order[i]);
            if (!scheduled.insert(b).second) {
                throw MulticoreCutError("duplicate scheduled subgraph");
            }
            core_of[b] = Id(c);
            position[b] = Id(i);
        }
    }
    if (blocks != scheduled) {
        throw MulticoreCutError("core schedules must cover every subgraph exactly once");
    }
    auto views = build_graph_views(graph.at("ops"), graph.at("edges"));
    for (const auto &[tid, writers] : views.producers) {
        IdSet writer_cores;
        for (Id writer : writers) {
            if (eligible.count(writer)) {
                writer_cores.insert(core_of.at(mapping.at(writer)));
            }
        }
        if (writer_cores.size() > 1) {
            throw MulticoreCutError("tensor " + key(tid) + " has writers on multiple cores; "
                                   "all writers of the same tensor must be scheduled on the same core");
        }
    }
    auto contracted = contract_excluded_copy_nodes(as_vector(eligible), views.succs);
    std::set<std::pair<Id, Id>> dependencies;
    for (Id a : eligible) {
        for (Id b : contracted.second.at(a)) {
            if (mapping.at(a) != mapping.at(b)) {
                dependencies.insert({mapping.at(a), mapping.at(b)});
            }
        }
    }
    Adjacency preds, succs;
    for (Id b : blocks) {
        preds[b];
        succs[b];
    }
    std::vector<std::tuple<Id, Id, std::string>> edges;
    for (auto [a, b] : dependencies) {
        preds[b].insert(a);
        succs[a].insert(b);
        edges.emplace_back(a, b, "subgraph");
        if (core_of[a] == core_of[b] && position[a] >= position[b]) {
            throw MulticoreCutError("dependency order violation on core " + key(core_of[a]));
        }
    }
    check_acyclic(as_vector(blocks), edges, "contracted subgraph graph");
    return {{"mapping", mapping_json},
            {"num_cores", schedules.size()},
            {"subgraph_ids", blocks},
            {"nodes_by_subgraph", nodes},
            {"core_orders", core_orders},
            {"core_by_subgraph", id_map_json(core_of)},
            {"dependency_pairs", dependencies},
            {"subgraph_preds", id_map_json(preds)},
            {"subgraph_succs", id_map_json(succs)}};
}
bool validate_multicore_plan(const Json &graph, const Json &plan) {
    derive_multicore_plan(graph, plan);
    return true;
}
// CPython random.Random 的整数种子初始化和 getrandbits。
// std::uniform_int_distribution 的取样方式不同，不能替代原来的 randrange。
class PythonRandom {
    std::array<std::uint32_t, 624> state{};
    std::size_t index = 624;
    std::uint32_t next() {
        if (index == 624) {
            for (std::size_t i = 0; i < 624; ++i) {
                auto y = (state[i] & 0x80000000u) | (state[(i + 1) % 624] & 0x7fffffffu);
                state[i] = state[(i + 397) % 624] ^ (y >> 1) ^ ((y & 1) ? 0x9908b0dfu : 0);
            }
            index = 0;
        }
        auto y = state[index++];
        y ^= y >> 11;
        y ^= (y << 7) & 0x9d2c5680u;
        y ^= (y << 15) & 0xefc60000u;
        y ^= y >> 18;
        return y;
    }

public:
    explicit PythonRandom(std::uint64_t seed) {
        std::vector<std::uint32_t> keys = {std::uint32_t(seed)};
        if (seed >> 32) {
            keys.push_back(std::uint32_t(seed >> 32));
        }
        state[0] = 19650218u;
        for (std::size_t i = 1; i < 624; ++i) {
            state[i] = 1812433253u * (state[i - 1] ^ (state[i - 1] >> 30)) + std::uint32_t(i);
        }
        std::size_t i = 1, j = 0;
        for (std::size_t k = 0; k < std::max<std::size_t>(624, keys.size()); ++k) {
            state[i] = (state[i] ^ ((state[i - 1] ^ (state[i - 1] >> 30)) * 1664525u)) + keys[j] +
                       std::uint32_t(j);
            if (++i >= 624) {
                state[0] = state[623];
                i = 1;
            }
            if (++j >= keys.size()) {
                j = 0;
            }
        }
        for (std::size_t k = 0; k < 623; ++k) {
            state[i] = (state[i] ^ ((state[i - 1] ^ (state[i - 1] >> 30)) * 1566083941u)) -
                       std::uint32_t(i);
            if (++i >= 624) {
                state[0] = state[623];
                i = 1;
            }
        }
        state[0] = 0x80000000u;
    }
    std::uint64_t below(std::uint64_t n) {
        unsigned bits = 0;
        for (auto x = n; x; x >>= 1) {
            ++bits;
        }
        std::uint64_t value;
        do {
            if (bits <= 32) {
                value = next() >> (32 - bits);
            } else {
                value = next();
                value |= std::uint64_t(next() >> (64 - bits)) << 32;
            }
        } while (value >= n);
        return value;
    }
};
Json generate_multicore_plan(const Json &graph, Id cores, std::uint64_t seed, Id min_size,
                             Id max_size) {
    validate_graph(graph);
    if (cores < 1 || min_size < 1 || max_size < min_size) {
        throw MulticoreCutError("invalid core count or subgraph size range");
    }
    Ids ids;
    for (const auto &op : graph.at("ops")) {
        if (!is_copy(op)) {
            ids.push_back(field_id(op));
        }
    }
    std::sort(ids.begin(), ids.end());
    auto views = build_graph_views(graph.at("ops"), graph.at("edges"));
    auto [preds, succs] = contract_excluded_copy_nodes(ids, views.succs);
    PythonRandom rng(seed);
    std::map<Id, Id> degree;
    Ids ready, order;
    for (Id id : ids) {
        degree[id] = Id(preds[id].size());
        if (!degree[id]) {
            ready.push_back(id);
        }
    }
    while (!ready.empty()) {
        auto i = rng.below(ready.size());
        Id id = ready[i];
        ready.erase(ready.begin() + i);
        order.push_back(id);
        for (Id succ : succs[id]) {
            if (--degree[succ] == 0) {
                ready.push_back(succ);
                std::sort(ready.begin(), ready.end());
            }
        }
    }
    Id remaining = Id(order.size());
    Ids sizes;
    while (remaining > max_size) {
        Id upper = std::min(max_size, remaining - min_size);
        Id size = min_size + Id(rng.below(upper - min_size + 1));
        sizes.push_back(size);
        remaining -= size;
    }
    if (remaining) {
        sizes.push_back(remaining);
    }
    Json mapping = object(), schedules = array();
    for (Id c = 0; c < cores; ++c) {
        schedules.push_back(array());
    }
    std::size_t cursor = 0;
    for (std::size_t b = 0; b < sizes.size(); ++b) {
        for (Id i = 0; i < sizes[b]; ++i) {
            mapping[key(order[cursor++])] = b;
        }
    }
    // 共享写者的子图作为一组分核，保持官方的随机调用次数与组顺序。
    Ids parent(sizes.size());
    std::iota(parent.begin(), parent.end(), Id(0));
    auto find = [&](Id id) {
        while (parent[id] != id) {
            parent[id] = parent[parent[id]];
            id = parent[id];
        }
        return id;
    };
    for (const auto &[tid, writers] : views.producers) {
        IdSet blocks;
        for (Id writer : writers) {
            if (mapping.contains(key(writer))) {
                blocks.insert(number(mapping.at(key(writer))));
            }
        }
        for (Id block : blocks) {
            parent[find(block)] = find(*blocks.begin());
        }
    }
    std::map<Id, Ids> groups;
    for (std::size_t b = 0; b < sizes.size(); ++b) {
        groups[find(Id(b))].push_back(Id(b));
    }
    std::vector<Ids> ordered_groups;
    for (const auto &[root, group] : groups) {
        ordered_groups.push_back(group);
    }
    std::sort(ordered_groups.begin(), ordered_groups.end());
    for (const auto &group : ordered_groups) {
        auto &schedule = schedules[rng.below(std::uint64_t(cores))];
        for (Id block : group) {
            schedule.push_back(block);
        }
    }
    for (auto &schedule : schedules) {
        std::sort(schedule.begin(), schedule.end());
    }
    return {{"node_to_subgraph", mapping}, {"core_schedules", schedules}};
}
} // namespace problem2
