// 对应 official_code/multicore_cut_evaluate_problem_1.py。
// 问题 1 / 场景 A：子图作为独立 Task，计入跨核和同核等待。
#include "multicore_cut_evaluate_problem_1.hpp"
#include "multicore_cut_evaluate_problem_2.hpp"
#include "stub_multicore_cut_and_schedule.hpp"
#include "evaluation_validation.hpp"
#include "schedule_step1.hpp"
#include "schedule_step2.hpp"
namespace problem2 {
SceneATasks build_scene_a_tasks(const Json &graph, const Json &plan, double bandwidth,
                                const Capacity &capacity) {
    SceneATasks result;
    result.plan_view = derive_multicore_plan(graph, plan);
    validate_task_order(result.plan_view);
    auto ops = index_records(graph.at("ops")), tensors = index_records(graph.at("tensors"));
    auto views = build_graph_views(graph.at("ops"), graph.at("edges"));
    std::map<Id, Id> mapping;
    for (auto it = result.plan_view.at("mapping").begin();
         it != result.plan_view.at("mapping").end(); ++it) {
        mapping[std::stoll(it.key())] = number(it.value());
    }
    Id next_op = ops.empty() ? 1 : ops.rbegin()->first + 1,
       next_tensor = tensors.empty() ? 10001 : std::max<Id>(10000, tensors.rbegin()->first) + 1;
    IdSet used = record_ids(ops);
    auto tids = record_ids(tensors);
    used.insert(tids.begin(), tids.end());
    auto new_ids = [&]() {
        while (used.count(next_tensor)) {
            ++next_tensor;
        }
        Id ddr = next_tensor++;
        used.insert(ddr);
        while (used.count(next_op)) {
            ++next_op;
        }
        Id copy = next_op++;
        used.insert(copy);
        return std::make_pair(ddr, copy);
    };
    Id task_copy = 0, spill_copy = 0;
    for (const auto &bvalue : result.plan_view.at("subgraph_ids")) {
        Id b = number(bvalue);
        auto members = result.plan_view.at("nodes_by_subgraph").at(key(b)).get<IdSet>();
        Json local = {{"ops", array()}, {"tensors", array()}, {"edges", array()}};
        for (Id op : members) {
            local["ops"].push_back(ops.at(op));
        }
        // 后面的 COPY 构造函数需要捕获 tid，使用 C++17 允许捕获的普通变量。
        for (const auto &tensor_entry : tensors) {
            const Id tid = tensor_entry.first;
            const auto &tensor = tensor_entry.second;
            IdSet producers, consumers, eligible_consumers;
            bool copy_out = false;
            for (Id op : views.producers[tid]) {
                if (members.count(op)) {
                    producers.insert(op);
                }
            }
            for (Id op : views.consumers[tid]) {
                if (members.count(op)) {
                    consumers.insert(op);
                }
                if (mapping.count(op)) {
                    eligible_consumers.insert(op);
                }
                if (ops.at(op).at("op") == "COPY_OUT") {
                    copy_out = true;
                }
            }
            if (producers.empty() && consumers.empty()) {
                continue;
            }
            bool input = !consumers.empty() && producers.empty(), remote = false;
            IdSet remote_tasks;
            for (Id op : eligible_consumers) {
                if (!members.count(op)) {
                    remote = true;
                    remote_tasks.insert(mapping.at(op));
                }
            }
            bool output = !producers.empty() && (copy_out || eligible_consumers.empty() || remote);
            Json t = tensor;
            if (t.at("pos") == "DDR") {
                t["pos"] = "UB";
            }
            local["tensors"].push_back(t);
            Id size = field_id(t, "size");
            for (Id op : producers) {
                local["edges"].push_back({{"source", op}, {"target", tid}});
            }
            for (Id op : consumers) {
                local["edges"].push_back({{"source", tid}, {"target", op}});
            }
            auto copy = [&](bool out) {
                auto [ddr, id] = new_ids();
                local["tensors"].push_back({{"id", ddr}, {"pos", "DDR"}, {"size", size}});
                local["ops"].push_back({{"id", id},
                                        {"op", out ? "COPY_OUT" : "COPY_IN"},
                                        {"pipe", out ? "PIPE_MTE3" : "PIPE_MTE2"},
                                        {"cycles", copy_duration(size, bandwidth)}});
                local["edges"].push_back({{"source", out ? tid : ddr}, {"target", id}});
                local["edges"].push_back({{"source", id}, {"target", out ? ddr : tid}});
            };
            if (input) {
                copy(false);
            }
            if (output) {
                copy(true);
                result.cross_traffic += size * Id(remote_tasks.size());
            }
        }
        for (const auto &e : graph.at("edges")) {
            if (members.count(field_id(e, "source")) && members.count(field_id(e, "target"))) {
                local["edges"].push_back(e);
            }
        }
        task_copy += copy_traffic_bytes(local);
        auto seq = step1_schedule(local);
        auto spills = step2_spill_insertion(local, seq, capacity);
        for (const auto &spill : spills.at("spill_records")) {
            spill_copy +=
                field_id(spill, "size") * (1 + int(spill.at("spill_out_copies_data").get<bool>()));
        }
        result.tasks[b] =
            prepare_step3_execution(build_extended_graph(local, spills), capacity, bandwidth);
    }
    Id original = copy_traffic_bytes(graph), partition = task_copy - original;
    result.traffic = {{"original_graph_copy_bytes", original},
                      {"scheduled_copy_bytes", task_copy + spill_copy},
                      {"added_copy_bytes", partition + spill_copy},
                      {"partition_added_copy_bytes", partition},
                      {"spill_added_copy_bytes", spill_copy}};
    return result;
}
Json evaluate_scene_a(const Json &graph, const Json &plan, double bandwidth,
                      const Capacity &capacity, double cross_wait, double same_wait, Id max_iter) {
    validate_parameters(bandwidth, capacity, max_iter);
    require_number(cross_wait, "cross_core_wait");
    require_number(same_wait, "same_core_wait");
    auto built = build_scene_a_tasks(graph, plan, bandwidth, capacity);
    auto &tasks = built.tasks;
    Id cores = field_id(built.plan_view, "num_cores");
    using OpKey = std::pair<Id, Id>;
    using PipeKey = std::pair<Id, std::string>;
    std::map<Id, Id> core_of;
    std::map<Id, Ids> orders;
    Adjacency pred_tasks;
    for (const auto &[b, task] : tasks) {
        core_of[b] = number(built.plan_view.at("core_by_subgraph").at(key(b)));
        pred_tasks[b] = built.plan_view.at("subgraph_preds").at(key(b)).get<IdSet>();
    }
    for (Id c = 0; c < cores; ++c) {
        orders[c] = plan.at("core_schedules")[c].get<Ids>();
    }
    std::map<Id, std::string> task_status;
    std::map<Id, double> task_start, task_end;
    std::map<Id, std::optional<Id>> active_task;
    std::map<Id, std::optional<double>> previous_end;
    std::map<Id, std::size_t> core_index;
    std::map<OpKey, std::string> status;
    std::map<OpKey, Id> remaining;
    std::map<OpKey, double> starts, ends, work;
    std::map<PipeKey, std::optional<OpKey>> running, ready;
    std::map<PipeKey, std::size_t> cursor;
    Json contention = array();
    double now = 0, last = 0;
    for (const auto &[b, task] : tasks) {
        task_status[b] = "waiting";
    }
    auto queue = [&](OpKey item) {
        if (status[item] != "pending" || remaining[item]) {
            return;
        }
        auto [b, op] = item;
        auto &task = tasks.at(b);
        auto pipe = field_string(task.ops.at(op), "pipe");
        auto &order = task.pipe_ops.at(pipe);
        auto i = cursor[{b, pipe}];
        if (i >= order.size() || order[i] != op) {
            return;
        }
        status[item] = "ready";
        ready[{core_of[b], pipe}] = item;
    };
    auto release_time = [&](Id b) -> std::optional<double> {
        Id c = core_of[b];
        double release = previous_end[c] ? *previous_end[c] + same_wait : 0;
        for (Id p : pred_tasks[b]) {
            if (task_status[p] != "done") {
                return {};
            }
            if (core_of[p] != c) {
                release = std::max(release, task_end[p] + cross_wait);
            }
        }
        return release;
    };
    auto reschedule = [&]() {
        auto projected = project_ddr_ends(work, now);
        for (auto [item, end] : projected) {
            ends[item] = end;
        }
        return projected;
    };
    bool completed = false;
    for (Id iteration = 0; iteration < max_iter; ++iteration) {
        poll_search_deadline();
        advance_ddr_work(work, last, now);
        bool retired_ddr = false;
        for (Id c = 0; c < cores; ++c) {
            for (auto pipe : PIPES) {
                PipeKey pk = {c, pipe};
                if (!running[pk] || ends.at(*running[pk]) > now) {
                    continue;
                }
                auto item = *running[pk];
                auto [b, op] = item;
                auto &task = tasks.at(b);
                status[item] = "done";
                auto index = ++cursor[{b, pipe}];
                if (index < task.pipe_ops.at(pipe).size()) {
                    queue({b, task.pipe_ops.at(pipe)[index]});
                }
                if (work.erase(item)) {
                    retired_ddr = true;
                }
                for (Id succ : task.views.succs.at(op)) {
                    --remaining[{b, succ}];
                    queue({b, succ});
                }
                running[pk].reset();
            }
        }
        if (retired_ddr) {
            reschedule();
        }
        for (auto &[b, state] : task_status) {
            if (state == "active") {
                bool done = true;
                for (Id op : tasks.at(b).sequence) {
                    if (status[{b, op}] != "done") {
                        done = false;
                    }
                }
                if (done) {
                    Id c = core_of[b];
                    state = "done";
                    task_end[b] = now;
                    active_task[c].reset();
                    previous_end[c] = now;
                    ++core_index[c];
                }
            }
        }
        for (Id c = 0; c < cores; ++c) {
            if (!active_task[c] && core_index[c] < orders[c].size()) {
                Id b = orders[c][core_index[c]];
                auto release = release_time(b);
                if (!release || *release > now) {
                    continue;
                }
                task_status[b] = "active";
                task_start[b] = now;
                active_task[c] = b;
                for (auto pipe : PIPES) {
                    cursor[{b, pipe}] = 0;
                }
                for (Id op : tasks.at(b).sequence) {
                    status[{b, op}] = "pending";
                    remaining[{b, op}] = Id(tasks.at(b).views.preds.at(op).size());
                }
                for (Id op : tasks.at(b).sequence) {
                    queue({b, op});
                }
            }
        }
        for (Id c = 0; c < cores; ++c) {
            for (auto pipe : PIPES) {
                PipeKey pk = {c, pipe};
                if (running[pk] || !ready[pk]) {
                    continue;
                }
                auto item = *ready[pk];
                ready[pk].reset();
                auto [b, id] = item;
                auto &task = tasks.at(b);
                auto &op = task.ops.at(id);
                Id duration = op_duration(op, task.views, task.tensors, bandwidth);
                status[item] = "running";
                starts[item] = now;
                ends[item] = now + duration;
                running[pk] = item;
                if (uses_ddr_bandwidth(op, task.views, task.tensors)) {
                    advance_ddr_work(work, last, now);
                    work[item] = double(duration);
                    auto projected = reschedule();
                    Json rows = array();
                    for (auto [active, end] : projected) {
                        rows.push_back(
                            {{"task_id", active.first}, {"op_id", active.second}, {"end", end}});
                    }
                    contention.push_back(
                        {{"time", now},
                         {"issued",
                          {{"task_id", b}, {"op_id", id}, {"core_id", c}, {"pipe", pipe}}},
                         {"active_count", work.size()},
                         {"projected_ends", rows}});
                }
            }
        }
        completed = true;
        for (auto [b, state] : task_status) {
            if (state != "done") {
                completed = false;
            }
        }
        if (completed) {
            break;
        }
        double next = std::numeric_limits<double>::infinity();
        for (auto [pk, item] : running) {
            if (item) {
                next = std::min(next, ends.at(*item));
            }
        }
        for (Id c = 0; c < cores; ++c) {
            if (!active_task[c] && core_index[c] < orders[c].size()) {
                auto release = release_time(orders[c][core_index[c]]);
                if (release && *release > now) {
                    next = std::min(next, *release);
                }
            }
        }
        if (!std::isfinite(next)) {
            throw SceneAEvaluationError("multicore scheduler deadlock");
        }
        if (next <= now) {
            throw SceneAEvaluationError("multicore scheduler made no progress");
        }
        now = next;
    }
    if (!completed) {
        throw SceneAEvaluationError("max_iter exceeded");
    }
    Json timeline = array(), peaks = object(), steps = object(), dependencies = array();
    double makespan = 0;
    for (Id c = 0; c < cores; ++c) {
        Json task_rows = array(), op_rows = array(), subgraphs = array(), peak = object();
        for (auto [pos, size] : capacity) {
            peak[pos] = 0;
        }
        for (Id b : orders[c]) {
            double a = task_start.at(b), end = task_end.at(b);
            makespan = std::max(makespan, end);
            task_rows.push_back({{"task_id", b},
                                 {"subgraph_id", b},
                                 {"start", a},
                                 {"end", end},
                                 {"duration", end - a}});
            double first = std::numeric_limits<double>::infinity(), last_end = 0;
            for (Id op : tasks.at(b).sequence) {
                double x = starts.at({b, op}), y = ends.at({b, op});
                first = std::min(first, x);
                last_end = std::max(last_end, y);
                op_rows.push_back({{"task_id", b},
                                   {"op_id", op},
                                   {"op", tasks.at(b).ops.at(op).at("op")},
                                   {"pipe", tasks.at(b).ops.at(op).at("pipe")},
                                   {"start", x},
                                   {"end", y},
                                   {"duration", y - x}});
            }
            if (!std::isfinite(first)) {
                throw SceneAEvaluationError("subgraph has no scheduled op");
            }
            subgraphs.push_back({{"subgraph_id", b},
                                 {"start", first},
                                 {"end", last_end},
                                 {"duration", last_end - first}});
            for (auto [pos, size] : capacity) {
                peak[pos] = std::max(number(peak.at(pos)),
                                     number(tasks.at(b).step3.at("memory_peak").at(pos)));
            }
        }
        std::stable_sort(op_rows.begin(), op_rows.end(), [](const Json &a, const Json &b) {
            return std::make_tuple(a.at("start").get<double>(), field_id(a, "task_id"),
                                   field_id(a, "op_id")) <
                   std::make_tuple(b.at("start").get<double>(), field_id(b, "task_id"),
                                   field_id(b, "op_id"));
        });
        timeline.push_back(
            {{"core_id", c}, {"tasks", task_rows}, {"subgraphs", subgraphs}, {"ops", op_rows}});
        peaks[key(c)] = peak;
    }
    for (const auto &[b, task] : tasks) {
        Json counts = object();
        for (auto pipe : PIPES) {
            counts[pipe] = task.pipe_ops.at(pipe).size();
        }
        steps[key(b)] = {{"local_makespan", task.step3.at("makespan")},
                         {"memory_dependency_count", task.step3.at("memory_dependencies").size()},
                         {"pipe_op_counts", counts}};
    }
    for (const auto &e : built.plan_view.at("dependency_pairs")) {
        dependencies.push_back({{"source", e[0]}, {"target", e[1]}});
    }
    return {{"scene", "A"},
            {"makespan", makespan},
            {"num_cores", cores},
            {"bandwidth_bytes_per_cycle", bandwidth},
            {"capacity_bytes", capacity},
            {"memory_peak_by_core", peaks},
            {"step3_by_task", steps},
            {"task_cross_core_wait_cycles", cross_wait},
            {"task_same_core_wait_cycles", same_wait},
            {"cross_task_traffic", built.cross_traffic},
            {"data_movement_bytes", built.traffic},
            {"task_dependencies", dependencies},
            {"per_core_timeline", timeline},
            {"ddr_contention_log", contention}};
}
} // namespace problem2
