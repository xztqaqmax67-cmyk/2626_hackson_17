// 对应 official_code/schedule_step1.py。
// 步骤 1：从图依赖生成固定的算子调度序列。
#include "schedule_step1.hpp"
namespace problem2 {
GraphViews build_graph_views(const Json &ops, const Json &edges) {
    GraphViews view;
    IdSet ids;
    for (const auto &op : ops) {
        Id id = field_id(op);
        ids.insert(id);
        view.inputs[id];
        view.outputs[id];
        view.preds[id];
        view.succs[id];
    }
    for (const auto &e : edges) {
        Id a = field_id(e, "source"), b = field_id(e, "target");
        if (ids.count(a) && !ids.count(b)) {
            view.outputs[a].push_back(b);
            view.producers[b].insert(a);
        } else if (!ids.count(a) && ids.count(b)) {
            view.inputs[b].push_back(a);
            view.consumers[a].insert(b);
        } else if (ids.count(a) && ids.count(b) && a != b) {
            view.preds[b].insert(a);
            view.succs[a].insert(b);
        }
    }
    for (const auto &[op, tids] : view.inputs) {
        for (Id tid : tids) {
            for (Id producer : view.producers[tid]) {
                if (producer != op) {
                    view.preds[op].insert(producer);
                    view.succs[producer].insert(op);
                }
            }
        }
    }
    return view;
}
std::map<Id, Id> precompute_depth(const Adjacency &preds, const Adjacency &succs,
                                  const Ids &nodes) {
    std::map<Id, Id> depth;
    std::deque<Id> queue;
    for (Id v : nodes) {
        if (preds.at(v).empty()) {
            depth[v] = 0;
            queue.push_back(v);
        }
    }
    while (!queue.empty()) {
        Id u = queue.front();
        queue.pop_front();
        for (Id w : succs.at(u)) {
            if (!depth.count(w) || depth[u] + 1 > depth[w]) {
                depth[w] = depth[u] + 1;
                queue.push_back(w);
            }
        }
    }
    return depth;
}
Ids step1_from_adj(const Adjacency &preds, const Adjacency &succs, const Ids &nodes,
                   const Json &ops) {
    if (nodes.empty()) {
        return {};
    }
    auto depth = precompute_depth(preds, succs, nodes);
    auto by_id = index_records(ops);
    auto order_key = [&](Id v, bool start) {
        bool copy = by_id.count(v) && by_id.at(v).at("op") == (start ? "COPY_OUT" : "COPY_IN");
        return std::make_tuple(!copy, depth.at(v), -v);
    };
    auto sorted = [&](Ids values, bool start) {
        std::stable_sort(values.begin(), values.end(),
                         [&](Id a, Id b) { return order_key(a, start) < order_key(b, start); });
        return values;
    };
    Ids outputs;
    for (Id v : nodes) {
        if (succs.at(v).empty()) {
            outputs.push_back(v);
        }
    }
    if (outputs.empty()) {
        outputs.push_back(*std::min_element(nodes.begin(), nodes.end()));
    }
    Ids stack = sorted(outputs, true), sequence;
    IdSet visited;
    // 反向 DFS：先把前驱压栈，等所有前驱已访问后再输出当前节点。
    while (!stack.empty() || visited.size() < nodes.size()) {
        poll_search_deadline();
        if (stack.empty()) {
            Ids rest;
            for (Id v : nodes) {
                if (!visited.count(v)) {
                    rest.push_back(v);
                }
            }
            append(stack, sorted(rest, false));
            continue;
        }
        Id u = stack.back();
        if (visited.count(u)) {
            stack.pop_back();
            continue;
        }
        Ids remaining;
        for (Id p : preds.at(u)) {
            if (!visited.count(p)) {
                remaining.push_back(p);
            }
        }
        if (!remaining.empty()) {
            append(stack, sorted(remaining, false));
        } else {
            visited.insert(u);
            sequence.push_back(u);
            stack.pop_back();
        }
    }
    return sequence;
}
Ids step1_schedule(const Json &graph) {
    auto v = build_graph_views(graph.at("ops"), graph.at("edges"));
    Ids ids;
    for (const auto &o : graph.at("ops")) {
        ids.push_back(field_id(o));
    }
    return step1_from_adj(v.preds, v.succs, ids, graph.at("ops"));
}
bool check_topo(const Json &graph, const Ids &sequence) {
    auto v = build_graph_views(graph.at("ops"), graph.at("edges"));
    std::map<Id, std::size_t> pos;
    for (std::size_t i = 0; i < sequence.size(); ++i) {
        pos[sequence[i]] = i;
    }
    for (const auto &[a, targets] : v.succs) {
        for (Id b : targets) {
            if (pos[a] > pos[b]) {
                return false;
            }
        }
    }
    return true;
}
} // namespace problem2
