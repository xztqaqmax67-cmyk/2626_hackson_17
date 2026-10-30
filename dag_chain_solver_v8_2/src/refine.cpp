#include "refine.hpp"
#include <deque>

namespace dag_chain {
double simulate_events(const vector<TimingEvent> &events, Deadline *deadline) {
    if (deadline) deadline->check();
    size_t checked_work = 0;
    // Amortize clock reads over 128 elementary operations. This controls
    // cancellation responsiveness, not the simulated NPU cycles or bandwidth.
    auto checkpoint = [&]() {
        if (deadline && (++checked_work & 127U) == 0) deadline->check();
    };
    const int n = int(events.size());
    vector<vector<pair<int, double>>> successors(n);
    vector<int> degree(n);
    vector<double> release(n);
    for (int v = 0; v < n; ++v) {
        checkpoint();
        for (auto [u, delay] : events[v].predecessors) {
            checkpoint();
            if (u < 0 || u >= n || u == v || delay < 0)
                return INF;
            successors[u].push_back({v, delay});
            ++degree[v];
        }
    }
    using Item = pair<double, int>;
    priority_queue<Item, vector<Item>, greater<Item>> ready, fixed;
    for (int v = 0; v < n; ++v) {
        checkpoint();
        if (!degree[v])
            ready.push({0, v});
    }
    vector<Item> active;
    double now = 0;
    int done = 0;
    auto retire = [&](int u) {
        checkpoint();
        ++done;
        for (auto [v, delay] : successors[u]) {
            checkpoint();
            release[v] = max(release[v], now + delay);
            if (!--degree[v])
                ready.push({release[v], v});
        }
    };
    while (done < n) {
        checkpoint();
        while (!fixed.empty() && fixed.top().first <= now + 1e-7) {
            checkpoint();
            int v = fixed.top().second;
            fixed.pop();
            retire(v);
        }
        // Retire all simultaneous completions before issuing new operations.
        vector<Item> remaining;
        for (auto [work, v] : active) {
            checkpoint();
            if (work <= 1e-7)
                retire(v);
            else
                remaining.push_back({work, v});
        }
        active.swap(remaining);
        while (!ready.empty() && ready.top().first <= now + 1e-7) {
            checkpoint();
            int v = ready.top().second;
            ready.pop();
            if (events[v].ddr)
                active.push_back({max(1.0, events[v].work), v});
            else
                fixed.push({now + max(1.0, events[v].work), v});
        }
        if (done == n)
            break;
        double next = INF;
        if (!fixed.empty())
            next = min(next, fixed.top().first);
        if (!ready.empty())
            next = min(next, ready.top().first);
        for (const auto &item : active) {
            checkpoint();
            next = min(next, now + item.first * double(active.size()));
        }
        if (next >= INF / 2 || next <= now)
            return INF; // Dependency/Pipe cycle, or an invalid event stream.
        double delta = active.empty() ? 0 : (next - now) / double(active.size());
        for (auto &item : active) {
            checkpoint();
            item.first -= delta;
        }
        now = next;
    }
    if (deadline) deadline->check();
    return now;
}

namespace {
struct OperandEvent {
    vector<int> uses, writes;
    int restored = -1;
};
struct Model {
    const Graph &g;
    const Solution &s;
    int k;
    Config cfg;
    RefinedEstimate result;
    vector<TimingEvent> events;
    vector<OperandEvent> operands;
    vector<vector<int>> before, after, base;
    vector<i64> sizes;
    vector<int> memory, position;
    Deadline *deadline;
    size_t checked_work = 0;

    void checkpoint() {
        if (deadline && (++checked_work & 127U) == 0) deadline->check();
    }

