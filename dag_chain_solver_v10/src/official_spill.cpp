#include "official_spill.hpp"
#include <list>

namespace dag_chain {
namespace {
i64 checked_add(i64 a, i64 b) {
    if (a < 0 || b < 0 || a > numeric_limits<i64>::max() - b)
        throw runtime_error("official spill trace exceeds signed 64-bit range");
    return a + b;
}
struct Ids {
    i64 last;
    i64 next() { last = checked_add(last, 1); return last; }
};
struct LocalTensor {
    i64 id = -1, size = 0;
    int memory = -1, original = -1;
    vector<int> writers, readers;
    set<i64> input_backings;
};
struct LocalOp {
    OfficialTraceOp info;
    vector<int> pred, succ;
};
struct Task {
    vector<LocalTensor> tensors;
    vector<LocalOp> ops;
    unordered_map<i64, int> tensor_by_id;
    unordered_map<int, int> compute_by_owner;
    vector<pair<int, int>> direct;
    vector<pair<i64, i64>> edges;
    bool capture_edges = false;
    i64 max_id = 0;
    int tensor(i64 id, i64 size, int memory, int original = -1) {
        auto it = tensor_by_id.find(id);
        if (it != tensor_by_id.end()) return it->second;
        int index = int(tensors.size());
        tensor_by_id.emplace(id, index);
        tensors.push_back({id, size, memory, original, {}, {}, {}});
        max_id = max(max_id, id);
        return index;
    }
    int op(i64 id, int owner, const string &kind) {
        const int index = int(ops.size());
        ops.push_back({{id, owner, kind}, {}, {}});
        max_id = max(max_id, id);
        return index;
    }
};

vector<Task> make_tasks(const Graph &g, const Solution &s, int cores, OfficialSpillTrace &trace,
                        OfficialSceneIR *ir = nullptr) {
    vector<Task> tasks(cores);
    for (auto &task : tasks) task.capture_edges = ir != nullptr;
    vector<int> position(g.size());
    for (int i = 0; i < g.size(); ++i) position[s.order[i]] = i;
    for (int v = 0; v < g.size(); ++v) {
        auto &task = tasks[s.core[v]];
        task.compute_by_owner[v] = task.op(g.ops[v].id, v, g.ops[v].name);
    }
    Ids ids{g.original_max_node_id};
    auto copy = [&](int core, int tid, i64 size, int owner, bool input, i64 shared_ddr = -1) {
        auto &task = tasks[core];
        const i64 ddr = shared_ddr >= 0 ? shared_ddr : ids.next();
        const i64 id = ids.next();
        task.tensor(ddr, size, -1);
        int op = task.op(id, owner, input ? "COPY_IN" : "COPY_OUT");
        if (input) {
            task.tensors[tid].writers.push_back(op);
            task.tensors[tid].input_backings.insert(ddr);
        } else task.tensors[tid].readers.push_back(op);
        if (task.capture_edges) {
            if (input) { task.edges.emplace_back(ddr, id); task.edges.emplace_back(id, task.tensors[tid].id); }
            else { task.edges.emplace_back(task.tensors[tid].id, id); task.edges.emplace_back(id, ddr); }
        }
        trace.route_bytes = checked_add(trace.route_bytes, size);
        return id;
    };
    vector<int> tensor_order(g.tensors.size());
    iota(tensor_order.begin(), tensor_order.end(), 0);
    sort(tensor_order.begin(), tensor_order.end(), [&](int a, int b) {
        return g.tensors[a].id < g.tensors[b].id;
    });
    for (int t : tensor_order) {
        const auto &tensor = g.tensors[t];
        vector<vector<int>> writers(cores), readers(cores);
        for (int v : tensor.producers) writers[s.core[v]].push_back(v);
        for (int v : tensor.consumers) readers[s.core[v]].push_back(v);
        vector<int> local(cores, -1), first(cores, -1), last(cores, -1);
        for (int c = 0; c < cores; ++c) {
            if (writers[c].empty() && readers[c].empty()) continue;
            auto &task = tasks[c];
            local[c] = task.tensor(tensor.id, tensor.size, tensor.pos, t);
            for (int v : writers[c]) {
                task.tensors[local[c]].writers.push_back(task.compute_by_owner.at(v));
                if (last[c] < 0 || position[v] > position[last[c]]) last[c] = v;
            }
            for (int v : readers[c]) {
                task.tensors[local[c]].readers.push_back(task.compute_by_owner.at(v));
                if (first[c] < 0 || position[v] < position[first[c]]) first[c] = v;
            }
            if (task.capture_edges) {
                vector<i64> producer_ids, consumer_ids;
                for (int v : writers[c]) producer_ids.push_back(g.ops[v].id);
                for (int v : readers[c]) consumer_ids.push_back(g.ops[v].id);
                sort(producer_ids.begin(), producer_ids.end()); sort(consumer_ids.begin(), consumer_ids.end());
                for (i64 id : producer_ids) task.edges.emplace_back(id, tensor.id);
                for (i64 id : consumer_ids) task.edges.emplace_back(tensor.id, id);
            }
        }
        if (tensor.producers.empty())
            for (int c = 0; c < cores; ++c)
                if (first[c] >= 0) copy(c, local[c], tensor.size, first[c], true);
        if (tensor.final_output)
            for (int c = 0; c < cores; ++c)
                if (last[c] >= 0) copy(c, local[c], tensor.size, last[c], false);
        for (int source = 0; source < cores; ++source)
            if (last[source] >= 0)
                for (int target = 0; target < cores; ++target)
                    if (target != source && first[target] >= 0) {
                        const i64 ddr = ids.next();
                        const i64 out_id = copy(source, local[source], tensor.size, last[source], false, ddr);
                        const i64 in_id = copy(target, local[target], tensor.size, first[target], true, ddr);
                        if (ir) ir->cross_links.push_back({source, target, out_id, in_id});
                    }
    }
    // Graph retains original direct-edge insertion order, as _original_tensor_views does.
    for (const auto &edge : g.direct) {
        const int source = s.core[edge.a], target = s.core[edge.b];
        if (source == target) {
            auto &task = tasks[source];
            task.direct.push_back({task.compute_by_owner.at(edge.a), task.compute_by_owner.at(edge.b)});
            if (task.capture_edges) task.edges.emplace_back(g.ops[edge.a].id, g.ops[edge.b].id);
            continue;
        }
        const i64 local_id = ids.next(), ddr = ids.next();
        auto &a = tasks[source]; auto &b = tasks[target];
        const int ta = a.tensor(local_id, edge.size, 1);
        const int tb = b.tensor(local_id, edge.size, 1);
        a.tensors[ta].writers.push_back(a.compute_by_owner.at(edge.a));
        b.tensors[tb].readers.push_back(b.compute_by_owner.at(edge.b));
        if (ir) { a.edges.emplace_back(g.ops[edge.a].id, local_id); b.edges.emplace_back(local_id, g.ops[edge.b].id); }
        const i64 out_id = copy(source, ta, edge.size, edge.a, false, ddr);
        const i64 in_id = copy(target, tb, edge.size, edge.b, true, ddr);
        if (ir) ir->cross_links.push_back({source, target, out_id, in_id});
    }
    return tasks;
}

vector<int> task_sequence(Task &task, const vector<int> &position) {
    const int n = int(task.ops.size());
    auto link = [&](int a, int b) {
        task.ops[a].succ.push_back(b); task.ops[b].pred.push_back(a);
    };
    for (auto &t : task.tensors) {
        Graph::unique_vec(t.writers); Graph::unique_vec(t.readers);
        for (int a : t.writers) for (int b : t.readers) link(a, b);
    }
    for (auto [a, b] : task.direct) link(a, b);
    vector<int> degree(n), depth(n);
    queue<int> ready;
    for (int v = 0; v < n; ++v) {
        auto &op = task.ops[v]; Graph::unique_vec(op.pred); Graph::unique_vec(op.succ);
        degree[v] = int(op.pred.size()); if (!degree[v]) ready.push(v);
    }
    int done = 0;
    while (!ready.empty()) {
        int v = ready.front(); ready.pop(); ++done;
        for (int w : task.ops[v].succ) {
            depth[w] = max(depth[w], depth[v] + 1);
            if (!--degree[w]) ready.push(w);
        }
    }
    if (done != n) throw runtime_error("official task graph is cyclic");
    auto less_key = [&](int a, int b, bool start) {
        const auto &x = task.ops[a].info, &y = task.ops[b].info;
        const string kind = start ? "COPY_OUT" : "COPY_IN";
        // Equivalent to (not is_copy, depth, -id), without negating int64 IDs.
        if ((x.kind != kind) != (y.kind != kind)) return x.kind == kind;
        if (depth[a] != depth[b]) return depth[a] < depth[b];
        return x.id > y.id;
    };
    vector<int> stack, raw;
    for (int v = 0; v < n; ++v) if (task.ops[v].succ.empty()) stack.push_back(v);
    sort(stack.begin(), stack.end(), [&](int a, int b) { return less_key(a, b, true); });
    vector<unsigned char> visited(n);
    while (int(raw.size()) < n) {
        if (stack.empty()) {
            for (int v = 0; v < n; ++v) if (!visited[v]) stack.push_back(v);
            sort(stack.begin(), stack.end(), [&](int a, int b) { return less_key(a, b, false); });
        }
        int v = stack.back();
        if (visited[v]) { stack.pop_back(); continue; }
        vector<int> pending;
        for (int p : task.ops[v].pred) if (!visited[p]) pending.push_back(p);
        if (pending.empty()) { visited[v] = 1; raw.push_back(v); stack.pop_back(); }
        else {
            sort(pending.begin(), pending.end(), [&](int a, int b) { return less_key(a, b, false); });
            stack.insert(stack.end(), pending.begin(), pending.end());
        }
    }
    stable_sort(raw.begin(), raw.end(), [&](int a, int b) {
        return position[task.ops[a].info.owner] < position[task.ops[b].info.owner];
    });
    vector<int> step(n);
    for (int i = 0; i < n; ++i) step[raw[i]] = i;
    for (int v = 0; v < n; ++v) for (int w : task.ops[v].succ)
        if (step[v] >= step[w]) throw runtime_error("official singleton priority order violates dependency");
    return raw;
}

void spill_task(Task &task, const vector<int> &seq, int core, OfficialSpillTrace &trace) {
    auto &out = trace.cores[core]; out.core = core;
    for (int op : seq) out.sequence.push_back(task.ops[op].info);
    const int n = int(seq.size()), nt = int(task.tensors.size()), never = n;
    vector<int> op_step(task.ops.size());
    for (int i = 0; i < n; ++i) op_step[seq[i]] = i;
    vector<vector<int>> uses(nt), writers(nt);
    vector<vector<pair<int, int>>> uses_at(n);
    for (int t = 0; t < nt; ++t) {
        if (task.tensors[t].memory < 0) continue;
        for (int op : task.tensors[t].writers) writers[t].push_back(op_step[op]);
        Graph::unique_vec(writers[t]); uses[t] = writers[t];
        for (int op : task.tensors[t].readers) uses[t].push_back(op_step[op]);
        Graph::unique_vec(uses[t]);
        for (int i = 0; i < int(uses[t].size()); ++i) uses_at[uses[t][i]].push_back({t, i});
    }
    struct Active { bool live = false; int next = -1, used = 0; list<int>::iterator iterator; };
    vector<Active> state(nt);
    array<list<int>, 2> active;
    array<i64, 2> resident{};
    vector<vector<pair<int, int>>> reload_at(n);
    vector<int> protected_at(nt, -1);
    Config cfg;
    auto insert = [&](int t, int next, int used) {
        auto &a = state[t]; const auto &tensor = task.tensors[t];
        if (!a.live) {
            active[tensor.memory].push_back(t); a.iterator = prev(active[tensor.memory].end());
            resident[tensor.memory] = checked_add(resident[tensor.memory], tensor.size);
            a.live = true;
        }
        a.next = next; a.used = used;
    };
    auto erase = [&](int t) {
        auto &a = state[t]; if (!a.live) return;
        int memory = task.tensors[t].memory;
        active[memory].erase(a.iterator); resident[memory] -= task.tensors[t].size; a.live = false;
    };
    const size_t first_record = trace.records.size();
    for (int step = 0; step < n; ++step) {
        for (auto [t, index] : uses_at[step]) { (void)index; protected_at[t] = step; }
        for (auto [t, index] : reload_at[step])
            if (!state[t].live) insert(t, index + 1 < int(uses[t].size()) ? uses[t][index + 1] : never, index);
        vector<int> release;
        for (auto [t, index] : uses_at[step]) {
            if (index == 0) insert(t, uses[t].size() > 1 ? uses[t][1] : never, 1);
            else if (index < int(uses[t].size()) - 1 && state[t].live) {
                state[t].next = uses[t][index + 1]; ++state[t].used;
            }
            if (index == int(uses[t].size()) - 1) release.push_back(t);
        }
        for (int memory = 0; memory < 2; ++memory) {
            while (resident[memory] > cfg.spill_limit(memory)) {
                ++out.overflow_checks;
                int victim = -1, farthest = -1;
                // List iteration preserves Python dict insertion order for equal-next-use ties.
                for (int t : active[memory])
                    if (protected_at[t] != step && state[t].next != never && state[t].next > farthest) {
                        victim = t; farthest = state[t].next;
                    }
                if (victim < 0) {
                    if (resident[memory] > cfg.capacity[memory])
                        throw runtime_error("official Step2 has no legal spill victim at core " + to_string(core) + " step " + to_string(step));
                    break;
                }
                const int t = victim, index = state[t].used;
                if (index < 0 || index >= int(uses[t].size())) throw runtime_error("official spill use index is inconsistent");
                OfficialSpillRecord record;
                record.core = core; record.tensor_index = task.tensors[t].original;
                record.tensor_id = task.tensors[t].id; record.size = task.tensors[t].size; record.memory = memory;
                record.trigger_step = step; record.prev_use_step = index ? uses[t][index - 1] : uses[t].front();
                record.next_use_step = state[t].next;
                const auto &trigger = out.sequence[step], &previous = out.sequence[record.prev_use_step], &next = out.sequence[record.next_use_step];
                record.trigger_op = trigger.id; record.trigger_owner = trigger.owner;
                record.prev_use_op = index ? previous.id : -1; record.prev_owner = index ? previous.owner : -1;
                record.next_use_op = next.id; record.next_owner = next.owner;
                trace.records.push_back(move(record));
                reload_at[farthest].push_back({t, index}); erase(t);
            }
            out.peak[memory] = max(out.peak[memory], resident[memory]);
        }
        for (int t : release) erase(t);
    }
    // Match Python's post-pass backing accounting. COPY_IN writers also count;
    // original backing is synchronized at the first use, exactly as in Step2.
    vector<i64> backing(nt, -1), incarnation(nt);
    vector<int> sync(nt, -1), version(nt);
    vector<unsigned char> original_backing(nt);
    for (int t = 0; t < nt; ++t) {
        incarnation[t] = task.tensors[t].id;
        if (!uses[t].empty() && task.tensors[t].input_backings.size() == 1) {
            backing[t] = *task.tensors[t].input_backings.begin(); sync[t] = uses[t].front(); original_backing[t] = 1;
        }
    }
    Ids ids{task.max_id};
    for (size_t i = first_record; i < trace.records.size(); ++i) {
        auto &record = trace.records[i]; int t = task.tensor_by_id.at(record.tensor_id);
        const auto first_dirty = upper_bound(writers[t].begin(), writers[t].end(), sync[t]);
        const bool dirty = first_dirty != writers[t].end() && *first_dirty <= record.prev_use_step;
        record.spill_out_copies_data = backing[t] < 0 || dirty;
        record.from_tid = incarnation[t];
        if (record.spill_out_copies_data) { sync[t] = record.prev_use_step; record.spill_out_id = ids.next(); }
        record.spill_in_id = ids.next();
        if (record.spill_out_copies_data) {
            backing[t] = ids.next(); original_backing[t] = 0; record.backing_source = "new_spill_out";
        } else record.backing_source = original_backing[t] ? "original_copy_in" : "reused_spill_out";
        record.backing_tid = backing[t]; record.version = ++version[t];
        record.to_tid = ids.next(); incarnation[t] = record.to_tid;
        trace.spill_bytes = checked_add(trace.spill_bytes, record.transfer_bytes());
    }
}
string quote(const string &s) {
    ostringstream out; out << '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') out << '\\' << char(c);
        else if (c < 32) out << "\\u" << hex << setw(4) << setfill('0') << int(c) << dec;
        else out << char(c);
    }
    out << '"'; return out.str();
}
void optional_id(ostream &out, i64 id) { if (id < 0) out << "null"; else out << id; }
} // namespace

