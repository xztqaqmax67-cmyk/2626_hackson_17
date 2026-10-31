#include "search_limits.hpp"
#include "slack.hpp"

namespace dag_chain {
double slack_exposed_delay(double arrival, double consumer_ready) {
    return max(0.0, arrival - consumer_ready);
}

double slack_uncontended_arrival(double producer_finish, i64 bytes, const Config &config) {
    const double copy = max(1.0, ceil(double(bytes) / config.bandwidth));
    // The rule waits AFTER COPY_OUT finishes and BEFORE COPY_IN starts.
    // Pipe contention and globally shared DDR are deliberately not certified by
    // this cheap filter; shortlisted moves are replayed on the complete graph.
    return producer_finish + copy + config.delay + copy;
}

namespace {
struct Boundary {
    int consumer = -1;
    double release = 0;
    i64 bytes = 0;
};
struct Group {
    vector<int> members, consumers;
    vector<Boundary> boundary;
    double work = 0, margin = -INF;
    int source = -1;
};
struct Replay {
    vector<double> start, finish;
    double cycles = 0, bytes = 0;
};

Replay replay(const Graph &g, int k, const Config &cfg, const vector<int> &assignment,
              const vector<int> &order) {
    Placement placement(g, k, cfg);
    Replay result;
    result.start.resize(g.size());
    result.finish.resize(g.size());
    for (int v : order) {
        auto trial = placement.trial(v, assignment[v]);
        result.start[v] = trial.start;
        result.finish[v] = trial.end;
        placement.commit(trial);
    }
    result.cycles = placement.makespan();
    result.bytes = placement.bytes;
    return result;
}
} // namespace

vector<Solution> slack_candidates(const Graph &g, int k, const Solution &seed,
                                  SlackStats *statistics) {
    SlackStats stats;
    vector<Solution> result;
    if (k < 1 || k > 5)
        throw runtime_error("slack core count must be 1..5");
    validate(g, seed, k);
    const size_t graph_work = size_t(g.size()) + g.edge_count + g.tensors.size();
    if (k == 1 || graph_work > search_limits::large_graph_work || g.size() == 0) {
        if (statistics)
            *statistics = stats;
        return result;
    }
    const Config cfg;
    const Replay original = replay(g, k, cfg, seed.core, seed.order);
    vector<Group> groups(g.size());
    for (int v : g.topo) {
        auto &group = groups[g.writer_group[v]];
        group.members.push_back(v);
        group.source = seed.core[v];
        group.work += g.weight(v);
    }

    auto add_boundary = [&](int group, int consumer, double release, i64 bytes) {
        if (group == g.writer_group[consumer])
            return;
        groups[group].boundary.push_back({consumer, release, bytes});
        groups[group].consumers.push_back(consumer);
    };
    // All writers of a Tensor have one writer-group root. Its COPY_OUT cannot
    // start before the final producer finishes, even if the chosen edge starts
    // at an earlier partial writer.
    for (const auto &tensor : g.tensors) {
        if (tensor.producers.empty())
            continue;
        const int group = g.writer_group[tensor.producers.front()];
        double release = 0;
        for (int v : tensor.producers)
            release = max(release, original.finish[v]);
        for (int consumer : tensor.consumers)
            add_boundary(group, consumer, release, tensor.size);
    }
    for (const auto &edge : g.direct)
        add_boundary(g.writer_group[edge.a], edge.b, original.finish[edge.a], edge.size);
    // Also retain consumers reached through contracted original COPY nodes.
    // Duplicate boundaries only repeat a max test; they never remove real edges.
    for (int v : g.topo)
        for (int consumer : g.ops[v].succ)
            add_boundary(g.writer_group[v], consumer, original.finish[v], 0);

    vector<int> selected;
    for (int id = 0; id < g.size(); ++id) {
        auto &group = groups[id];
        if (group.members.empty())
            continue;
        ++stats.groups_examined;
        Graph::unique_vec(group.consumers);
        for (const auto &edge : group.boundary) {
            if (seed.core[edge.consumer] != group.source)
                continue;
            group.margin =
                max(group.margin, original.start[edge.consumer] -
                                      slack_uncontended_arrival(edge.release, edge.bytes, cfg));
        }
        // Only release preferences with consumer-specific waiting room. A slow
        // unrelated core can never create margin in this test.
        if (group.margin >= 0)
            selected.push_back(id);
    }
    stable_sort(selected.begin(), selected.end(), [&](int a, int b) {
        return make_tuple(-groups[a].work, -groups[a].margin, a) <
               make_tuple(-groups[b].work, -groups[b].margin, b);
    });
    const size_t group_limit = graph_work > search_limits::medium_graph_work
                                   ? search_limits::slack_groups_medium
                                   : search_limits::slack_groups_small;
    if (selected.size() > group_limit)
        selected.resize(group_limit);
    if (selected.empty()) {
        if (statistics)
            *statistics = stats;
        return result;
    }

    // Estimate destination readiness at the actual position in the seed order,
    // not at the end of the entire core. For a multi-writer group the largest
    // shift is applied to its boundary as a screening estimate, not a bound.
    vector<bool> chosen_group(g.size());
    vector<vector<double>> shifts(g.size());
    for (int id : selected) {
        chosen_group[id] = true;
        shifts[id].assign(k, -INF);
    }
    Placement prefix(g, k, cfg);
    for (int v : seed.order) {
        const int id = g.writer_group[v];
        if (chosen_group[id]) {
            for (int core = 0; core < k; ++core) {
                if (core == groups[id].source)
                    continue;
                const auto trial = prefix.trial(v, core);
                shifts[id][core] = max(shifts[id][core], trial.end - original.finish[v]);
            }
        }
        prefix.commit(prefix.trial(v, seed.core[v]));
    }

    struct Move {
        int group, core;
        double score, bytes;
    };
    vector<Move> moves;
    TrafficState traffic(g, cfg, k, seed.core);
    for (int id : selected) {
        const auto &group = groups[id];
        for (int core = 0; core < k; ++core) {
            if (core == group.source)
                continue;
            ++stats.moves_screened;
            double exposed = 0;
            for (const auto &edge : group.boundary) {
                double arrival = max(0.0, edge.release + shifts[id][core]);
                if (core != seed.core[edge.consumer])
                    arrival = slack_uncontended_arrival(arrival, edge.bytes, cfg);
                exposed = max(exposed, slack_exposed_delay(arrival, original.start[edge.consumer]));
            }
            if (exposed > 1e-9)
                continue;
            for (int v : group.members)
                traffic.move_op(v, core);
            moves.push_back({id, core, traffic.light_score(0.03), traffic.bytes});
            for (int v : group.members)
                traffic.move_op(v, group.source);
        }
    }
    stable_sort(moves.begin(), moves.end(), [](const Move &a, const Move &b) {
        return tie(a.score, a.bytes, a.group, a.core) < tie(b.score, b.bytes, b.group, b.core);
    });
    const size_t replay_limit = graph_work > search_limits::medium_graph_work
                                    ? search_limits::slack_replays_medium
                                    : search_limits::slack_replays_small;
    if (moves.size() > replay_limit)
        moves.resize(replay_limit);

    struct Candidate {
        vector<int> core;
        double score, bytes;
        int group, destination;
    };
    vector<Candidate> candidates;
    for (const auto &move : moves) {
        const auto &group = groups[move.group];
        auto assignment = seed.core;
        for (int v : group.members)
            assignment[v] = move.core;
        const Replay changed = replay(g, k, cfg, assignment, seed.order);
        ++stats.replayed;
        double exposed = 0;
        for (int consumer : group.consumers)
            exposed = max(exposed,
                          slack_exposed_delay(changed.start[consumer], original.start[consumer]));
        if (exposed > 1e-9)
            continue;
        // The full fixed-order replay catches input transfers and Pipe changes
        // that the boundary filter did not model. Shared DDR and spill still
        // require the caller's full refinement, and official legality checks.
        const double score =
            max(changed.cycles, move.bytes / cfg.bandwidth) + 0.03 * move.bytes / cfg.bandwidth;
        candidates.push_back({std::move(assignment), score, move.bytes, move.group, move.core});
    }
    stable_sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
        return tie(a.score, a.bytes, a.group, a.destination) <
               tie(b.score, b.bytes, b.group, b.destination);
    });
    if (candidates.size() > search_limits::slack_assignments)
        candidates.resize(search_limits::slack_assignments);
    set<pair<vector<int>, vector<int>>> seen;
    for (const auto &candidate : candidates) {
        for (const string &mode : {string("critical"), string("dfs")}) {
            // Reordering may change the screened waiting room. It is only a
            // proposal: retain the caller's incumbent and compare full refined
            // results instead of claiming this local test proves improvement.
            SeedOptions options;
            options.mode = mode;
            options.window = 8;
            options.memory = 0.5;
            auto solution = operation_plan(g, k, cfg, options, &candidate.core, nullptr);
            solution.label = "slack_group_" + to_string(candidate.group) + "_core_" +
                             to_string(candidate.destination) + "_" + mode;
            validate(g, solution, k);
            if (seen.insert({solution.core, solution.order}).second)
                result.push_back(move(solution));
        }
    }
    stats.candidates = result.size();
    if (statistics)
        *statistics = stats;
    return result;
}
} // namespace dag_chain
