// 对应 core.py。
// 计算图、分块方案及其基本变换；估算模型只用于筛选候选。
#include "core.hpp"
#include "utils.hpp"
#include "official_code/evaluation_validation.hpp"
#include "official_code/stub_multicore_cut_and_schedule.hpp"
namespace problem2 {
// 一次建立算子、张量与邻接关系的索引，后续变换无需反复扫描 JSON。
GraphInfo::GraphInfo(const Json &input) : graph(input) {
    validate_graph(graph);
    ops = index_records(graph.at("ops"));
    tensors = index_records(graph.at("tensors"));
    for (const auto &[id, op] : ops) {
        if (!is_copy(op)) {
            ids.push_back(id);
        }
    }
    eligible = as_set(ids);
    auto views = build_graph_views(graph.at("ops"), graph.at("edges"));
    auto contracted = contract_excluded_copy_nodes(ids, views.succs);
    preds = contracted.first;
    succs = contracted.second;
    producers = views.producers;
    consumers = views.consumers;
    for (Id op : ids) {
        op_inputs[op] = views.inputs[op];
        op_outputs[op] = views.outputs[op];
        weight[op] = std::max<Id>(1, field_id(ops.at(op), "cycles"));
    }
    for (const auto &[tid, tensor] : tensors) {
        auto &ps = eligible_producers[tid];
        auto &cs = eligible_consumers[tid];
        for (Id op : producers[tid]) {
            if (eligible.count(op)) {
                ps.insert(op);
            }
        }
        for (Id op : consumers[tid]) {
            if (eligible.count(op)) {
                cs.insert(op);
            }
        }
        if (!cs.empty() && ps.empty()) {
            external_input_groups[tid] = cs;
        }
        bool out = cs.empty();
        for (Id op : consumers[tid]) {
            if (ops.at(op).at("op") == "COPY_OUT") {
                out = true;
            }
        }
        if (!ps.empty() && out) {
            original_output_relations[tid] = ps;
        }
        for (Id a : ps) {
            for (Id b : cs) {
                edge_bytes[{a, b}] += field_id(tensor, "size");
            }
        }
    }
    for (const auto &e : graph.at("edges")) {
        if (ops.count(field_id(e, "source")) && ops.count(field_id(e, "target"))) {
            Id a = field_id(e, "source"), b = field_id(e, "target"),
               index = Id(direct_edges.size()), size = std::max<Id>(0, e.value("data_size", Id(0)));
            direct_edges.push_back(e);
            if (eligible.count(a) && eligible.count(b)) {
                direct_inputs[b].emplace_back(index, a, size);
                edge_bytes[{a, b}] += size;
            }
        }
    }
    id_order = topological_order("id");
    for (std::size_t i = 0; i < id_order.size(); ++i) {
        position[id_order[i]] = Id(i);
    }
    for (auto i = id_order.rbegin(); i != id_order.rend(); ++i) {
        Id tail = 0;
        for (Id s : succs.at(*i)) {
            tail = std::max(tail, rank.at(s));
        }
        rank[*i] = weight.at(*i) + tail;
    }
    for (Id root : id_order) {
        if (!atom_of.count(root)) {
            Id index = Id(atoms.size());
            atoms.push_back({});
            Ids stack = {root};
            atom_of[root] = index;
            while (!stack.empty()) {
                Id v = stack.back();
                stack.pop_back();
                IdSet neighbors = preds.at(v);
                neighbors.insert(succs.at(v).begin(), succs.at(v).end());
                for (Id other : neighbors) {
                    if (!atom_of.count(other)) {
                        atom_of[other] = index;
                        stack.push_back(other);
                    }
                }
            }
        }
    }
    for (Id v : id_order) {
        atoms[atom_of.at(v)].push_back(v);
    }
}
GraphInfo GraphInfo::from_path(const fs::path &path) {
    return GraphInfo(read_json_strict(path));
}
// 拓扑约束优先；同时就绪时，按原版的局部性或关键路径规则打破平局。
Ids GraphInfo::topological_order(const std::string &mode) const {
    if (mode != "id" && mode != "locality" && mode != "critical") {
        throw std::invalid_argument("unsupported topological order mode");
    }
    std::map<Id, Id> degree;
    IdSet ready;
    Ids order;
    std::optional<Id> last;
    for (Id v : ids) {
        degree[v] = Id(preds.at(v).size());
        if (!degree[v]) {
            ready.insert(v);
        }
    }
    while (!ready.empty()) {
        IdSet nearby;
        if (last && mode == "locality") {
            for (Id v : succs.at(*last)) {
                if (ready.count(v)) {
                    nearby.insert(v);
                }
            }
        }
        auto priority = [&](Id v) { return std::make_pair(mode == "id" ? Id(0) : -rank.at(v), v); };
        Id v = *std::min_element(ready.begin(), ready.end(),
                                 [&](Id a, Id b) { return priority(a) < priority(b); });
        if (!nearby.empty()) {
            auto local_key = [&](Id w) {
                auto it = edge_bytes.find({*last, w});
                return std::make_tuple(it == edge_bytes.end() ? Id(0) : -it->second, -rank.at(w),
                                       w);
            };
            v = *std::min_element(nearby.begin(), nearby.end(),
                                  [&](Id a, Id b) { return local_key(a) < local_key(b); });
        }
        ready.erase(v);
        order.push_back(v);
        last = v;
        for (Id w : succs.at(v)) {
            if (--degree[w] == 0) {
                ready.insert(w);
            }
        }
    }
    if (order.size() != ids.size()) {
        throw std::invalid_argument("non-COPY operation graph contains a cycle");
    }
    return order;
}
std::map<Id, Id> Plan::owner() const {
    std::map<Id, Id> result;
    for (const auto &[b, nodes] : blocks) {
        for (Id v : nodes) {
            result[v] = b;
        }
    }
    return result;
}
// 内部保留全局块顺序，提交时按核心拆成 FIFO 队列；输出仅含题目规定字段。
Json Plan::to_dict() const {
    Json mapping = object(), schedules = array();
    auto &entries = mapping.get_ref<Json::object_t &>();
    std::size_t count = 0;
    for (const auto &[b, nodes] : blocks) count += nodes.size();
    entries.reserve(count);
    for (const auto &[b, nodes] : blocks) {
        for (Id v : nodes) {
            // A valid Plan covers each operation exactly once.
            entries.emplace_back(key(v), Json(b));
        }
    }
    for (Id c = 0; c < ncores; ++c) {
        Ids local;
        for (Id b : order) {
            if (core_of.at(b) == c) {
                local.push_back(b);
            }
        }
        schedules.push_back(local);
    }
    return {{"node_to_subgraph", mapping}, {"core_schedules", schedules}};
}
// 规范化键顺序后计算方案标识；相同方案即使构造路径不同也只评估一次。
std::string Plan::fingerprint() const {
    nlohmann::json sorted = to_dict();
    return sha256_text(sorted.dump());
}
bool Plan::validate(const GraphInfo &g) const {
    if (ncores < 1) {
        throw std::invalid_argument("ncores must be positive");
    }
    IdSet block_ids;
    for (const auto &[b, nodes] : blocks) {
        block_ids.insert(b);
    }
    IdSet assignment;
    for (auto [b, c] : core_of) {
        assignment.insert(b);
    }
    if (block_ids != assignment || order.size() != blocks.size() || as_set(order) != block_ids) {
        throw std::invalid_argument("blocks, assignment and global order disagree");
    }
    auto owners = owner();
    Id count = 0;
    IdSet covered;
    for (auto [v, b] : owners) {
        covered.insert(v);
    }
    for (const auto &[b, nodes] : blocks) {
        count += Id(nodes.size());
    }
    if (Id(owners.size()) != count || covered != g.eligible) {
        throw std::invalid_argument("plan must cover each eligible operation exactly once");
    }
    for (const auto &[b, nodes] : blocks) {
        if (b < 0 || nodes.empty() || core_of.at(b) < 0 || core_of.at(b) >= ncores) {
            throw std::invalid_argument("invalid block or core assignment");
        }
        std::map<Id, Id> pos;
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            pos[nodes[i]] = Id(i);
        }
        for (Id v : nodes) {
            for (Id p : g.preds.at(v)) {
                if (pos.count(p) && pos[p] >= pos[v]) {
                    throw std::invalid_argument("block internal order violates a dependency");
                }
            }
        }
    }
    std::map<Id, Id> pos;
    for (std::size_t i = 0; i < order.size(); ++i) {
        pos[order[i]] = Id(i);
    }
    for (Id v : g.ids) {
        for (Id w : g.succs.at(v)) {
            if (owners.at(v) != owners.at(w) && pos[owners.at(v)] >= pos[owners.at(w)]) {
                throw std::invalid_argument("global block order violates a dependency");
            }
        }
    }
    if (next_block_id <= (blocks.empty() ? -1 : blocks.rbegin()->first)) {
        throw std::invalid_argument("next_block_id must exceed stable IDs");
    }
    return true;
}
std::pair<Adjacency, Adjacency> block_dependencies(const GraphInfo &g, const Blocks &blocks) {
    Adjacency preds, succs;
    std::map<Id, Id> owner;
    for (const auto &[b, nodes] : blocks) {
        preds[b];
        succs[b];
        for (Id v : nodes) {
            owner[v] = b;
        }
    }
    for (Id v : g.ids) {
        for (Id w : g.succs.at(v)) {
            Id a = owner.at(v), b = owner.at(w);
            if (a != b) {
                preds[b].insert(a);
                succs[a].insert(b);
            }
        }
    }
    return {preds, succs};
}
Ids topological_blocks(const Adjacency &preds, const Adjacency &succs,
                       const std::map<Id, double> &priority) {
    std::map<Id, Id> degree;
    std::set<std::pair<double, Id>> ready;
    auto key_for = [&](Id b) {
        return std::make_pair(priority.empty() ? double(b) : priority.at(b), b);
    };
    for (auto [b, ps] : preds) {
        degree[b] = Id(ps.size());
        if (!degree[b]) {
            ready.insert(key_for(b));
        }
    }
    Ids order;
    while (!ready.empty()) {
        Id b = ready.begin()->second;
        ready.erase(ready.begin());
        order.push_back(b);
        for (Id c : succs.at(b)) {
            if (--degree[c] == 0) {
                ready.insert(key_for(c));
            }
        }
    }
    if (order.size() != preds.size()) {
        throw std::invalid_argument("block dependency/order graph contains a cycle");
    }
    return order;
}
Plan Plan::from_dict(const GraphInfo &g, const Json &data) {
    Plan p;
    p.ncores = Id(data.at("core_schedules").size());
    for (Id v : g.id_order) {
        p.blocks[number(data.at("node_to_subgraph").at(key(v)))].push_back(v);
    }
    auto [preds, succs] = block_dependencies(g, p.blocks);
    for (std::size_t c = 0; c < data.at("core_schedules").size(); ++c) {
        auto local = data.at("core_schedules")[c].get<Ids>();
        for (Id b : local) {
            p.core_of[b] = Id(c);
        }
        for (std::size_t i = 1; i < local.size(); ++i) {
            preds[local[i]].insert(local[i - 1]);
            succs[local[i - 1]].insert(local[i]);
        }
    }
    p.order = topological_blocks(preds, succs);
    p.next_block_id = p.blocks.empty() ? 0 : p.blocks.rbegin()->first + 1;
    p.validate(g);
    return p;
}
// 按原子组切块，避免拆开必须一同处理的结构；max_ops 只约束可切分部分。
Blocks build_blocks(const GraphInfo &g, Id cores, Id max_ops, double deadline) {
    double total = 0;
    for (auto [v, w] : g.weight) {
        total += w;
    }
    double target = total / std::max<Id>(1, std::min<Id>(Id(g.ids.size()), 8 * cores));
    Blocks blocks;
    Ids nodes;
    Id work = 0;
    std::optional<Id> previous;
    for (Id v : g.topological_order("locality")) {
        check_deadline(deadline);
        Id component = g.atom_of.at(v);
        if (!nodes.empty() && (Id(nodes.size()) >= max_ops || work + g.weight.at(v) > target ||
                               component != previous)) {
            blocks[Id(blocks.size())] = nodes;
            nodes.clear();
            work = 0;
        }
        nodes.push_back(v);
        work += g.weight.at(v);
        previous = component;
    }
    if (!nodes.empty()) {
        blocks[Id(blocks.size())] = nodes;
    }
    return blocks;
}
Plan single_core_plan(const GraphInfo &g, Id cores) {
    Plan p;
    p.ncores = cores;
    if (!g.ids.empty()) {
        p.blocks[0] = g.id_order;
        p.core_of[0] = 0;
        p.order = {0};
        p.next_block_id = 1;
    }
    return p;
}
// 按“源核—目标核”组合计搬运量；同一张量的同核多个消费者不重复收费。
Json route_details(const GraphInfo &g, const Plan &p) {
    auto owner = p.owner();
    std::map<Id, Id> core;
    for (Id v : g.ids) {
        core[v] = p.core_of.at(owner.at(v));
    }
    Json ledger = array();
    Id total = 0;
    auto add = [&](Json row) {
        total += field_id(row, "bytes");
        ledger.push_back(row);
    };
    for (const auto &[tid, tensor] : g.tensors) {
        Id size = field_id(tensor, "size");
        IdSet ps, cs;
        for (Id v : g.eligible_producers.at(tid)) {
            ps.insert(core.at(v));
        }
        for (Id v : g.eligible_consumers.at(tid)) {
            cs.insert(core.at(v));
        }
        if (ps.empty()) {
            for (Id c : cs) {
                add({{"kind", "input"}, {"tensor_id", tid}, {"target_core", c}, {"bytes", size}});
            }
        }
        if (g.original_output_relations.count(tid)) {
            for (Id c : ps) {
                add({{"kind", "output"}, {"tensor_id", tid}, {"source_core", c}, {"bytes", size}});
            }
        }
        for (Id a : ps) {
            for (Id b : cs) {
                if (a != b) {
                    add({{"kind", "cross"},
                         {"tensor_id", tid},
                         {"source_core", a},
                         {"target_core", b},
                         {"bytes", 2 * size}});
                }
            }
        }
    }
    for (std::size_t i = 0; i < g.direct_edges.size(); ++i) {
        const auto &e = g.direct_edges[i];
        Id a = field_id(e, "source"), b = field_id(e, "target");
        if (core.count(a) && core.count(b) && core[a] != core[b]) {
            add({{"kind", "direct"},
                 {"edge_index", i},
                 {"source_core", core[a]},
                 {"target_core", core[b]},
                 {"bytes", 2 * std::max<Id>(0, e.value("data_size", Id(0)))}});
        }
    }
    return {{"total_ddr_bytes", total}, {"ledger", ledger}};
}
Id route_bytes(const GraphInfo &g, const Plan &p) {
    const auto owner = p.owner();
    std::map<Id, Id> core;
    for (Id v : g.ids) core[v] = p.core_of.at(owner.at(v));
    Id total = 0;
    for (const auto &[tid, tensor] : g.tensors) {
        Id size = field_id(tensor, "size");
        IdSet producers, consumers;
        for (Id v : g.eligible_producers.at(tid)) producers.insert(core.at(v));
        for (Id v : g.eligible_consumers.at(tid)) consumers.insert(core.at(v));
        if (producers.empty()) {
            for (Id c : consumers) { (void)c; total += size; }
        }
        if (g.original_output_relations.count(tid)) {
            for (Id c : producers) { (void)c; total += size; }
        }
        for (Id a : producers) for (Id b : consumers) if (a != b) total += 2 * size;
    }
    for (const auto &e : g.direct_edges) {
        Id a = field_id(e, "source"), b = field_id(e, "target");
        if (core.count(a) && core.count(b) && core.at(a) != core.at(b))
            total += 2 * std::max<Id>(0, e.value("data_size", Id(0)));
    }
    return total;
}
PipeLoads pipe_loads(const GraphInfo &g, const Plan &p) {
    PipeLoads loads(p.ncores);
    for (auto &row : loads) {
        for (auto pipe : PIPES) {
            row[pipe] = 0;
        }
    }
    for (const auto &[b, nodes] : p.blocks) {
        for (Id v : nodes) {
            loads[p.core_of.at(b)][field_string(g.ops.at(v), "pipe")] +=
                field_id(g.ops.at(v), "cycles");
        }
    }
    return loads;
}
Sketch::Sketch(const GraphInfo &g, Id cores, const Settings &settings)
    : graph_(&g), settings_(&settings), available(cores), fixed(cores) {
    for (auto *rows : {&available, &fixed}) {
        for (auto &row : *rows) {
            for (auto pipe : PIPES) {
                row[pipe] = 0;
            }
        }
    }
}
double Sketch::transfer(const std::string &route, Id size, Id source, Id target, double ready) {
    Id duration = copy_duration(size, settings_->bandwidth);
    if (!copy_out_.count(route)) {
        double end = std::max(ready, available[source]["PIPE_MTE3"]) + duration;
        available[source]["PIPE_MTE3"] = end;
        copy_out_[route] = end;
        route_bytes += size;
    }
    if (!copy_in_.count(route)) {
        double end = std::max(copy_out_.at(route) + settings_->cross_core_copy_delay,
                              available[target]["PIPE_MTE2"]) +
                     duration;
        available[target]["PIPE_MTE2"] = end;
        copy_in_[route] = end;
        route_bytes += size;
    }
    return copy_in_.at(route);
}
// 张量就绪时间取决于生产者完成时间，以及到目标核心所需的搬运。
double Sketch::tensor_ready(Id tid, Id target) {
    const auto &g = *graph_;
    const auto &ps = g.eligible_producers.at(tid);
    Id size = field_id(g.tensors.at(tid), "size");
    if (ps.empty()) {
        auto pair = std::make_pair(tid, target);
        if (!local_ready_.count(pair)) {
            available[target]["PIPE_MTE2"] += copy_duration(size, settings_->bandwidth);
            local_ready_[pair] = available[target]["PIPE_MTE2"];
            route_bytes += size;
        }
        return local_ready_.at(pair);
    }
    std::map<Id, double> production;
    for (Id p : ps) {
        production[core.at(p)] = std::max(production[core.at(p)], finish.at(p));
    }
    double ready = 0;
    for (auto [source, time] : production) {
        ready = std::max(ready, source == target ? time
                                                 : transfer("tensor:" + key(tid) + ":" +
                                                                key(source) + ":" + key(target),
                                                            size, source, target, time));
    }
    local_ready_[{tid, target}] = ready;
    return ready;
}
double Sketch::append(const Ids &nodes, Id target) {
    const auto &g = *graph_;
    double block_finish = 0;
    for (Id v : nodes) {
        double readiness = 0;
        for (Id p : g.preds.at(v)) {
            readiness = std::max(readiness, finish.at(p));
        }
        for (Id tid : g.op_inputs.at(v)) {
            readiness = std::max(readiness, tensor_ready(tid, target));
        }
        auto inputs = g.direct_inputs.find(v);
        if (inputs != g.direct_inputs.end()) {
            for (auto [index, p, size] : inputs->second) {
                if (core.at(p) != target) {
                    readiness = std::max(readiness, transfer("direct:" + key(index), size,
                                                             core.at(p), target, finish.at(p)));
                }
            }
        }
        const auto &op = g.ops.at(v);
        auto pipe = field_string(op, "pipe");
        double end =
            std::max(readiness, available[target][pipe]) + std::max<Id>(1, field_id(op, "cycles"));
        available[target][pipe] = end;
        fixed[target][pipe] += field_id(op, "cycles");
        finish[v] = end;
        core[v] = target;
        block_finish = std::max(block_finish, end);
        for (Id tid : g.op_outputs.at(v)) {
            if (!g.original_output_relations.count(tid)) {
                continue;
            }
            const auto &ps = g.eligible_producers.at(tid);
            bool complete = true;
            for (Id p : ps) {
                if (!finish.count(p)) {
                    complete = false;
                }
            }
            if (!complete) {
                continue;
            }
            std::map<Id, double> groups;
            for (Id p : ps) {
                groups[core.at(p)] = std::max(groups[core.at(p)], finish.at(p));
            }
            for (auto [source, production] : groups) {
                if (output_done_.insert({tid, source}).second) {
                    Id size = field_id(g.tensors.at(tid), "size");
                    double tail = std::max(available[source]["PIPE_MTE3"], production) +
                                  copy_duration(size, settings_->bandwidth);
                    available[source]["PIPE_MTE3"] = tail;
                    route_bytes += size;
                    block_finish = std::max(block_finish, tail);
                }
            }
        }
    }
    return block_finish;
}
double Sketch::makespan() const {
    double value = 0;
    for (const auto &row : available) {
        for (auto [pipe, end] : row) {
            value = std::max(value, end);
        }
    }
    return value;
}
// 逐块尝试放到各核心，用草图完成时间和负载惩罚选择目标核心。
Plan construct(const GraphInfo &g, Id cores, double lambda, const Settings &settings,
               const std::optional<Blocks> &supplied, double deadline) {
    Blocks blocks = supplied ? *supplied : build_blocks(g, cores, 64, deadline);
    auto [preds, succs] = block_dependencies(g, blocks);
    auto topo = topological_blocks(preds, succs);
    std::map<Id, double> rank;
    for (auto i = topo.rbegin(); i != topo.rend(); ++i) {
        double tail = 0;
        for (Id c : succs.at(*i)) {
            tail = std::max(tail, rank.at(c));
        }
        for (Id v : blocks.at(*i)) {
            tail += g.weight.at(v);
        }
        rank[*i] = tail;
    }
    std::map<Id, double> priority;
    for (auto [b, r] : rank) {
        priority[b] = -r;
    }
    auto order = topological_blocks(preds, succs, priority);
    Sketch sketch(g, cores, settings);
    std::map<Id, Id> assignment;
    for (Id b : order) {
        check_deadline(deadline);
        std::optional<Sketch> best;
        std::tuple<double, double, Id> best_key;
        for (Id c = 0; c < cores; ++c) {
            auto trial = sketch;
            double finish = trial.append(blocks.at(b), c), load = 0;
            for (auto [pipe, value] : trial.fixed[c]) {
                load = std::max(load, value);
            }
            auto score = std::make_tuple(
                finish + lambda * (trial.route_bytes - sketch.route_bytes) / settings.bandwidth,
                load, c);
            if (!best || score < best_key) {
                best = trial;
                best_key = score;
            }
        }
        sketch = *best;
        assignment[b] = std::get<2>(best_key);
    }
    Plan p{blocks, assignment, order, blocks.empty() ? 0 : blocks.rbegin()->first + 1, cores};
    p.validate(g);
    return p;
}
Json replay_sketch_details(const GraphInfo &g, const Plan &p, const Settings &settings,
                           double deadline) {
    Sketch sketch(g, p.ncores, settings);
    for (Id b : p.order) {
        check_deadline(deadline);
        sketch.append(p.blocks.at(b), p.core_of.at(b));
    }
    return {{"makespan", sketch.makespan()},
            {"route_bytes", sketch.route_bytes},
            {"available", sketch.available},
            {"finish", id_map_json(sketch.finish)}};
}
double replay_sketch(const GraphInfo &g, const Plan &p, const Settings &settings, double deadline) {
    Sketch sketch(g, p.ncores, settings);
    for (Id b : p.order) {
        check_deadline(deadline);
        sketch.append(p.blocks.at(b), p.core_of.at(b));
    }
    return sketch.makespan();
}
Plan migrate(const Plan &p, const Ids &blocks, Id target) {
    if (target < 0 || target >= p.ncores) {
        throw std::invalid_argument("invalid migration target");
    }
    auto result = p;
    for (Id b : blocks) {
        if (!result.blocks.count(b)) {
            throw std::invalid_argument("unknown migration block");
        }
        result.core_of[b] = target;
    }
    return result;
}
Plan swap(const Plan &p, Id first, Id second) {
    auto result = p;
    std::swap(result.core_of.at(first), result.core_of.at(second));
    return result;
}
std::optional<Plan> split_block(const GraphInfo &g, const Plan &p, Id b, std::optional<Id> cut) {
    const auto &nodes = p.blocks.at(b);
    if (nodes.size() < 2) {
        return {};
    }
    if (!cut) {
        double half = 0;
        for (Id v : nodes) {
            half += g.weight.at(v);
        }
        half /= 2;
        Id total = 0;
        for (std::size_t i = 0; i + 1 < nodes.size(); ++i) {
            total += g.weight.at(nodes[i]);
            cut = Id(i) + 1;
            if (total >= half) {
                break;
            }
        }
    }
    if (*cut <= 0 || *cut >= Id(nodes.size())) {
        throw std::invalid_argument("split cut must be internal");
    }
    auto result = p;
    Id new_id = result.next_block_id++;
    result.blocks[b] = Ids(nodes.begin(), nodes.begin() + *cut);
    result.blocks[new_id] = Ids(nodes.begin() + *cut, nodes.end());
    result.core_of[new_id] = p.core_of.at(b);
    result.order.insert(std::find(result.order.begin(), result.order.end(), b) + 1, new_id);
    result.validate(g);
    return result;
}
std::optional<Plan> merge_adjacent(const GraphInfo &g, const Plan &p, Id first,
                                   std::optional<Id> second, Id max_ops) {
    auto it = std::find(p.order.begin(), p.order.end(), first);
    if (it == p.order.end() || it + 1 == p.order.end()) {
        return {};
    }
    if (!second) {
        second = *(it + 1);
    }
    if (*(it + 1) != *second || p.core_of.at(first) != p.core_of.at(*second) ||
        Id(p.blocks.at(first).size() + p.blocks.at(*second).size()) > max_ops) {
        return {};
    }
    auto result = p;
    append(result.blocks[first], result.blocks.at(*second));
    result.blocks.erase(*second);
    result.core_of.erase(*second);
    result.order.erase(std::find(result.order.begin(), result.order.end(), *second));
    result.validate(g);
    return result;
}
std::pair<Id, Id> insertion_bounds(const GraphInfo &g, const Plan &p, Id block) {
    std::map<Id, Id> positions;
    Id n = 0;
    for (Id b : p.order) {
        if (b != block) {
            positions[b] = n++;
        }
    }
    auto [preds, succs] = block_dependencies(g, p.blocks);
    Id lower = 0, upper = n;
    for (Id b : preds.at(block)) {
        lower = std::max(lower, positions.at(b) + 1);
    }
    for (Id b : succs.at(block)) {
        upper = std::min(upper, positions.at(b));
    }
    return {lower, upper};
}
std::optional<Plan> move_block(const GraphInfo &g, const Plan &p, Id block, Id gap) {
    auto [lower, upper] = insertion_bounds(g, p, block);
    if (gap < lower || gap > upper) {
        return {};
    }
    auto result = p;
    result.order.erase(std::find(result.order.begin(), result.order.end(), block));
    result.order.insert(result.order.begin() + gap, block);
    return result;
}
// 用块顺序估算张量存活区间和内存压力；这不是最终的官方物理内存峰值。
static Json lifetime_proxy_impl(const GraphInfo &g, const Plan &p, const Capacity &capacity,
                                bool detailed) {
    auto owner = p.owner();
    std::map<Id, Id> position, local_position, local_count;
    for (std::size_t i = 0; i < p.order.size(); ++i) {
        Id b = p.order[i];
        position[b] = Id(i);
        local_position[b] = local_count[p.core_of.at(b)]++;
    }
    Json intervals = array(), peak = array();
    Id spans = 0, excess = 0;
    std::map<std::pair<Id, std::string>, std::map<Id, Id>> events;
    for (const auto &[tid, tensor] : g.tensors) {
        std::map<Id, IdSet> prod, cons;
        IdSet cores;
        for (Id v : g.eligible_producers.at(tid)) {
            Id c = p.core_of.at(owner.at(v));
            prod[c].insert(owner.at(v));
            cores.insert(c);
        }
        for (Id v : g.eligible_consumers.at(tid)) {
            Id c = p.core_of.at(owner.at(v));
            cons[c].insert(owner.at(v));
            cores.insert(c);
        }
        auto pos = field_string(tensor, "pos");
        if (pos == "DDR") {
            pos = "UB";
        }
        if (!capacity.count(pos)) {
            continue;
        }
        Id size = field_id(tensor, "size");
        for (Id c : cores) {
            const auto &ps = prod[c];
            const auto &cs = cons[c];
            IdSet endpoints = ps;
            endpoints.insert(cs.begin(), cs.end());
            const auto &starts = ps.empty() ? cs : ps;
            if (starts.empty()) {
                continue;
            }
            Id first = Id(p.order.size()), last = 0;
            for (Id b : starts) {
                first = std::min(first, position.at(b));
            }
            for (Id b : endpoints) {
                last = std::max(last, position.at(b));
            }
            Id a = local_position.at(p.order[first]), z = local_position.at(p.order[last]);
            Id span = size * (z - a + 1);
            spans += span;
            events[{c, pos}][a] += size;
            events[{c, pos}][z + 1] -= size;
            if (detailed) {
                Ids last_blocks;
                for (Id b : endpoints) {
                    if (position.at(b) == last) {
                        last_blocks.push_back(b);
                    }
                }
                intervals.push_back({{"tensor_id", tid},
                                     {"core_id", c},
                                     {"position", pos},
                                     {"size", size},
                                     {"first", first},
                                     {"last", last},
                                     {"first_local", a},
                                     {"last_local", z},
                                     {"span_bytes", span},
                                     {"producer_blocks", ps},
                                     {"consumer_blocks", cs},
                                     {"last_blocks", last_blocks}});
            }
        }
    }
    for (const auto &[location, sequence] : events) {
        Id live = 0, last = 0, maximum = 0;
        for (auto [index, delta] : sequence) {
            excess += std::max<Id>(0, live - capacity.at(location.second)) * (index - last);
            live += delta;
            maximum = std::max(maximum, live);
            last = index;
        }
        if (detailed) peak.push_back(
            {{"core_id", location.first}, {"position", location.second}, {"bytes", maximum}});
    }
    std::stable_sort(intervals.begin(), intervals.end(), [](const Json &a, const Json &b) {
        return std::make_tuple(-field_id(a, "span_bytes"), field_id(a, "tensor_id"),
                               field_id(a, "core_id")) < std::make_tuple(-field_id(b, "span_bytes"),
                                                                         field_id(b, "tensor_id"),
                                                                         field_id(b, "core_id"));
    });
    return {{"score", {excess, spans}},
            {"excess_area", excess},
            {"span_bytes", spans},
            {"peak", peak},
            {"intervals", intervals}};
}
Json lifetime_proxy_details(const GraphInfo &g, const Plan &p, const Capacity &capacity) {
    return lifetime_proxy_impl(g, p, capacity, true);
}
std::pair<Id, Id> lifetime_proxy(const GraphInfo &g, const Plan &p, const Capacity &capacity) {
    auto value = lifetime_proxy_impl(g, p, capacity, false);
    return {field_id(value, "excess_area"), field_id(value, "span_bytes")};
}
// 在依赖允许的插入区间内微调块顺序，降低生命周期重叠和换页风险。
std::optional<Plan> repair_order(const GraphInfo &g, const Plan &p, const Settings &settings,
                                 const Json &profile, const std::optional<IdSet> &affected,
                                 Id max_moves) {
    Plan current = p;
    for (Id step = 0; step < max_moves; ++step) {
        Json intervals = lifetime_proxy_details(g, current, settings.capacity).at("intervals");
        std::map<std::pair<Id, Id>, Id> priorities;
        if (profile.is_object()) {
            for (const auto &row : profile.value("lifetime_hotspots", array())) {
                priorities[{field_id(row, "tensor_id"), field_id(row, "core")}] =
                    row.value("spill_bytes", Id(0));
            }
        }
        if (!priorities.empty()) {
            std::stable_sort(intervals.begin(), intervals.end(), [&](const Json &a, const Json &b) {
                return std::make_pair(
                           -priorities[{field_id(a, "tensor_id"), field_id(a, "core_id")}],
                           -field_id(a, "span_bytes")) <
                       std::make_pair(
                           -priorities[{field_id(b, "tensor_id"), field_id(b, "core_id")}],
                           -field_id(b, "span_bytes"));
            });
        }
        if (step == 0 && profile.is_object() &&
            !profile.value("lifetime_hotspots", array()).empty()) {
            Json official = array();
            auto owner = current.owner();
            std::map<Id, Id> positions;
            for (std::size_t i = 0; i < current.order.size(); ++i) {
                positions[current.order[i]] = Id(i);
            }
            for (const auto &row : profile.at("lifetime_hotspots")) {
                Ids controls;
                for (auto name : {"producer_blocks", "consumer_blocks"}) {
                    for (const auto &b : row.at(name)) {
                        if (positions.count(number(b))) {
                            controls.push_back(number(b));
                        }
                    }
                }
                if (controls.empty()) {
                    continue;
                }
                IdSet ps, cs;
                for (const auto &v : row.value("producer_ops", array())) {
                    if (owner.count(number(v))) {
                        ps.insert(owner.at(number(v)));
                    }
                }
                for (const auto &v : row.value("consumer_ops", array())) {
                    if (owner.count(number(v))) {
                        cs.insert(owner.at(number(v)));
                    }
                }
                Id first = Id(current.order.size()), last = 0;
                for (Id b : controls) {
                    first = std::min(first, positions.at(b));
                    last = std::max(last, positions.at(b));
                }
                Ids last_blocks;
                if (row.contains("last_use_block") && !row.at("last_use_block").is_null() &&
                    positions.count(field_id(row, "last_use_block")) &&
                    row.contains("last_use_op") && !row.at("last_use_op").is_null() &&
                    g.eligible.count(field_id(row, "last_use_op"))) {
                    last_blocks.push_back(field_id(row, "last_use_block"));
                }
                official.push_back(
                    {{"tensor_id", row.at("tensor_id")},
                     {"core_id", row.at("core")},
                     {"position", row.at("pos")},
                     {"size", row.at("size")},
                     {"first", first},
                     {"last", last},
                     {"span_bytes", field_id(row, "size") * (field_id(row, "last_index") -
                                                             field_id(row, "first_index") + 1)},
                     {"producer_blocks", ps},
                     {"consumer_blocks", cs},
                     {"last_blocks", last_blocks}});
            }
            if (!official.empty()) {
                intervals = official;
            }
        }
        std::vector<Plan> candidates;
        std::set<Ids> seen;
        for (std::size_t h = 0;
             h < std::min<std::size_t>(8, intervals.size()) && candidates.size() < 24; ++h) {
            const auto &hot = intervals[h];
            Id first = field_id(hot, "first"), last = field_id(hot, "last");
            if (first == last) {
                continue;
            }
            std::map<Id, Id> positions;
            for (std::size_t i = 0; i < current.order.size(); ++i) {
                positions[current.order[i]] = Id(i);
            }
            IdSet relevant = hot.at("producer_blocks").get<IdSet>();
            auto consumers = hot.at("consumer_blocks").get<IdSet>();
            relevant.insert(consumers.begin(), consumers.end());
            if (affected) {
                bool intersects = false;
                for (Id b : relevant) {
                    if (affected->count(b)) {
                        intersects = true;
                    }
                }
                if (!intersects) {
                    continue;
                }
            }
            std::vector<std::pair<Id, std::string>> targets;
            for (const auto &b : hot.at("last_blocks")) {
                if (consumers.count(number(b))) {
                    targets.emplace_back(number(b), "early");
                }
            }
            for (const auto &b : hot.at("producer_blocks")) {
                targets.emplace_back(number(b), "late");
            }
            Ids middle;
            for (Id i = first + 1; i < last; ++i) {
                Id b = current.order[i];
                if (current.core_of.at(b) == field_id(hot, "core_id") && !relevant.count(b)) {
                    middle.push_back(b);
                }
            }
            if (!middle.empty()) {
                auto footprint = [&](Id b) {
                    IdSet tensors;
                    for (Id v : current.blocks.at(b)) {
                        for (Id tid : g.op_outputs.at(v)) {
                            tensors.insert(tid);
                        }
                    }
                    Id size = 0;
                    for (Id tid : tensors) {
                        size += field_id(g.tensors.at(tid), "size");
                    }
                    return std::make_pair(size, -b);
                };
                targets.emplace_back(
                    *std::max_element(middle.begin(), middle.end(),
                                      [&](Id a, Id b) { return footprint(a) < footprint(b); }),
                    "outside");
            }
            for (std::size_t t = 0;
                 t < std::min<std::size_t>(6, targets.size()) && candidates.size() < 24; ++t) {
                auto [block, direction] = targets[t];
                auto [lower, upper] = insertion_bounds(g, current, block);
                Ids local;
                for (Id b : current.order) {
                    if (current.core_of.at(b) == current.core_of.at(block)) {
                        local.push_back(b);
                    }
                }
                Id index = Id(std::find(local.begin(), local.end(), block) - local.begin()),
                   left = positions.at(local[std::max<Id>(0, index - 16)]),
                   right = positions.at(local[std::min<Id>(Id(local.size()) - 1, index + 16)]);
                IdSet gaps;
                if (direction == "early") {
                    gaps = {lower, std::max(lower, left), std::max(lower, first + 1)};
                } else if (direction == "late") {
                    Id use = upper;
                    bool found = false;
                    for (Id b : consumers) {
                        if (!found || positions.at(b) < use) {
                            use = positions.at(b);
                        }
                        found = true;
                    }
                    gaps = {upper, std::min(upper, right),
                            std::min(upper, std::max<Id>(0, use - 1))};
                } else {
                    gaps = {std::max(lower, first), std::min(upper, last)};
                }
                for (Id gap : gaps) {
                    auto candidate = move_block(g, current, block, gap);
                    if (!candidate || candidate->order == current.order ||
                        !seen.insert(candidate->order).second) {
                        continue;
                    }
                    candidates.push_back(*candidate);
                    if (candidates.size() >= 24) {
                        break;
                    }
                }
            }
        }
        if (candidates.empty()) {
            break;
        }
        std::vector<std::pair<Id, Id>> life_scores;
        std::vector<std::size_t> indices(candidates.size());
        std::iota(indices.begin(), indices.end(), 0);
        life_scores.reserve(candidates.size());
        for (const auto &candidate : candidates)
            life_scores.push_back(lifetime_proxy(g, candidate, settings.capacity));
        std::stable_sort(indices.begin(), indices.end(), [&](std::size_t a, std::size_t b) {
            return life_scores[a] < life_scores[b];
        });
        if (indices.size() > 3) indices.resize(3);
        std::optional<std::tuple<double, Id, Id>> best_score;
        std::size_t best_index = indices.front();
        for (std::size_t index : indices) {
            const auto &plan = candidates[index];
            auto score = std::make_tuple(
                std::max(replay_sketch(g, plan, settings), route_bytes(g, plan) / settings.bandwidth),
                life_scores[index].first, life_scores[index].second);
            if (!best_score || score < *best_score) {
                best_score = score;
                best_index = index;
            }
        }
        current = std::move(candidates[best_index]);
    }
    if (current.fingerprint() == p.fingerprint()) {
        return {};
    }
    current.validate(g);
    return current;
}
} // namespace problem2