    Model(const Graph &graph, const Solution &solution, int cores, Deadline *stop)
        : g(graph), s(solution), k(cores), before(g.size()), after(g.size()), base(k),
          position(g.size()), deadline(stop) {
        if (deadline) deadline->check();
        for (int i = 0; i < g.size(); ++i) {
            checkpoint();
            position[s.order[i]] = i;
        }
        for (const auto &t : g.tensors) {
            checkpoint();
            sizes.push_back(t.size);
            memory.push_back(t.pos);
        }
        for (int v = 0; v < g.size(); ++v) {
            checkpoint();
            TimingEvent event;
            event.core = s.core[v];
            event.pipe = g.ops[v].pipe;
            event.work = g.weight(v);
            for (int u : g.ops[v].pred) {
                checkpoint();
                event.predecessors.push_back({u, 0});
            }
            OperandEvent op;
            op.uses = g.ops[v].inputs;
            op.uses.insert(op.uses.end(), g.ops[v].outputs.begin(), g.ops[v].outputs.end());
            Graph::unique_vec(op.uses);
            op.writes = g.ops[v].outputs;
            events.push_back(move(event));
            operands.push_back(move(op));
        }
    }
    void depend(int a, int b, double delay = 0) {
        checkpoint();
        if (a >= 0 && b >= 0 && a != b)
            events[b].predecessors.push_back({a, delay});
    }
    int copy(int c, int t, bool input, bool spill = false) {
        checkpoint();
        int id = int(events.size());
        TimingEvent event;
        event.core = c;
        event.pipe = input ? 0 : 1;
        event.ddr = true;
        event.work = max(1.0, ceil(double(sizes[t]) / cfg.bandwidth));
        events.push_back(move(event));
        operands.push_back({{t}, {}, input ? t : -1});
        if (spill)
            result.spill += double(sizes[t]);
        else
            result.route += double(sizes[t]);
        return id;
    }
    void build_boundary() {
        // Singleton subgraphs make their input COPYs precede the computation,
        // and output COPYs follow the last writer. Preserve deterministic order.
        for (int t = 0; t < int(g.tensors.size()); ++t) {
            checkpoint();
            const auto &x = g.tensors[t];
            vector<int> first(k, -1);
            for (int v : x.consumers) {
                checkpoint();
                if (first[s.core[v]] < 0 || position[v] < position[first[s.core[v]]])
                    first[s.core[v]] = v;
            }
            int last = -1;
            for (int v : x.producers) {
                checkpoint();
                if (last < 0 || position[v] > position[last])
                    last = v;
            }
            if (last < 0) {
                for (int c = 0; c < k; ++c)
                    if (first[c] >= 0) {
                        int in = copy(c, t, true);
                        before[first[c]].push_back(in);
                        for (int v : x.consumers) {
                            checkpoint();
                            if (s.core[v] == c)
                                depend(in, v);
                        }
                    }
                continue;
            }
            int src = s.core[last];
            auto write = [&]() {
                int out = copy(src, t, false);
                after[last].push_back(out);
                for (int v : x.producers)
                    depend(v, out);
                return out;
            };
            if (x.final_output)
                write();
            for (int c = 0; c < k; ++c)
                if (c != src && first[c] >= 0) {
                    int out = write(), in = copy(c, t, true);
                    before[first[c]].push_back(in);
                    depend(out, in, cfg.delay);
                    for (int v : x.consumers) {
                        checkpoint();
                        if (s.core[v] == c)
                            depend(in, v);
                    }
                }
        }
        for (const auto &d : g.direct) {
            checkpoint();
            if (s.core[d.a] != s.core[d.b]) {
                int t = int(sizes.size());
                sizes.push_back(d.size);
                memory.push_back(1);
                operands[d.a].uses.push_back(t);
                operands[d.a].writes.push_back(t);
                operands[d.b].uses.push_back(t);
                int out = copy(s.core[d.a], t, false), in = copy(s.core[d.b], t, true);
                after[d.a].push_back(out);
                before[d.b].push_back(in);
                depend(d.a, out);
                depend(out, in, cfg.delay);
                depend(in, d.b);
            }
        }
        for (int v : s.order) {
            checkpoint();
            auto &seq = base[s.core[v]];
            seq.insert(seq.end(), before[v].begin(), before[v].end());
            seq.push_back(v);
            seq.insert(seq.end(), after[v].begin(), after[v].end());
        }
    }
    bool build_memory(int c) {
        if (deadline) deadline->check();
        const auto &seq = base[c];
        const int nt = int(sizes.size()), never = int(seq.size()) + 1;
        vector<vector<pair<int, int>>> usage(seq.size());
        vector<int> next(nt, never), last(nt, -1), protected_at(nt, -1), backing(nt, -1);
        for (int i = int(seq.size()) - 1; i >= 0; --i) {
            checkpoint();
            for (int t : operands[seq[i]].uses) {
                checkpoint();
                usage[i].push_back({t, next[t]});
                next[t] = i;
            }
        }
        vector<unsigned char> live(nt), seen(nt), clean(nt);
        vector<int> restore(nt, -1), allocation(nt);
        int stamp = 0;
        array<i64, 2> used{};
        using Item = tuple<int, int, int>;
        array<priority_queue<Item>, 2> victims;
        // Memory credits record which completed operations release capacity.
        // They approximate reuse stalls without modelling physical addresses.
        struct Credit { i64 bytes; vector<int> owners; };
        array<deque<Credit>, 2> credits;
        vector<vector<int>> owners(nt);
        for (int mem = 0; mem < 2; ++mem)
            credits[mem].push_back({cfg.capacity[mem], {}});
        vector<vector<int>> pre(seq.size()), post(seq.size());
        auto release = [&](int t, int spill_out) {
            checkpoint();
            if (spill_out >= 0)
                owners[t].push_back(spill_out);
            credits[memory[t]].push_back({sizes[t], move(owners[t])});
            owners[t].clear();
            used[memory[t]] -= sizes[t];
            live[t] = 0;
        };
        auto acquire = [&](int t, int event) {
            i64 needed = sizes[t];
            auto &q = credits[memory[t]];
            while (needed > 0 && !q.empty()) {
                checkpoint();
                auto &credit = q.front();
                for (int prev : credit.owners)
                    depend(prev, event);
                i64 take = min(needed, credit.bytes);
                needed -= take;
                credit.bytes -= take;
                if (!credit.bytes)
                    q.pop_front();
            }
            return needed == 0;
        };
        for (int i = 0; i < int(seq.size()); ++i) {
            checkpoint();
            int v = seq[i];
            for (auto [t, nxt] : usage[i]) {
                checkpoint();
                (void)nxt;
                protected_at[t] = i;
            }
            vector<int> added;
            for (auto [t, nxt] : usage[i]) {
                checkpoint();
                (void)nxt;
                if (!live[t]) {
                    live[t] = 1;
                    used[memory[t]] += sizes[t];
                    allocation[t] = stamp++;
                    added.push_back(t);
                    if (seen[t]) {
                        int in = copy(c, t, true, true);
                        pre[i].push_back(in);
                        depend(backing[t], in);
                        restore[t] = in;
                    }
                    seen[t] = 1;
                }
                depend(restore[t], v);
            }
            for (int mem = 0; mem < 2; ++mem) {
                vector<Item> held;
                while (used[mem] > cfg.spill_limit(mem)) {
                    checkpoint();
                    while (!victims[mem].empty()) {
                        checkpoint();
                        auto [nxt, neg_stamp, t] = victims[mem].top();
                        if (!live[t] || next[t] != nxt || -allocation[t] != neg_stamp || nxt == never)
                            victims[mem].pop();
                        else if (protected_at[t] == i) {
                            held.push_back(victims[mem].top());
                            victims[mem].pop();
                        } else
                            break;
                    }
                    if (victims[mem].empty())
                        break;
                    int t = get<2>(victims[mem].top());
                    victims[mem].pop();
                    int out = -1;
                    if (!clean[t]) {
                        out = copy(c, t, false, true);
                        post[last[t]].push_back(out);
                        for (int prev : owners[t])
                            depend(prev, out);
                        backing[t] = out;
                        clean[t] = 1;
                    }
                    release(t, out);
                }
                for (const auto &item : held) {
                    checkpoint();
                    victims[mem].push(item);
                }
                if (used[mem] > cfg.capacity[mem])
                    return false;
                result.peak[mem] = max(result.peak[mem], double(used[mem]));
            }
            for (int t : added) {
                checkpoint();
                int allocation_event = restore[t] >= 0 ? restore[t] : v;
                if (!acquire(t, allocation_event))
                    return false;
                if (restore[t] >= 0)
                    owners[t].push_back(restore[t]);
            }
            if (operands[v].restored >= 0) {
                int t = operands[v].restored;
                clean[t] = 1;
                backing[t] = v;
            }
            for (int t : operands[v].writes) {
                checkpoint();
                clean[t] = 0;
            }
            for (auto [t, nxt] : usage[i]) {
                checkpoint();
                owners[t].push_back(v);
                last[t] = i;
                next[t] = nxt;
                if (nxt == never)
                    release(t, -1);
                else
                    victims[memory[t]].push({nxt, -allocation[t], t});
            }
        }
        array<int, 5> tail;
        tail.fill(-1);
        auto append = [&](int v) {
            checkpoint();
            int pipe = events[v].pipe;
            depend(tail[pipe], v);
            tail[pipe] = v;
        };
        for (int i = 0; i < int(seq.size()); ++i) {
            checkpoint();
            for (int v : pre[i]) append(v);
            append(seq[i]);
            for (int v : post[i]) append(v);
        }
        return true;
    }
};
} // namespace

RefinedEstimate refine_estimate(const Graph &g, const Solution &s, int cores, Deadline *deadline) {
    if (deadline) deadline->check();
    Model model(g, s, cores, deadline);
    model.build_boundary();
    for (int c = 0; c < cores; ++c)
        if (!model.build_memory(c))
            return model.result;
    for (auto &e : model.events) {
        model.checkpoint();
        sort(e.predecessors.begin(), e.predecessors.end());
        e.predecessors.erase(unique(e.predecessors.begin(), e.predecessors.end()),
                             e.predecessors.end());
    }
    model.result.cycles = simulate_events(model.events, deadline);
    model.result.valid = model.result.cycles < INF / 2;
    model.result.events = model.events.size();
    if (deadline) deadline->check();
    return model.result;
}
} // namespace dag_chain

