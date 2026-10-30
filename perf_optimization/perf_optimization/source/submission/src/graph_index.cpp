// Graph indexing for the solution generator. No evaluation or scheduling simulation.
#include "graph_index.hpp"
namespace problem2 {
void validate_graph(const Json &graph) {
    if (!graph.is_object()) throw std::invalid_argument("graph must be an object");
    IdSet ids, op_ids;
    for (auto name : {"tensors", "ops", "edges"}) {
        if (!graph.at(name).is_array()) throw std::invalid_argument("graph arrays missing");
    }
    for (auto name : {"tensors", "ops"}) {
        for (const auto &row : graph.at(name)) {
            if (!row.at("id").is_number_integer() || field_id(row) < 0 || !ids.insert(field_id(row)).second)
                throw std::invalid_argument("invalid or duplicate node id");
            if (std::string(name) == "ops") {
                op_ids.insert(field_id(row));
                const auto pipe = field_string(row, "pipe");
                if (std::find(PIPES.begin(), PIPES.end(), pipe) == PIPES.end())
                    throw std::invalid_argument("unknown operation pipe");
                if (!row.at("cycles").is_number_integer() || (!is_copy(row) && field_id(row, "cycles") < 0))
                    throw std::invalid_argument("invalid compute cycles");
            } else {
                const auto position = field_string(row, "pos");
                if (position != "DDR" && position != "L1" && position != "UB")
                    throw std::invalid_argument("unknown tensor location");
                if (!row.at("size").is_number_integer() || field_id(row, "size") < 0)
                    throw std::invalid_argument("invalid tensor size");
            }
        }
    }
    for (const auto &edge : graph.at("edges")) {
        Id a = field_id(edge, "source"), b = field_id(edge, "target");
        if (!ids.count(a) || !ids.count(b) || op_ids.count(a) == op_ids.count(b))
            throw std::invalid_argument("edges must connect an operation and a tensor");
    }
}
GraphViews build_graph_views(const Json &ops, const Json &edges) {
    GraphViews view;
    IdSet op_ids;
    for (const auto &op : ops) {
        Id id = field_id(op);
        op_ids.insert(id);
        view.inputs[id]; view.outputs[id]; view.preds[id]; view.succs[id];
    }
    for (const auto &edge : edges) {
        Id a = field_id(edge, "source"), b = field_id(edge, "target");
        if (op_ids.count(a)) { view.outputs[a].push_back(b); view.producers[b].insert(a); }
        else { view.inputs[b].push_back(a); view.consumers[a].insert(b); }
    }
    for (const auto &[tensor, writers] : view.producers) {
        for (Id writer : writers) for (Id reader : view.consumers[tensor]) {
            view.preds[reader].insert(writer);
            view.succs[writer].insert(reader);
        }
    }
    for (auto *index : {&view.inputs, &view.outputs}) for (auto &[id, values] : *index) {
        std::sort(values.begin(), values.end());
        values.erase(std::unique(values.begin(), values.end()), values.end());
    }
    return view;
}
std::pair<Adjacency, Adjacency> contract_excluded_copy_nodes(const Ids &ids, const Adjacency &all) {
    const auto eligible = as_set(ids);
    Adjacency preds, succs;
    for (Id id : ids) { preds[id]; succs[id]; }
    for (Id source : ids) {
        IdSet visited;
        Ids stack(all.at(source).begin(), all.at(source).end());
        while (!stack.empty()) {
            Id node = stack.back(); stack.pop_back();
            if (!visited.insert(node).second) continue;
            if (eligible.count(node)) { succs[source].insert(node); preds[node].insert(source); }
            else { const auto &next = all.at(node); stack.insert(stack.end(), next.begin(), next.end()); }
        }
    }
    return {preds, succs};
}
}
