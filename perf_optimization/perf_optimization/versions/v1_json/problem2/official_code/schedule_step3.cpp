// 对应 official_code/schedule_step3.py。
// 步骤 3：结合数据依赖、内存复用依赖和流水线 FIFO 计算时间线。
#include "schedule_step3.hpp"
#include "evaluation_validation.hpp"
#include "../python_order.hpp"
namespace problem2 {
Id op_duration(const Json &op, const GraphViews &views, const Records &tensors, double bandwidth) {
    if (is_copy(op)) {
        const auto &tids =
            (op.at("op") == "COPY_IN" ? views.outputs : views.inputs).at(field_id(op));
        Id size = 0;
        bool found = false;
        for (Id tid : tids) {
            if (tensors.count(tid)) {
                size += field_id(tensors.at(tid), "size");
                found = true;
            }
        }
        if (found) {
            return copy_duration(size, bandwidth);
        }
    }
    return std::max<Id>(1, op.value("cycles", Id(1)));
}
bool uses_ddr_bandwidth(const Json &op, const GraphViews &views, const Records &tensors) {
    if (!is_copy(op)) {
        return false;
    }
    Id id = field_id(op);
    for (const auto *table : {&views.inputs, &views.outputs}) {
        for (Id tid : table->at(id)) {
            if (tensors.count(tid) && tensors.at(tid).at("pos") == "DDR") {
                return true;
            }
        }
    }
    return false;
}
Json step3_simulation(const Json &graph, const Capacity &capacity, double bandwidth, Id max_iter) {
    auto ops = index_records(graph.at("ops")), tensors = index_records(graph.at("tensors"));
    auto views = build_graph_views(graph.at("ops"), graph.at("edges"));
    auto seq = graph.at("seq_ext").get<Ids>();
    IdSet managed, resident;
    std::map<Id, Id> remaining_consumers;
    Capacity used, peak;
    for (auto [pos, size] : capacity) {
        used[pos] = 0;
        peak[pos] = 0;
    }
    for (const auto &[tid, tensor] : tensors) {
        if (capacity.count(field_string(tensor, "pos"))) {
            managed.insert(tid);
            remaining_consumers[tid] = Id(views.consumers[tid].size());
        }
    }
    Json events = array(), contention = array();
    struct Credit {
        Id bytes;
        Ids sources;
        std::optional<Id> tid;
        std::string kind;
    };
    struct Dependency {
        Id bytes = 0;
        IdSet tensors;
        std::set<std::string> kinds, positions;
    };
    std::map<std::string, std::deque<Credit>> credits;
    std::map<std::pair<Id, Id>, Dependency> dependencies;
    auto allocate = [&](Id tid, double now, std::optional<Id> producer, const std::string &kind) {
        if (!managed.count(tid) || resident.count(tid)) {
            return;
        }
        const auto &tensor = tensors.at(tid);
        auto pos = field_string(tensor, "pos");
        Id size = field_id(tensor, "size");
        if (producer) {
            Id remaining = size;
            auto &list = credits[pos];
            while (remaining > 0 && !list.empty()) {
                auto &credit = list.front();
                Id taken = std::min(remaining, credit.bytes);
                for (Id source : credit.sources) {
                    if (source != *producer) {
                        auto &dep = dependencies[{source, *producer}];
                        dep.bytes += taken;
                        if (credit.tid) {
                            dep.tensors.insert(*credit.tid);
                        }
                        dep.kinds.insert(credit.kind);
                        dep.positions.insert(pos);
                    }
                }
                credit.bytes -= taken;
                remaining -= taken;
                if (credit.bytes == 0) {
                    list.pop_front();
                }
            }
            if (remaining) {
                throw Step3SchedulingError("virtual capacity credits exhausted: tid=" + key(tid));
            }
        }
        used[pos] += size;
        resident.insert(tid);
        peak[pos] = std::max(peak[pos], used[pos]);
        events.push_back({{"time", now},
                          {"kind", kind},
                          {"tid", tid},
                          {"logical_tid", tensor.value("logical_tid", tid)},
                          {"pos", pos},
                          {"size", size},
                          {"op_id", producer ? Json(*producer) : Json()},
                          {"used_after", used[pos]}});
    };
    auto release = [&](Id tid, double now, Id consumer) {
        if (!resident.erase(tid)) {
            return;
        }
        const auto &tensor = tensors.at(tid);
        auto pos = field_string(tensor, "pos");
        Id size = field_id(tensor, "size");
        used[pos] -= size;
        bool readers = !views.consumers[tid].empty();
        credits[pos].push_back({size,
                                as_vector(readers ? views.consumers[tid] : views.producers[tid]),
                                tid, readers ? "WAR" : "WAW"});
        events.push_back({{"time", now},
                          {"kind", "free"},
                          {"tid", tid},
                          {"logical_tid", tensor.value("logical_tid", tid)},
                          {"pos", pos},
                          {"size", size},
                          {"op_id", consumer},
                          {"used_after", used[pos]}});
    };
    for (Id tid : managed) {
        if (views.producers[tid].empty() && !views.consumers[tid].empty()) {
            allocate(tid, 0, {}, "initial_alloc");
        }
    }
    for (auto [pos, limit] : capacity) {
        if (used[pos] > limit) {
            throw Step3SchedulingError("initial resident tensors exceed capacity");
        }
        if (limit - used[pos]) {
            credits[pos].push_back({limit - used[pos], {}, {}, "VIRGIN"});
        }
    }
    auto can_allocate = [&](Id op) {
        Capacity need;
        for (Id tid : as_set(views.outputs[op])) {
            if (managed.count(tid) && !resident.count(tid)) {
                need[field_string(tensors.at(tid), "pos")] += field_id(tensors.at(tid), "size");
            }
        }
        for (auto [pos, limit] : capacity) {
            if (used[pos] + need[pos] > limit) {
                return false;
            }
        }
        return true;
    };
    std::map<std::string, Ids> pipe_orders;
    std::map<std::string, std::size_t> pipe_cursor;
    for (auto pipe : PIPES) {
        pipe_orders[pipe];
    }
    Ids allocation_order;
    IdSet allocating;
    std::map<Id, Id> seq_pos, pred_remaining;
    std::map<Id, std::string> status;
    std::map<Id, bool> ddr_transfer;
    std::map<Id, double> start, end, work;
    Ids start_order;
    for (std::size_t i = 0; i < seq.size(); ++i) {
        Id op = seq[i];
        seq_pos[op] = Id(i);
        const auto pipe = field_string(ops.at(op), "pipe");
        if (!pipe_orders.count(pipe)) {
            throw Step3SchedulingError("unsupported pipe: " + pipe);
        }
        pipe_orders.at(pipe).push_back(op);
        status[op] = "pending";
        pred_remaining[op] = Id(views.preds[op].size());
        ddr_transfer[op] = uses_ddr_bandwidth(ops.at(op), views, tensors);
        for (Id tid : views.outputs[op]) {
            if (managed.count(tid)) {
                allocation_order.push_back(op);
                allocating.insert(op);
                break;
            }
        }
    }
    std::map<std::string, std::optional<Id>> running, ready;
    IdSet allocation_ready;
    std::size_t allocation_cursor = 0;
    double last_update = 0, now = 0;
    auto queue_if_ready = [&](Id op) {
        if (status[op] != "pending" || pred_remaining[op]) {
            return;
        }
        auto pipe = field_string(ops.at(op), "pipe");
        const auto &order = pipe_orders[pipe];
        auto cursor = pipe_cursor[pipe];
        if (cursor >= order.size() || order[cursor] != op) {
            return;
        }
        status[op] = "ready";
        if (allocating.count(op)) {
            allocation_ready.insert(op);
        } else {
            ready[pipe] = op;
        }
    };
    auto reschedule = [&](double time, bool log, Id issued, Id duration) {
        auto projected = project_ddr_ends(work, time);
        for (auto [op, finish] : projected) {
            end[op] = finish;
        }
        if (!log) {
            return;
        }
        std::vector<std::pair<double, Id>> ordered;
        for (auto [op, w] : work) {
            ordered.emplace_back(std::max(0., w), op);
        }
        std::sort(ordered.begin(), ordered.end());
        double base = time, adjusted = time, previous = 0;
        std::size_t count = ordered.size(), i = 0;
        Json stages = array();
        std::map<Id, double> provisional;
        while (i < ordered.size()) {
            double w = ordered[i].first, delta = w - previous,
                   base_delta = delta * (count > 1 ? count - 1 : 1),
                   adjusted_delta = count > 1 ? base_delta * count / (count - 1) : delta;
            double base_start = base, adjusted_start = adjusted;
            base += base_delta;
            adjusted += adjusted_delta;
            std::size_t j = i;
            Ids completed, active;
            for (std::size_t k = i; k < ordered.size(); ++k) {
                active.push_back(ordered[k].second);
            }
            std::sort(active.begin(), active.end());
            while (j < ordered.size() && std::abs(ordered[j].first - w) <= 1e-9) {
                provisional[ordered[j].second] = base;
                completed.push_back(ordered[j++].second);
            }
            std::sort(completed.begin(), completed.end());
            stages.push_back({{"active_ops", active},
                              {"n", count},
                              {"base_start", base_start},
                              {"base_end", base},
                              {"adjusted_start", adjusted_start},
                              {"adjusted_before_penalty_end", adjusted_start + base_delta},
                              {"adjusted_end", adjusted},
                              {"factor", count > 1 ? double(count) / (count - 1) : 1.},
                              {"penalty", adjusted_delta - base_delta},
                              {"completed_ops", completed}});
            count -= j - i;
            previous = w;
            i = j;
        }
        Ids active;
        for (auto [op, w] : work) {
            active.push_back(op);
        }
        contention.push_back(
            {{"time", time},
             {"issued_op", issued},
             {"issue_order", contention.size() + 1},
             {"exclusive_end", time + duration},
             {"provisional_end", provisional.at(issued)},
             {"slowdown_factor", work.size() > 1 ? double(work.size()) / (work.size() - 1) : 1.},
             {"active_ops", active},
             {"stages", stages},
             {"projected_ends", id_map_json(projected)}});
    };
    for (Id op : seq) {
        queue_if_ready(op);
    }
    std::size_t done = 0;
    Id iteration = 0;
    while (true) {
        poll_search_deadline();
        if (++iteration > max_iter) {
            throw Step3SchedulingError("scheduler deadlock: max_iter_exceeded");
        }
        advance_ddr_work(work, last_update, now);
        bool retired_ddr = false;
        for (auto pipe : PIPES) {
            if (running[pipe] && end[*running[pipe]] <= now + 1e-9) {
                Id op = *running[pipe];
                status[op] = "done";
                ++done;
                for (Id tid : python_set_order(views.inputs[op])) {
                    if (remaining_consumers.count(tid) && views.consumers[tid].count(op)) {
                        if (--remaining_consumers[tid] < 0) {
                            throw Step3SchedulingError("negative tensor refcount");
                        }
                        if (remaining_consumers[tid] == 0) {
                            release(tid, now, op);
                        }
                    }
                }
                for (Id tid : python_set_order(views.outputs[op])) {
                    if (remaining_consumers.count(tid) && remaining_consumers[tid] == 0) {
                        release(tid, now, op);
                    }
                }
                auto cursor = ++pipe_cursor[pipe];
                if (cursor < pipe_orders[pipe].size()) {
                    queue_if_ready(pipe_orders[pipe][cursor]);
                }
                if (work.erase(op)) {
                    retired_ddr = true;
                }
                for (Id succ : views.succs[op]) {
                    --pred_remaining[succ];
                    queue_if_ready(succ);
                }
                running[pipe].reset();
            }
        }
        if (retired_ddr) {
            reschedule(now, false, 0, 0);
        }
        if (done == ops.size()) {
            break;
        }
        bool issued = true;
        while (issued) {
            issued = false;
            for (auto pipe : PIPES) {
                if (!running[pipe]) {
                    std::optional<Id> chosen;
                    if (allocation_cursor < allocation_order.size()) {
                        Id op = allocation_order[allocation_cursor];
                        if (allocation_ready.count(op) &&
                            field_string(ops.at(op), "pipe") == pipe && can_allocate(op)) {
                            chosen = op;
                            allocation_ready.erase(op);
                        }
                    }
                    if (!chosen && ready[pipe]) {
                        chosen = ready[pipe];
                        ready[pipe].reset();
                    }
                    if (!chosen) {
                        continue;
                    }
                    Id op = *chosen;
                    issued = true;
                    if (allocating.count(op)) {
                        ++allocation_cursor;
                    }
                    status[op] = "running";
                    for (Id tid : as_set(views.outputs[op])) {
                        allocate(tid, now, op, "alloc");
                    }
                    Id duration = op_duration(ops.at(op), views, tensors, bandwidth);
                    start[op] = now;
                    start_order.push_back(op);
                    end[op] = now + duration;
                    running[pipe] = op;
                    if (ddr_transfer[op]) {
                        advance_ddr_work(work, last_update, now);
                        work[op] = double(duration);
                        reschedule(now, true, op, duration);
                    }
                }
            }
        }
        double next = std::numeric_limits<double>::infinity();
        for (auto pipe : PIPES) {
            if (running[pipe]) {
                next = std::min(next, end[*running[pipe]]);
            }
        }
        if (!std::isfinite(next)) {
            throw Step3SchedulingError("scheduler deadlock: no_running_or_issuable_op");
        }
        now = next;
    }
    Json memory_dependencies = array(), execution = graph;
    std::set<std::pair<Id, Id>> existing;
    for (const auto &e : execution.at("edges")) {
        existing.insert({field_id(e, "source"), field_id(e, "target")});
    }
    for (const auto &[pair, dep] : dependencies) {
        std::string kind;
        for (const auto &name : dep.kinds) {
            if (!kind.empty()) {
                kind += '+';
            }
            kind += name;
        }
        memory_dependencies.push_back({{"source", pair.first},
                                       {"target", pair.second},
                                       {"kind", kind},
                                       {"positions", dep.positions},
                                       {"reused_bytes", dep.bytes},
                                       {"previous_tensor_ids", dep.tensors}});
        if (existing.insert(pair).second) {
            execution["edges"].push_back(
                {{"source", pair.first}, {"target", pair.second}, {"dependency", "MEMORY_REUSE"}});
        }
    }
    auto exec_views = build_graph_views(execution.at("ops"), execution.at("edges"));
    std::vector<std::tuple<Id, Id, std::string>> edges;
    for (auto [target, preds] : exec_views.preds) {
        for (Id source : preds) {
            if (end.at(source) > start.at(target)) {
                throw Step3SchedulingError("dependency timing violation");
            }
            edges.emplace_back(source, target, "execution");
        }
    }
    check_acyclic(seq, edges, "memory reuse dependencies");
    for (auto [tid, count] : remaining_consumers) {
        if (count) {
            throw Step3SchedulingError("tensor lifetime did not close");
        }
    }
    if (!resident.empty()) {
        throw Step3SchedulingError("resident tensors did not close");
    }
    Json timeline = object();
    double makespan = 0;
    for (auto pipe : PIPES) {
        timeline[pipe] = array();
    }
    for (Id op : start_order) {
        timeline[field_string(ops.at(op), "pipe")].push_back({op, start[op], end[op]});
        makespan = std::max(makespan, end[op]);
    }
    for (auto &rows : timeline) {
        std::stable_sort(rows.begin(), rows.end(), [&](const Json &a, const Json &b) {
            return std::make_pair(a[1].get<double>(), seq_pos[number(a[0])]) <
                   std::make_pair(b[1].get<double>(), seq_pos[number(b[0])]);
        });
    }
    return {{"makespan", makespan},
            {"op_start", id_map_json(start)},
            {"op_end", id_map_json(end)},
            {"per_pipe_timeline", timeline},
            {"pipe_orders", pipe_orders},
            {"execution_graph", execution},
            {"memory_dependencies", memory_dependencies},
            {"op_status", id_map_json(status)},
            {"completed_count", start.size()},
            {"pred_remaining", id_map_json(pred_remaining)},
            {"in_tids", id_map_json(views.inputs)},
            {"out_tids", id_map_json(views.outputs)},
            {"ddr_transfer", id_map_json(ddr_transfer)},
            {"ddr_contention_log", contention},
            {"memory_peak", peak},
            {"memory_used_final", used},
            {"memory_events", events},
            {"remaining_consumers", id_map_json(remaining_consumers)},
            {"resident_tensors_final", resident},
            {"execution_contract_validated", true}};
}
PreparedTask prepare_step3_execution(const Json &graph, const Capacity &capacity,
                                     double bandwidth) {
    PreparedTask task;
    task.step3 = step3_simulation(graph, capacity, bandwidth);
    task.graph = task.step3.at("execution_graph");
    task.sequence = task.graph.at("seq_ext").get<Ids>();
    task.ops = index_records(task.graph.at("ops"));
    task.tensors = index_records(task.graph.at("tensors"));
    task.views = build_graph_views(task.graph.at("ops"), task.graph.at("edges"));
    for (std::size_t i = 0; i < task.sequence.size(); ++i) {
        task.sequence_position[task.sequence[i]] = Id(i);
    }
    task.pipe_ops = task.step3.at("pipe_orders").get<std::map<std::string, Ids>>();
    return task;
}
} // namespace problem2
