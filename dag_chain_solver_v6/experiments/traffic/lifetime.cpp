#include "lifetime.hpp"

namespace dag_chain {
namespace {
void check(Deadline *deadline) { if (deadline) deadline->check(); }
struct Interval {
    int tensor, core, first, last, first_local, last_local;
    double priority;
};
}

vector<Solution> lifetime_candidates(const Graph &g, int cores, const Solution &base,
                                    Deadline *deadline, size_t max_candidates) {
    check(deadline);
    if (cores < 1 || cores > 5) throw runtime_error("lifetime core count must be 1..5");
    validate(g, base, cores);
    max_candidates = min(size_t(24), max_candidates);
    vector<Solution> result;
    const int n = g.size();
    if (!n || !max_candidates) return result;
    vector<int> position(n), local_position(n), count(cores);
    for (int i = 0; i < n; ++i) {
        if ((i & 127) == 0) check(deadline);
        const int v = base.order[i];
        position[v] = i;
        local_position[v] = count[base.core[v]]++;
    }
    vector<Interval> intervals;
    for (size_t t = 0; t < g.tensors.size(); ++t) {
        if ((t & 127U) == 0) check(deadline);
        if (!g.tensors[t].size) continue;
        array<int,5> first, last;
        first.fill(n); last.fill(-1);
        size_t visited = 0;
        for (int v : g.tensor_users[t]) {
            if ((visited++ & 127U) == 0) check(deadline);
            const int c = base.core[v];
            first[c] = min(first[c], position[v]);
            last[c] = max(last[c], position[v]);
        }
        for (int c = 0; c < cores; ++c) {
            if (last[c] < 0) continue;
            const int a = local_position[base.order[first[c]]];
            const int b = local_position[base.order[last[c]]];
            // Adjacent uses leave no unrelated local operation to remove.
            if (b - a < 2) continue;
            intervals.push_back({int(t),c,first[c],last[c],a,b,
                                 double(g.tensors[t].size) * double(b-a)});
        }
    }
    auto key = [](const Interval &x) {
        return make_tuple(-x.priority, x.tensor, x.core);
    };
    const size_t hot_count = min(size_t(8), intervals.size());
    partial_sort(intervals.begin(), intervals.begin()+hot_count, intervals.end(),
                 [&](const Interval &a, const Interval &b) { return key(a) < key(b); });
    intervals.resize(hot_count);
    check(deadline);

    // Canonical per-core streams ignore global interleavings that would submit
    // exactly the same plan. At most 24 full keys can be retained.
    set<vector<int>> seen;
    auto stream_key = [&](const Solution &s) {
        vector<int> stream;
        stream.reserve(size_t(n)+size_t(cores));
        for (int c = 0; c < cores; ++c) {
            for (int v : s.order) if (s.core[v] == c) stream.push_back(v);
            stream.push_back(-1);
        }
        return stream;
    };
    seen.insert(stream_key(base));
    vector<unsigned char> relevant(n);
    for (const auto &hot : intervals) {
        check(deadline);
        fill(relevant.begin(), relevant.end(), 0);
        for (int v : g.tensor_users[hot.tensor]) relevant[v] = 1;

        auto offer = [&](int v, int desired, const char *kind) {
            if (result.size() >= max_candidates) return;
            check(deadline);
            const int from = position[v];
            int lower = 0, upper = n-1;
            for (int p : g.ops[v].pred) lower = max(lower, position[p]+1);
            for (int s : g.ops[v].succ) upper = min(upper, position[s]-1);
            const int target = max(lower, min(upper, desired));
            if (target == from) return;
            Solution candidate = base;
            candidate.order.erase(candidate.order.begin()+from);
            candidate.order.insert(candidate.order.begin()+target,v);

            // Require this hot lifetime to shrink in its own core's sequence.
            // Other lifetimes may worsen; the full evaluator decides acceptance.
            int first = n, last = -1, local = 0;
            for (int node : candidate.order) {
                if ((local & 127) == 0) check(deadline);
                if (base.core[node] != hot.core) continue;
                if (relevant[node]) { first = min(first,local); last = local; }
                ++local;
            }
            if (last-first >= hot.last_local-hot.first_local) return;
            auto signature = stream_key(candidate);
            if (!seen.insert(move(signature)).second) return;
            candidate.label = "lifetime_" + to_string(hot.tensor) + "_core" +
                              to_string(hot.core) + "_" + kind + "_" + to_string(v);
            validate(g,candidate,cores);
            check(deadline);
            result.push_back(move(candidate));
        };

        // The last local user is normally a consumer; moving it earlier can
        // release memory. Moving the first producer/use later avoids holding
        // the allocation across unrelated work. Dependency bounds clamp both.
        offer(base.order[hot.last], hot.first+1, "early_last_use");
        offer(base.order[hot.first], hot.last-1, "late_first_use");

        int heavy = -1;
        double bytes = -1;
        for (int i = hot.first+1; i < hot.last; ++i) {
            if ((i & 127) == 0) check(deadline);
            const int v = base.order[i];
            if (base.core[v] != hot.core || relevant[v]) continue;
            double footprint = 0;
            for (int t : g.ops[v].outputs) footprint += double(g.tensors[t].size);
            if (footprint > bytes) { bytes = footprint; heavy = v; }
        }
        if (heavy >= 0 && bytes > 0) {
            offer(heavy, hot.first, "outside_before");
            offer(heavy, hot.last, "outside_after");
        }
        if (result.size() >= max_candidates) break;
    }
    check(deadline);
    return result;
}
} // namespace dag_chain
