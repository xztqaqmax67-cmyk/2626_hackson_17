#include "official_timing.hpp"
#include <deque>

namespace dag_chain {
namespace {
constexpr const char *pipes[5] = {"PIPE_MTE2", "PIPE_MTE3", "PIPE_M", "PIPE_V", "PIPE_FIX"};
i64 add_checked(i64 a, i64 b) {
    if (a < 0 || b < 0 || a > numeric_limits<i64>::max() - b)
        throw runtime_error("official timing exceeds signed 64-bit range");
    return a + b;
}
i64 ceil_time(double x) {
    x = ceil(x);
    if (!isfinite(x) || x < 0 || x >= 9223372036854775808.0)
        throw runtime_error("official timing exceeds signed 64-bit range");
    return i64(x);
}
bool integer_le_float(i64 integer, double value) {
    // Python compares a large int to float without first rounding the int.
    // C++'s usual integer-to-double conversion would retire some ops too early.
    if (value >= 9223372036854775808.0) return true;
    if (value < 0) return false;
    return integer <= i64(floor(value));
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
struct View {
    const OfficialExecutionGraph *graph = nullptr;
    unordered_map<i64, int> op_index, tensor_index;
    vector<vector<int>> inputs, outputs, input_set, output_set, sorted_outputs, pred, succ;
    vector<vector<int>> producers, consumers;
    vector<int> seq, rank;
    array<vector<int>, 5> order;
    vector<i64> duration;
    vector<unsigned char> ddr;
    explicit View(const OfficialExecutionGraph &g) : graph(&g) {
        const int n = int(g.ops.size()), nt = int(g.tensors.size());
        inputs.resize(n); outputs.resize(n); pred.resize(n); succ.resize(n);
        producers.resize(nt); consumers.resize(nt); rank.resize(n, -1);
        input_set.resize(n); output_set.resize(n); sorted_outputs.resize(n);
        duration.resize(n); ddr.resize(n);
        for (int v = 0; v < n; ++v) {
            if (!op_index.emplace(g.ops[v].id, v).second) throw runtime_error("duplicate execution op ID");
            if (g.ops[v].pipe < 0 || g.ops[v].pipe >= 5) throw runtime_error("invalid execution pipe");
        }
        for (int t = 0; t < nt; ++t) {
            if (!tensor_index.emplace(g.tensors[t].id, t).second || op_index.count(g.tensors[t].id))
                throw runtime_error("duplicate execution tensor ID");
            if (g.tensors[t].size < 0 || g.tensors[t].memory < -1 || g.tensors[t].memory > 1)
                throw runtime_error("invalid execution tensor");
        }
        auto link = [&](int a, int b) { if (a != b) { pred[b].push_back(a); succ[a].push_back(b); } };
        for (auto [a, b] : g.edges) {
            auto oa = op_index.find(a), ob = op_index.find(b);
            if (oa != op_index.end() && ob != op_index.end()) link(oa->second, ob->second);
            else if (oa != op_index.end()) outputs[oa->second].push_back(tensor_index.at(b));
            else if (ob != op_index.end()) inputs[ob->second].push_back(tensor_index.at(a));
            else throw runtime_error("execution edge has no op endpoint");
        }
        for (int v = 0; v < n; ++v) {
            for (int t : inputs[v]) consumers[t].push_back(v);
            for (int t : outputs[v]) producers[t].push_back(v);
        }
        for (int t = 0; t < nt; ++t) {
            Graph::unique_vec(producers[t]); Graph::unique_vec(consumers[t]);
            for (int a : producers[t]) for (int b : consumers[t]) link(a, b);
        }
        for (i64 id : g.sequence) {
            const int v = op_index.at(id);
            if (rank[v] >= 0) throw runtime_error("duplicate op in execution sequence");
            rank[v] = int(seq.size()); seq.push_back(v); order[g.ops[v].pipe].push_back(v);
        }
        if (int(seq.size()) != n) throw runtime_error("incomplete execution sequence");
        for (int v = 0; v < n; ++v) {
            Graph::unique_vec(pred[v]); Graph::unique_vec(succ[v]);
            for (int p : pred[v]) if (rank[p] >= rank[v]) throw runtime_error("Step2 sequence is not topological");
            auto as_set = [&](const vector<int> &tids) {
                vector<i64> ids; for (int t : tids) ids.push_back(g.tensors[t].id);
                vector<int> result; for (i64 id : official_python_int_set_order(ids)) result.push_back(tensor_index.at(id));
                return result;
            };
            input_set[v] = as_set(inputs[v]); output_set[v] = as_set(outputs[v]);
            sorted_outputs[v] = output_set[v];
            sort(sorted_outputs[v].begin(), sorted_outputs[v].end(), [&](int a, int b) { return g.tensors[a].id < g.tensors[b].id; });
            const auto &op = g.ops[v]; duration[v] = max<i64>(1, op.cycles);
            if (op.kind == "COPY_IN" || op.kind == "COPY_OUT") {
                const auto &sizes = op.kind == "COPY_IN" ? outputs[v] : inputs[v];
                if (!sizes.empty()) {
                    i64 bytes = 0; for (int t : sizes) bytes = add_checked(bytes, g.tensors[t].size);
                    duration[v] = max<i64>(1, ceil_time(double(bytes) / 60.0));
                }
                for (int t : inputs[v]) if (g.tensors[t].memory < 0) ddr[v] = 1;
                for (int t : outputs[v]) if (g.tensors[t].memory < 0) ddr[v] = 1;
            }
        }
    }
};

// DDR work is measured in exclusive-bandwidth cycles, just as the Python
// evaluator. Preserve its double operations and ceil(cursor - 1e-9) rounding.
struct DdrPool {
    map<int, double> work;
    i64 last_update = 0;
    const vector<pair<int, i64>> &key;
    vector<i64> &end;
    bool local;
    DdrPool(const vector<pair<int, i64>> &k, vector<i64> &e, bool l) : key(k), end(e), local(l) {}
    void advance(i64 now) {
        // Python stores `now` (an int) after every update, so subtraction must
        // happen before conversion even when timestamps exceed 2^53 cycles.
        double elapsed = double(now - last_update);
        while (elapsed > 1e-9) {
            double smallest = numeric_limits<double>::infinity(); int active = 0;
            for (const auto &[v, w] : work) { (void)v; if (w > 1e-9) { ++active; smallest = min(smallest, w); } }
            if (!active) break;
            const double first = smallest * active;
            if (first >= elapsed - 1e-9) {
                const double progress = elapsed / active;
                for (auto &[v, w] : work) { (void)v; if (w > 1e-9) w = max(0.0, w - progress); }
                break;
            }
            for (auto &[v, w] : work) { (void)v; if (w > 1e-9) w = max(0.0, w - smallest); }
            elapsed -= first;
        }
        last_update = now;
    }
    void reschedule(i64 now) {
        vector<pair<double, int>> ordered;
        for (auto [v, w] : work) ordered.emplace_back(max(0.0, w), v);
        sort(ordered.begin(), ordered.end(), [&](const auto &a, const auto &b) {
            if (a.first != b.first) return a.first < b.first;
            return key[a.second] < key[b.second];
        });
        double cursor = double(now), previous = 0; size_t active = ordered.size();
        for (size_t i = 0; i < ordered.size();) {
            const double w = ordered[i].first, delta = w - previous;
            double increment = delta * double(active);
            if (local && active > 1) {
                const double base = delta * double(active - 1);
                increment = base * double(active) / double(active - 1);
            }
            cursor += increment;
            const i64 projected = ceil_time(cursor - 1e-9);
            size_t j = i;
            while (j < ordered.size() && abs(ordered[j].first - w) <= 1e-9) end[ordered[j++].second] = projected;
            active -= j - i; previous = w; i = j;
        }
    }
};

void local_timing(View &view, OfficialCoreTiming &result) {
    const auto &g = *view.graph;
    const int n = int(g.ops.size()), nt = int(g.tensors.size());
    vector<int> degree(n), status(n), remaining(nt), allocation_order, allocation_rank(n, -1);
    vector<i64> start(n), end(n);
    vector<pair<int, i64>> keys;
    for (int v = 0; v < n; ++v) { degree[v] = int(view.pred[v].size()); keys.emplace_back(0, g.ops[v].id); }
    for (int t = 0; t < nt; ++t) remaining[t] = int(view.consumers[t].size());
    for (int v : view.seq) for (int t : view.outputs[v]) if (g.tensors[t].memory >= 0) {
        allocation_rank[v] = int(allocation_order.size()); allocation_order.push_back(v); break;
    }
    struct Credit { i64 bytes = 0; int tensor = -1; vector<int> sources; bool war = false; };
    struct Part { i64 bytes = 0; set<i64> tids; set<string> kinds, positions; };
    map<pair<i64, i64>, Part> parts;
    array<deque<Credit>, 2> credits;
    array<i64, 2> used{};
    vector<unsigned char> resident(nt);
    const Config cfg;
    auto allocate = [&](int t, int producer) {
        const auto &tensor = g.tensors[t]; int m = tensor.memory;
        if (m < 0 || resident[t]) return;
        if (producer >= 0) {
            i64 missing = tensor.size;
            while (missing > 0 && !credits[m].empty()) {
                auto &credit = credits[m].front(); i64 taken = min(missing, credit.bytes);
                for (int source : credit.sources) if (source != producer) {
                    auto &part = parts[{g.ops[source].id, g.ops[producer].id}];
                    part.bytes = add_checked(part.bytes, taken); part.tids.insert(g.tensors[credit.tensor].id);
                    part.kinds.insert(credit.war ? "WAR" : "WAW"); part.positions.insert(m == 0 ? "L1" : "UB");
                }
                credit.bytes -= taken; missing -= taken;
                if (credit.bytes == 0) credits[m].pop_front();
            }
            if (missing) throw runtime_error("Step3 virtual capacity credits exhausted");
        }
        used[m] = add_checked(used[m], tensor.size); resident[t] = 1;
        result.peak[m] = max(result.peak[m], used[m]);
    };
    auto release = [&](int t) {
        const auto &tensor = g.tensors[t]; int m = tensor.memory;
        if (m < 0 || !resident[t]) return;
        resident[t] = 0; used[m] -= tensor.size;
        Credit credit; credit.bytes = tensor.size; credit.tensor = t; credit.war = !view.consumers[t].empty();
        credit.sources = credit.war ? view.consumers[t] : view.producers[t];
        sort(credit.sources.begin(), credit.sources.end(), [&](int a, int b) { return g.ops[a].id < g.ops[b].id; });
        credits[m].push_back(move(credit));
    };
    vector<int> tids(nt); iota(tids.begin(), tids.end(), 0);
    sort(tids.begin(), tids.end(), [&](int a, int b) { return g.tensors[a].id < g.tensors[b].id; });
    for (int t : tids) if (view.producers[t].empty() && !view.consumers[t].empty()) allocate(t, -1);
    for (int m = 0; m < 2; ++m) {
        if (used[m] > cfg.capacity[m]) throw runtime_error("Step3 initial resident tensors exceed capacity");
        if (used[m] < cfg.capacity[m]) credits[m].push_back({cfg.capacity[m] - used[m], -1, {}, false});
    }
    auto can_allocate = [&](int v) {
        array<i64, 2> required{};
        for (int t : view.output_set[v]) if (g.tensors[t].memory >= 0 && !resident[t]) {
            int m = g.tensors[t].memory; required[m] = add_checked(required[m], g.tensors[t].size);
        }
        for (int m = 0; m < 2; ++m) if (required[m] > cfg.capacity[m] - used[m]) return false;
        return true;
    };
    array<int, 5> running; running.fill(-1);
    array<size_t, 5> cursor{}; size_t next_allocation = 0;
    DdrPool pool(keys, end, true); i64 now = 0; int done = 0;
    for (int iteration = 0;; ++iteration) {
        if (iteration >= 100000) throw runtime_error("Step3 maximum iterations exceeded");
        pool.advance(now); bool retired_ddr = false;
        for (int p = 0; p < 5; ++p) {
            const int v = running[p];
            if (v < 0 || !integer_le_float(end[v], double(now) + 1e-9)) continue;
            status[v] = 2; ++done;
            for (int t : view.input_set[v]) if (g.tensors[t].memory >= 0 && --remaining[t] == 0) release(t);
            for (int t : view.output_set[v]) if (g.tensors[t].memory >= 0 && remaining[t] == 0) release(t);
            running[p] = -1; ++cursor[p];
            retired_ddr = pool.work.erase(v) > 0 || retired_ddr;
            for (int w : view.succ[v]) --degree[w];
        }
        if (retired_ddr) pool.reschedule(now);
        if (done == n) break;
        // Each pipe has a fixed head, so its ready heap contains at most one
        // eligible op. Repeated pipe passes preserve the official allocation gate.
        bool issued;
        do {
            issued = false;
            for (int p = 0; p < 5; ++p) {
                if (running[p] >= 0 || cursor[p] == view.order[p].size()) continue;
                const int v = view.order[p][cursor[p]];
                if (degree[v] != 0) continue;
                if (allocation_rank[v] >= 0 && (next_allocation >= allocation_order.size() ||
                    allocation_order[next_allocation] != v || !can_allocate(v))) continue;
                if (allocation_rank[v] >= 0) ++next_allocation;
                status[v] = 1;
                for (int t : view.sorted_outputs[v]) allocate(t, v);
                start[v] = now; end[v] = add_checked(now, view.duration[v]); running[p] = v; issued = true;
                if (view.ddr[v]) { pool.advance(now); pool.work[v] = double(view.duration[v]); pool.reschedule(now); }
            }
        } while (issued);
        i64 next = numeric_limits<i64>::max(); bool any = false;
        for (int v : running) if (v >= 0) { next = min(next, end[v]); any = true; }
        if (!any) throw runtime_error("Step3 cannot issue remaining operations");
        if (next < now) throw runtime_error("Step3 time moved backwards");
        now = next;
    }
    for (int t = 0; t < nt; ++t) if (g.tensors[t].memory >= 0 && (remaining[t] || resident[t]))
        throw runtime_error("Step3 tensor lifetime contract failed");
    for (int m = 0; m < 2; ++m) if (used[m] != 0) throw runtime_error("Step3 leaked resident bytes");
    for (int p = 0; p < 5; ++p) for (int v : view.order[p]) result.pipe_orders[p].push_back(g.ops[v].id);
    for (int v = 0; v < n; ++v) {
        result.local_cycles = max(result.local_cycles, end[v]);
        result.local_timeline.push_back({g.ops[v].id, start[v], end[v]});
    }
    for (const auto &[edge, part] : parts) {
        const int a = view.op_index.at(edge.first), b = view.op_index.at(edge.second);
        if (end[a] > start[b]) throw runtime_error("Step3 memory dependency violates local timeline");
        OfficialMemoryDependency d; d.source = edge.first; d.target = edge.second; d.reused_bytes = part.bytes;
        d.previous_tensor_ids.assign(part.tids.begin(), part.tids.end());
        for (const auto &kind : part.kinds) { if (!d.kind.empty()) d.kind += '+'; d.kind += kind; }
        d.positions.assign(part.positions.begin(), part.positions.end()); result.memory_dependencies.push_back(move(d));
        view.pred[b].push_back(a); view.succ[a].push_back(b);
    }
    queue<int> ready;
    for (int v = 0; v < n; ++v) {
        Graph::unique_vec(view.pred[v]); Graph::unique_vec(view.succ[v]);
        degree[v] = int(view.pred[v].size());
        if (!degree[v]) ready.push(v);
        for (int source : view.pred[v]) if (end[source] > start[v])
            throw runtime_error("Step3 dependency timing violation");
    }
    int visited = 0;
    while (!ready.empty()) {
        const int v = ready.front(); ready.pop(); ++visited;
        for (int w : view.succ[v]) if (!--degree[w]) ready.push(w);
    }
    if (visited != n) throw runtime_error("Step3 memory reuse dependencies introduced a cycle");
}

void global_timing(const OfficialSceneIR &ir, vector<View> &views, OfficialTimingResult &result) {
    const int cores = int(views.size());
    vector<int> offset(cores + 1);
    for (int c = 0; c < cores; ++c) offset[c + 1] = offset[c] + int(views[c].graph->ops.size());
    const int n = offset.back();
    vector<pair<int, i64>> keys; vector<int> degree(n), status(n); vector<i64> start(n), end(n);
    vector<vector<int>> external(n);
    vector<array<size_t, 5>> cursor(cores);
    vector<array<int, 5>> running(cores); for (auto &r : running) r.fill(-1);
    for (int c = 0; c < cores; ++c) for (int v = 0; v < int(views[c].graph->ops.size()); ++v) {
        keys.emplace_back(c, views[c].graph->ops[v].id); degree[offset[c] + v] = int(views[c].pred[v].size());
    }
    for (const auto &link : ir.cross_links) {
        const int a = offset.at(link.source_core) + views.at(link.source_core).op_index.at(link.source_copy_out_id);
        const int b = offset.at(link.target_core) + views.at(link.target_core).op_index.at(link.target_copy_in_id);
        external[b].push_back(a);
    }
    for (auto &pred : external) Graph::unique_vec(pred);
    DdrPool pool(keys, end, false); i64 now = 0; int remaining = n;
    auto release = [&](int v, i64 &time) {
        time = 0;
        for (int p : external[v]) {
            if (status[p] != 2) return false;
            time = max(time, add_checked(end[p], 1000));
        }
        return true;
    };
    for (int iteration = 0;; ++iteration) {
        if (iteration >= 1000000) throw runtime_error("Scene B maximum iterations exceeded");
        pool.advance(now); bool retired_ddr = false;
        for (int c = 0; c < cores; ++c) for (int p = 0; p < 5; ++p) {
            const int v = running[c][p]; if (v < 0 || end[v] > now) continue;
            const int local = v - offset[c];
            status[v] = 2; --remaining; running[c][p] = -1; ++cursor[c][p];
            retired_ddr = pool.work.erase(v) > 0 || retired_ddr;
            for (int w : views[c].succ[local]) --degree[offset[c] + w];
        }
        if (retired_ddr) pool.reschedule(now);
        for (int c = 0; c < cores; ++c) for (int p = 0; p < 5; ++p) {
            const auto &view = views[c];
            if (running[c][p] >= 0 || cursor[c][p] == view.order[p].size()) continue;
            const int local = view.order[p][cursor[c][p]], v = offset[c] + local;
            i64 ready_at;
            if (degree[v] || !release(v, ready_at) || ready_at > now) continue;
            status[v] = 1; start[v] = now; end[v] = add_checked(now, view.duration[local]); running[c][p] = v;
            if (view.ddr[local]) { pool.advance(now); pool.work[v] = double(view.duration[local]); pool.reschedule(now); }
        }
        if (!remaining) break;
        i64 next = numeric_limits<i64>::max(); bool any = false;
        for (int c = 0; c < cores; ++c) for (int p = 0; p < 5; ++p) {
            if (running[c][p] >= 0) { next = min(next, end[running[c][p]]); any = true; continue; }
            const auto &view = views[c]; if (cursor[c][p] == view.order[p].size()) continue;
            const int v = offset[c] + view.order[p][cursor[c][p]]; i64 ready_at;
            if (degree[v] == 0 && release(v, ready_at) && ready_at > now) { next = min(next, ready_at); any = true; }
        }
        if (!any || next <= now) throw runtime_error("Scene B cannot advance remaining operations");
        now = next;
    }
    for (int c = 0; c < cores; ++c) for (int v = offset[c]; v < offset[c + 1]; ++v) {
        result.cycles = max(result.cycles, end[v]);
        result.cores[c].timeline.push_back({keys[v].second, start[v], end[v]});
    }
}
} // namespace

vector<i64> official_python_int_set_order(const vector<i64> &values) {
    // CPython 3.11.11 Objects/setobject.c: set_add_entry, set_insert_clean,
    // set_table_resize, set_update_internal (list/generic iterator branch).
    // Source: https://github.com/python/cpython/blob/v3.11.11/Objects/setobject.c
    // Integer hashes: Objects/longobject.c long_hash, 64-bit PyHASH_MODULUS.
    // No deletions occur in set(in_tids)/set(out_tids), hence no dummy slots.
    struct Entry { i64 value = 0; uint64_t hash = 0; bool occupied = false; };
    vector<Entry> table(8); size_t used = 0;
    auto hash_integer = [](i64 value) {
        constexpr uint64_t modulus = (uint64_t(1) << 61) - 1;
        const uint64_t magnitude = value < 0 ? uint64_t(0) - uint64_t(value) : uint64_t(value);
        i64 hash = i64(magnitude % modulus); if (value < 0) hash = -hash;
        if (hash == -1) hash = -2;
        return uint64_t(hash);
    };
    auto insert = [](vector<Entry> &slots, i64 value, uint64_t hash) {
        const size_t mask = slots.size() - 1; size_t i = size_t(hash) & mask; uint64_t perturb = hash;
        while (true) {
            const size_t probes = i + 9 <= mask ? 9 : 0;
            for (size_t j = 0; j <= probes; ++j) {
                auto &entry = slots[i + j];
                if (!entry.occupied) { entry = {value, hash, true}; return true; }
                if (entry.hash == hash && entry.value == value) return false;
            }
            perturb >>= 5; i = (i * 5 + 1 + size_t(perturb)) & mask;
        }
    };
    for (i64 value : values) if (insert(table, value, hash_integer(value))) {
        ++used;
        if (used * 5 >= (table.size() - 1) * 3) {
            const size_t minimum = used * (used > 50000 ? 2 : 4); size_t size = 8;
            while (size <= minimum) size <<= 1;
            vector<Entry> resized(size);
            for (const auto &entry : table) if (entry.occupied) insert(resized, entry.value, entry.hash);
            table.swap(resized);
        }
    }
    vector<i64> result; result.reserve(used);
    for (const auto &entry : table) if (entry.occupied) result.push_back(entry.value);
    return result;
}

OfficialTimingResult official_timing(const OfficialSceneIR &ir) {
    OfficialTimingResult result; result.route_bytes = ir.route_bytes; result.spill_bytes = ir.spill_bytes;
    if (!ir.valid) { result.error = ir.error; return result; }
    try {
        vector<View> views; views.reserve(ir.tasks.size()); result.cores.resize(ir.tasks.size());
        for (size_t c = 0; c < ir.tasks.size(); ++c) {
            views.emplace_back(ir.tasks[c]); result.cores[c].core = int(c);
            local_timing(views.back(), result.cores[c]);
        }
        global_timing(ir, views, result); result.valid = true;
    } catch (const exception &error) { result.error = error.what(); }
    return result;
}
OfficialTimingResult official_timing(const Graph &g, const Solution &s, int cores) {
    return official_timing(official_execution_graph(g, s, cores));
}
string official_timing_json(const OfficialTimingResult &result) {
    ostringstream out;
    auto timeline = [&](const vector<OfficialOpTiming> &values) {
        out << '[';
        for (size_t i = 0; i < values.size(); ++i) {
            if (i) out << ',';
            const auto &v = values[i];
            out << "{\"id\":" << v.id << ",\"start\":" << v.start << ",\"end\":" << v.end << '}';
        }
        out << ']';
    };
    out << "{\"valid\":" << (result.valid ? "true" : "false") << ",\"error\":" << quote(result.error)
        << ",\"cycles\":" << result.cycles << ",\"route_bytes\":" << result.route_bytes
        << ",\"spill_bytes\":" << result.spill_bytes << ",\"cores\":[";
    for (size_t c = 0; c < result.cores.size(); ++c) {
        if (c) out << ',';
        const auto &r = result.cores[c];
        out << "{\"core\":" << r.core << ",\"local_cycles\":" << r.local_cycles
            << ",\"peak\":{\"L1\":" << r.peak[0] << ",\"UB\":" << r.peak[1] << "},\"pipe_orders\":{";
        for (int p = 0; p < 5; ++p) {
            if (p) out << ',';
            out << quote(pipes[p]) << ":[";
            for (size_t i = 0; i < r.pipe_orders[p].size(); ++i) { if (i) out << ','; out << r.pipe_orders[p][i]; }
            out << ']';
        }
        out << "},\"memory_dependencies\":[";
        for (size_t i = 0; i < r.memory_dependencies.size(); ++i) {
            if (i) out << ',';
            const auto &d = r.memory_dependencies[i];
            out << "{\"source\":" << d.source << ",\"target\":" << d.target << ",\"reused_bytes\":" << d.reused_bytes
                << ",\"kind\":" << quote(d.kind) << ",\"previous_tensor_ids\":[";
            for (size_t j = 0; j < d.previous_tensor_ids.size(); ++j) { if (j) out << ','; out << d.previous_tensor_ids[j]; }
            out << "],\"positions\":[";
            for (size_t j = 0; j < d.positions.size(); ++j) { if (j) out << ','; out << quote(d.positions[j]); }
            out << "]}";
        }
        out << "],\"local_timeline\":"; timeline(r.local_timeline);
        out << ",\"timeline\":"; timeline(r.timeline); out << '}';
    }
    out << "]}"; return out.str();
}
} // namespace dag_chain
