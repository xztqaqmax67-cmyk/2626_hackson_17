#include "annealing.hpp"

namespace dag_chain {
namespace {
// SplitMix64 and explicit conversions avoid implementation-dependent standard
// distributions. No clock, thread ID, input filename or public-case identifier.
struct Random {
    uint64_t state;
    uint64_t next() {
        uint64_t x = (state += 0x9e3779b97f4a7c15ULL);
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        return x ^ (x >> 31);
    }
    size_t bounded(size_t count) {
        const uint64_t bound = uint64_t(count), threshold = (uint64_t(0) - bound) % bound;
        uint64_t value;
        do { value = next(); } while (value < threshold);
        return size_t(value % bound);
    }
    double unit() { return double(next() >> 11) * (1.0 / 9007199254740992.0); }
};
struct Metrics {
    double energy = 0, bytes = 0, pipe = 0;
};
struct State {
    vector<int> core;
    Metrics metrics;
    string origin;
};
vector<int> canonical(const vector<int> &assignment, int cores) {
    vector<int> labels(cores, -1), key; key.reserve(assignment.size());
    int next = 0;
    for (int c : assignment) {
        if (labels[c] < 0) labels[c] = next++;
        key.push_back(labels[c]);
    }
    return key;
}
void normalize_zero_cycles(const Graph &g, TrafficState &state) {
    for (int v = 0; v < g.size(); ++v)
        state.load[state.core[v]][g.ops[v].pipe] += g.weight(v) - double(g.ops[v].cycles);
}
void move_node(const Graph &g, TrafficState &state, int v, int target) {
    const int source = state.core[v];
    if (source == target) return;
    state.move_op(v, target);
    const double correction = g.weight(v) - double(g.ops[v].cycles);
    state.load[source][g.ops[v].pipe] -= correction;
    state.load[target][g.ops[v].pipe] += correction;
}
Metrics measure(const TrafficState &state, const Row &total, const AnnealingLimits &limits) {
    double square = 0;
    for (const auto &row : state.load) for (int p = 0; p < 5; ++p)
        square += row[p] * row[p] / max(1.0, total[p]);
    Metrics result;
    result.bytes = state.bytes; result.pipe = state.pipe_bound();
    result.energy = max(result.pipe, result.bytes / state.cfg.bandwidth) +
        limits.route_weight * result.bytes / state.cfg.bandwidth + limits.balance_weight * square;
    if (!isfinite(result.energy) || !isfinite(result.bytes) || !isfinite(result.pipe))
        throw invalid_argument("annealing proxy energy is not finite");
    return result;
}
bool better(const State &a, const State &b) {
    return tie(a.metrics.energy, a.metrics.bytes, a.metrics.pipe, a.core) <
           tie(b.metrics.energy, b.metrics.bytes, b.metrics.pipe, b.core);
}
} // namespace

AnnealingResult anneal_assignment(const Graph &g, const Decomposition &decomposition,
    int cores, const vector<int> &greedy, const AnnealingLimits &limits, AnnealingStats *statistics, Deadline *deadline) {
    AnnealingStats stats;
    if (cores < 2 || cores > 5 || greedy.size() != size_t(g.size()))
        throw invalid_argument("annealing requires a complete 2..5-core assignment");
    if (limits.swap_percent > 100 || !isfinite(limits.route_weight) || limits.route_weight < 0 ||
        !isfinite(limits.balance_weight) || limits.balance_weight < 0 ||
        !isfinite(limits.start_temperature) || !isfinite(limits.end_temperature) ||
        limits.start_temperature <= 0 || limits.end_temperature <= 0 ||
        limits.end_temperature > limits.start_temperature)
        throw invalid_argument("invalid annealing weights or cooling parameters");
    for (int c : greedy) if (c < 0 || c >= cores) throw invalid_argument("invalid greedy core");
    const int n = g.size();
    DSU closure(n);
    vector<unsigned char> covered(n);
    for (const auto &bundle : decomposition.bundles) {
        for (int v : bundle) {
            if (v < 0 || v >= n || covered[v]) throw invalid_argument("invalid or duplicate decomposition member");
            covered[v] = 1;
            closure.join(bundle.front(), v);
        }
    }
    if (find(covered.begin(), covered.end(), 0) != covered.end())
        throw invalid_argument("decomposition does not cover every operation");
    for (int v = 0; v < n; ++v) closure.join(v, g.writer_group[v]);
    AnnealingResult result;
    result.placement_group.resize(n);
    map<int, int> index;
    vector<vector<int>> bundles;
    vector<int> current_core;
    for (int v : g.topo) {
        const int root = closure.get(v);
        auto [it, inserted] = index.emplace(root, int(index.size()));
        const int group = it->second;
        if (inserted) { bundles.push_back({}); current_core.push_back(greedy[v]); }
        if (greedy[v] != current_core[group])
            throw invalid_argument("greedy assignment splits a bundle/writer closure");
        bundles[group].push_back(v); result.placement_group[v] = group;
    }
    stats.bundles = bundles.size();
    Row total{};
    for (int v = 0; v < n; ++v) total[g.ops[v].pipe] += g.weight(v);
    const Config cfg;
    TrafficState ledger(g, cfg, cores, greedy);
    normalize_zero_cycles(g, ledger);
    const Metrics initial = measure(ledger, total, limits);
    State initial_state{current_core, initial, "greedy"}, best = initial_state,
          low_route = initial_state, low_pipe = initial_state;
    Metrics current = initial;
    stats.initial_energy = initial.energy; stats.initial_route_bytes = initial.bytes;
    stats.initial_pipe_bound = initial.pipe;
    const double scale = max(1.0, initial.energy);
    const double first_temperature = scale * limits.start_temperature;
    const double last_temperature = scale * limits.end_temperature;
    if (!isfinite(first_temperature) || !isfinite(last_temperature) || last_temperature <= 0)
        throw invalid_argument("annealing temperatures are outside floating-point range");
    stats.initial_temperature = stats.final_temperature = first_temperature;
    Random random{limits.seed};
    auto expand = [&](const vector<int> &by_bundle) {
        vector<int> assignment(n);
        for (size_t b = 0; b < bundles.size(); ++b)
            for (int v : bundles[b]) assignment[v] = by_bundle[b];
        return assignment;
    };
    auto observe = [&]() {
        // Most accepted states are not records. Compare borrowed vectors first
        // and allocate a snapshot only when a retained objective improves.
        if (tie(current.energy, current.bytes, current.pipe, current_core) <
            tie(best.metrics.energy, best.metrics.bytes, best.metrics.pipe, best.core))
            best = {current_core, current, "best_energy"};
        if (tie(current.bytes, current.energy, current.pipe, current_core) <
            tie(low_route.metrics.bytes, low_route.metrics.energy, low_route.metrics.pipe, low_route.core)) {
            low_route = {current_core, current, "low_route"};
        }
        if (tie(current.pipe, current.energy, current.bytes, current_core) <
            tie(low_pipe.metrics.pipe, low_pipe.metrics.energy, low_pipe.metrics.bytes, low_pipe.core)) {
            low_pipe = {current_core, current, "low_pipe"};
        }
    };
    auto rebase = [&]() {
        TrafficState reference(g, cfg, cores, ledger.core);
        normalize_zero_cycles(g, reference);
        // Counts must be restored exactly, including shared consumers and all
        // writers. Only floating totals are permitted tiny accumulation error.
        if (ledger.pc != reference.pc || ledger.cc != reference.cc ||
            ledger.core != expand(current_core))
            throw runtime_error("annealing incremental ownership invariant failed");
        auto near = [](double a, double b) { return abs(a - b) <= 1e-9 * max({1.0, abs(a), abs(b)}); };
        if (!near(ledger.bytes, reference.bytes))
            throw runtime_error("annealing incremental traffic invariant failed");
        for (int c = 0; c < cores; ++c) for (int p = 0; p < 5; ++p)
            if (!near(ledger.load[c][p], reference.load[c][p]))
                throw runtime_error("annealing incremental load invariant failed");
        ledger.bytes = reference.bytes; ledger.load = move(reference.load);
        current = measure(ledger, total, limits); ++stats.rebasings;
        observe();
    };
    for (size_t step = 0; step < limits.proposals && !bundles.empty(); ++step) {
        // Only stop between whole move/swap transactions, never during undo.
        if (deadline && deadline->expired()) { stats.deadline_reached = true; break; }
        ++stats.proposals;
        const double fraction = limits.proposals > 1 ? double(step) / double(limits.proposals - 1) : 0;
        const double temperature = exp(log(first_temperature) +
            fraction * (log(last_temperature) - log(first_temperature)));
        stats.final_temperature = temperature;
        const size_t a = random.bounded(bundles.size());
        const bool swap = random.bounded(100) < limits.swap_percent;
        const int source = current_core[a];
        int target = source;
        size_t b = bundles.size(), count = bundles[a].size();
        if (swap) {
            ++stats.swap_proposals;
            if (bundles.size() < 2) { ++stats.skipped_noop; continue; }
            b = random.bounded(bundles.size() - 1); if (b >= a) ++b;
            target = current_core[b];
            if (target == source) { ++stats.skipped_noop; continue; }
            count += bundles[b].size();
        } else {
            ++stats.move_proposals;
            target = int(random.bounded(size_t(cores - 1))); if (target >= source) ++target;
        }
        if (count > limits.move_nodes) { ++stats.skipped_size; continue; }
        if (count > limits.node_touches - stats.touched_nodes) { ++stats.skipped_budget; continue; }
        stats.touched_nodes += count; ++stats.evaluated;
        const double old_bytes = ledger.bytes;
        const auto old_load = ledger.load;
        for (int v : bundles[a]) move_node(g, ledger, v, target);
        if (swap) for (int v : bundles[b]) move_node(g, ledger, v, source);
        const Metrics trial = measure(ledger, total, limits);
        const double delta = trial.energy - current.energy;
        const bool accepted = delta <= 0 || random.unit() < exp(-delta / temperature);
        if (accepted) {
            ++stats.accepted; if (delta > 0) ++stats.uphill_accepted;
            current_core[a] = target; if (swap) current_core[b] = source;
            current = trial; observe();
        } else {
            ++stats.rejected; stats.undo_nodes += count;
            if (swap) for (auto v = bundles[b].rbegin(); v != bundles[b].rend(); ++v) move_node(g, ledger, *v, target);
            for (auto v = bundles[a].rbegin(); v != bundles[a].rend(); ++v) move_node(g, ledger, *v, source);
            // Reverse moves restore integer incidence counts. Assigning saved
            // doubles (rather than relying on subtraction) makes rollback exact.
            ledger.bytes = old_bytes; ledger.load = old_load;
        }
        // Bounded full rebasing limits drift across ACCEPTED moves as well.
        if (stats.evaluated % 64 == 0) rebase();
    }
    rebase();
    State final_state{current_core, current, "final_state"};
    vector<State> retained{initial_state, best, low_route, low_pipe, final_state};
    // Compare freshly reconstructed totals before exporting. The greedy state
    // remains an explicit fallback even if floating differences affected a tie.
    for (auto &state : retained) {
        TrafficState reference(g, cfg, cores, expand(state.core));
        normalize_zero_cycles(g, reference); state.metrics = measure(reference, total, limits);
    }
    best = retained.front();
    for (const auto &state : retained) if (better(state, best)) best = state;
    best.origin = "best_energy";
    result.best_assignment = expand(best.core);
    stats.best_energy = best.metrics.energy; stats.final_energy = current.energy;
    stats.best_route_bytes = best.metrics.bytes; stats.best_pipe_bound = best.metrics.pipe;
    set<vector<int>> seen;
    auto emit = [&](const State &state) {
        if (result.candidates.size() >= limits.retained || !seen.insert(canonical(state.core, cores)).second) return;
        result.candidates.push_back({expand(state.core), state.metrics.energy, state.metrics.bytes,
                                     state.metrics.pipe, state.origin});
    };
    emit(best); emit(retained[0]); emit(retained[2]); emit(retained[3]); emit(retained[4]);
    if (statistics) *statistics = stats;
    return result;
}
} // namespace dag_chain
