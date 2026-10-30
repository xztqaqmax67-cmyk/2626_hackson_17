#include "spill_mapping.hpp"
#include "search_limits.hpp"

namespace dag_chain {
vector<Solution> spill_mapping_candidates(const Graph &g, int cores,
                                           const Solution &seed,
                                           const OfficialSpillTrace &trace) {
    vector<Solution> result;
    if (!trace.valid || !search_limits::feedback_mapping_candidates ||
        !search_limits::feedback_mapping_records || !search_limits::feedback_mapping_nodes)
        return result;
    validate(g, seed, cores);
    const Config cfg;
    // A move must preserve both mandatory writer atoms and the selected v9
    // co-location groups. Union them before taking closures so overlapping
    // constraints are transitive; only one pass over the roots would be unsafe.
    DSU closed_groups(g.size());
    for (int v = 0; v < g.size(); ++v) closed_groups.join(v, g.writer_group[v]);
    if (!seed.placement_group.empty()) {
        vector<int> representative(g.size(), -1);
        for (int v = 0; v < g.size(); ++v) {
            int &first = representative[seed.placement_group[v]];
            if (first < 0) first = v;
            else closed_groups.join(first, v);
        }
    }
    vector<vector<int>> atoms(g.size());
    for (int v = 0; v < g.size(); ++v) atoms[closed_groups.get(v)].push_back(v);
    vector<size_t> records(trace.records.size());
    iota(records.begin(), records.end(), size_t(0));
    stable_sort(records.begin(), records.end(), [&](size_t a, size_t b) {
        const auto &x = trace.records[a], &y = trace.records[b];
        if (x.transfer_bytes() != y.transfer_bytes()) return x.transfer_bytes() > y.transfer_bytes();
        return tie(x.core, x.tensor_id, x.trigger_step, a) < tie(y.core, y.tensor_id, y.trigger_step, b);
    });
    // One representative per core/tensor prevents repeated evictions of one
    // tensor from consuming every mapping neighborhood.
    set<pair<int, i64>> hot_tensors;
    struct Move { double score; int source, target; size_t record; vector<int> nodes; };
    vector<Move> moves;
    set<pair<vector<int>, int>> seen;
    TrafficState ledger(g, cfg, cores, seed.core);
    auto close_groups = [&](const vector<int> &roots, int source) {
        vector<int> groups, nodes;
        for (int v : roots)
            if (v >= 0 && v < g.size() && seed.core[v] == source) groups.push_back(closed_groups.get(v));
        Graph::unique_vec(groups);
        for (int group : groups) {
            const auto &atom = atoms[group];
            if (nodes.size() + atom.size() > search_limits::feedback_mapping_nodes) return vector<int>{};
            nodes.insert(nodes.end(), atom.begin(), atom.end());
        }
        Graph::unique_vec(nodes);
        return nodes;
    };
    for (size_t index : records) {
        const auto &event = trace.records[index];
        if (event.transfer_bytes() <= 0 || event.core < 0 || event.core >= cores) continue;
        if (hot_tensors.count({event.core, event.tensor_id})) continue;
        if (hot_tensors.size() >= search_limits::feedback_mapping_records) break;
        hot_tensors.insert({event.core, event.tensor_id});
        vector<vector<int>> neighborhoods;
        neighborhoods.push_back(close_groups({event.trigger_owner}, event.core));
        if (event.tensor_index >= 0 && event.tensor_index < int(g.tensors.size()))
            neighborhoods.push_back(close_groups(g.tensor_users[event.tensor_index], event.core));
        else neighborhoods.push_back(close_groups({event.prev_owner, event.next_owner}, event.core));
        for (const auto &nodes : neighborhoods) {
            if (nodes.empty()) continue;
            for (int target = 0; target < cores; ++target) {
                if (target == event.core || !seen.insert({nodes, target}).second) continue;
                for (int v : nodes) ledger.move_op(v, target);
                // This is only a ranking hint. The moved work may reduce local
                // overlap, but it may also create new route traffic and stalls.
                // Full Step2 and cycles screening remain mandatory downstream.
                const double relief = double(event.transfer_bytes());
                const double score = ledger.light_score(0.03) - relief / cfg.bandwidth;
                moves.push_back({score, event.core, target, index, nodes});
                for (auto it = nodes.rbegin(); it != nodes.rend(); ++it)
                    ledger.move_op(*it, seed.core[*it]);
            }
        }
    }
    stable_sort(moves.begin(), moves.end(), [](const Move &a, const Move &b) {
        return tie(a.score, a.source, a.target, a.record, a.nodes) <
               tie(b.score, b.source, b.target, b.record, b.nodes);
    });
    set<vector<int>> assignments;
    set<pair<int, i64>> selected_hotspots;
    // Prefer different spill locations before spending spare slots on another
    // destination for the same hotspot. No candidate is accepted here.
    for (int pass = 0; pass < 2; ++pass)
        for (const auto &move : moves) {
            if (result.size() >= search_limits::feedback_mapping_candidates) return result;
            const auto &event = trace.records[move.record];
            const pair<int, i64> hotspot{event.core, event.tensor_id};
            if (!pass && selected_hotspots.count(hotspot)) continue;
            Solution candidate = seed;
            for (int v : move.nodes) candidate.core[v] = move.target;
            if (candidate.core == seed.core || !assignments.insert(candidate.core).second) continue;
            validate(g, candidate, cores);
            candidate.label = "spill_event_mapping_" + to_string(move.record) + "_" + to_string(move.target);
            selected_hotspots.insert(hotspot);
            result.push_back(std::move(candidate));
        }
    return result;
}
} // namespace dag_chain