i64 OfficialSpillRecord::transfer_bytes() const { return spill_out_copies_data ? checked_add(size, size) : size; }
OfficialSpillTrace official_spill_trace(const Graph &g, const Solution &s, int cores) {
    OfficialSpillTrace trace;
    try {
        if (cores < 2 || cores > 5) throw runtime_error("official spill trace requires 2..5 cores");
        validate(g, s, cores);
        auto tasks = make_tasks(g, s, cores, trace);
        vector<int> position(g.size());
        for (int i = 0; i < g.size(); ++i) position[s.order[i]] = i;
        trace.cores.resize(cores);
        for (int c = 0; c < cores; ++c) {
            auto seq = task_sequence(tasks[c], position);
            spill_task(tasks[c], seq, c, trace);
        }
        trace.valid = true;
    } catch (const exception &error) { trace.error = error.what(); }
    return trace;
}
OfficialSceneIR official_execution_graph(const Graph &g, const Solution &s, int cores) {
    OfficialSceneIR ir;
    try {
        if (cores < 2 || cores > 5) throw runtime_error("official timing requires 2..5 cores");
        validate(g, s, cores);
        OfficialSpillTrace trace;
        auto tasks = make_tasks(g, s, cores, trace, &ir);
        vector<int> position(g.size());
        for (int i = 0; i < g.size(); ++i) position[s.order[i]] = i;
        trace.cores.resize(cores); ir.tasks.resize(cores);
        for (int c = 0; c < cores; ++c) {
            auto &task = tasks[c]; auto &out = ir.tasks[c];
            const auto seq = task_sequence(task, position);
            const size_t begin = trace.records.size();
            spill_task(task, seq, c, trace);
            unordered_map<i64, int> step;
            for (int i = 0; i < int(seq.size()); ++i) step.emplace(task.ops[seq[i]].info.id, i);
            for (const auto &t : task.tensors) out.tensors.push_back({t.id, t.size, t.memory});
            for (const auto &op : task.ops) {
                const auto &info = op.info;
                const bool compute = task.compute_by_owner.at(info.owner) == int(&op - task.ops.data());
                out.ops.push_back({info.id, compute ? g.ops[info.owner].cycles : 1,
                                  info.owner, compute ? g.ops[info.owner].pipe : (info.kind == "COPY_IN" ? 0 : 1), info.kind});
            }
            unordered_map<i64, vector<const OfficialSpillRecord *>> records;
            vector<vector<i64>> before(seq.size()), after(seq.size());
            for (size_t j = begin; j < trace.records.size(); ++j) {
                const auto &r = trace.records[j]; records[r.tensor_id].push_back(&r);
                if (r.spill_out_copies_data) {
                    out.tensors.push_back({r.backing_tid, r.size, -1});
                    out.ops.push_back({r.spill_out_id, max<i64>(1, r.size / 64), r.prev_owner, 1, "COPY_OUT"});
                    after.at(r.prev_use_step).push_back(r.spill_out_id);
                }
                out.tensors.push_back({r.to_tid, r.size, r.memory});
                out.ops.push_back({r.spill_in_id, max<i64>(1, r.size / 64), r.next_owner, 0, "COPY_IN"});
                before.at(r.next_use_step).push_back(r.spill_in_id);
            }
            auto incarnation = [&](i64 tid, i64 op) {
                const auto found = records.find(tid);
                if (found == records.end()) return tid;
                const auto &versions = found->second;
                // For one tensor, next_use_step is strictly increasing: after
                // eviction it cannot become a victim again until reloaded at
                // that step, where current operands are protected. Its next
                // eviction and next use must therefore be later. Select the
                // last reload at or before this use (including a later writer),
                // exactly as the former prefix scan, in O(log spill_count).
                const auto next = upper_bound(versions.begin(), versions.end(), step.at(op),
                    [](int use_step, const OfficialSpillRecord *r) { return use_step < r->next_use_step; });
                return next == versions.begin() ? tid : (*prev(next))->to_tid;
            };
            for (auto [a, b] : task.edges) {
                if (records.count(a) && step.count(b)) a = incarnation(a, b);
                else if (step.count(a) && records.count(b)) b = incarnation(b, a);
                out.edges.emplace_back(a, b);
            }
            for (size_t j = begin; j < trace.records.size(); ++j) {
                const auto &r = trace.records[j];
                if (r.spill_out_copies_data) {
                    out.edges.emplace_back(r.from_tid, r.spill_out_id);
                    out.edges.emplace_back(r.spill_out_id, r.backing_tid);
                }
                out.edges.emplace_back(r.backing_tid, r.spill_in_id);
                out.edges.emplace_back(r.spill_in_id, r.to_tid);
            }
            for (size_t i = 0; i < seq.size(); ++i) {
                out.sequence.insert(out.sequence.end(), before[i].begin(), before[i].end());
                out.sequence.push_back(task.ops[seq[i]].info.id);
                out.sequence.insert(out.sequence.end(), after[i].begin(), after[i].end());
            }
        }
        ir.route_bytes = trace.route_bytes; ir.spill_bytes = trace.spill_bytes; ir.valid = true;
    } catch (const exception &error) { ir.error = error.what(); }
    return ir;
}
string official_spill_json(const OfficialSpillTrace &trace) {
    ostringstream out;
    out << "{\"valid\":" << (trace.valid ? "true" : "false") << ",\"error\":" << quote(trace.error)
        << ",\"route_bytes\":" << trace.route_bytes << ",\"spill_bytes\":" << trace.spill_bytes << ",\"cores\":[";
    for (size_t c = 0; c < trace.cores.size(); ++c) {
        if (c) out << ',';
        const auto &core = trace.cores[c];
        out << "{\"core\":" << core.core << ",\"peak\":[" << core.peak[0] << ',' << core.peak[1]
            << "],\"overflow_checks\":" << core.overflow_checks << ",\"sequence\":[";
        for (size_t i = 0; i < core.sequence.size(); ++i) {
            if (i) out << ',';
            const auto &op = core.sequence[i];
            out << "{\"id\":" << op.id << ",\"owner\":" << op.owner << ",\"kind\":" << quote(op.kind) << '}';
        }
        out << "]}";
    }
    out << "],\"records\":[";
    for (size_t i = 0; i < trace.records.size(); ++i) {
        if (i) out << ',';
        const auto &r = trace.records[i];
        out << "{\"core\":" << r.core << ",\"tensor_index\":" << r.tensor_index
            << ",\"tensor_id\":" << r.tensor_id << ",\"size\":" << r.size << ",\"memory\":" << r.memory
            << ",\"trigger_step\":" << r.trigger_step << ",\"prev_use_step\":" << r.prev_use_step
            << ",\"next_use_step\":" << r.next_use_step << ",\"trigger_owner\":" << r.trigger_owner
            << ",\"prev_owner\":" << r.prev_owner << ",\"next_owner\":" << r.next_owner
            << ",\"trigger_op\":" << r.trigger_op << ",\"prev_use_op\":";
        optional_id(out, r.prev_use_op);
        out << ",\"next_use_op\":" << r.next_use_op << ",\"spill_out_copies_data\":" << (r.spill_out_copies_data ? "true" : "false")
            << ",\"spill_out_id\":"; optional_id(out, r.spill_out_id);
        out << ",\"spill_in_id\":" << r.spill_in_id << ",\"backing_tid\":" << r.backing_tid
            << ",\"from_tid\":" << r.from_tid << ",\"to_tid\":" << r.to_tid << ",\"version\":" << r.version
            << ",\"backing_source\":" << quote(r.backing_source) << '}';
    }
    out << "]}"; return out.str();
}
} // namespace dag_chain
