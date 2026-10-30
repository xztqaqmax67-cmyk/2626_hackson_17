// 对应 official_code/multicore_cut_evaluate_problem_2.py。
// 问题 2 / 场景 B：同一核心的子图合并为 Task，以 COPY 显式表示跨核搬运。
#include "multicore_cut_evaluate_problem_2.hpp"
#include "stub_multicore_cut_and_schedule.hpp"
#include "evaluation_validation.hpp"
#include "schedule_step1.hpp"
#include "schedule_step2.hpp"
#include "multicore_cut_evaluate_problem_3.hpp"
namespace problem2 {
Id copy_traffic_bytes(const Json &graph) {
    auto v = build_graph_views(graph.at("ops"), graph.at("edges"));
    auto tensors = index_records(graph.at("tensors"));
    Id total = 0;
    for (const auto &op : graph.at("ops")) {
        if (is_copy(op)) {
            for (Id tid : (op.at("op") == "COPY_IN" ? v.outputs : v.inputs).at(field_id(op))) {
                if (tensors.count(tid)) {
                    total += field_id(tensors.at(tid), "size");
                }
            }
        }
    }
    return total;
}
SceneBTasks build_scene_b_tasks(const Json &graph, const Json &plan, double bandwidth,
                                const Capacity &capacity, bool skip_execution, bool boundary_only) {
    SceneBTasks result;
    result.plan_view = derive_multicore_plan(graph, plan);
    result.links = array();
    auto ops = index_records(graph.at("ops")), tensors = index_records(graph.at("tensors"));
    auto views = build_graph_views(graph.at("ops"), graph.at("edges"));
    Id cores = field_id(result.plan_view, "num_cores");
    std::map<Id, Id> mapping, core_of;
    std::map<std::pair<Id, Id>, Id> rank;
    for (auto it = result.plan_view.at("mapping").begin();
         it != result.plan_view.at("mapping").end(); ++it) {
        Id op = std::stoll(it.key()), b = number(it.value());
        mapping[op] = b;
        core_of[op] = number(result.plan_view.at("core_by_subgraph").at(key(b)));
    }
    struct TaskData {
        Json ops = array(), tensors = object(), edges = array();
        std::map<Id, Id> op_subgraph;
        Ids order;
    };
    std::vector<TaskData> data(std::size_t(cores), TaskData{});
    for (Id c = 0; c < cores; ++c) {
        data[c].order = plan.at("core_schedules")[c].get<Ids>();
        for (std::size_t i = 0; i < data[c].order.size(); ++i) {
            rank[{c, data[c].order[i]}] = Id(i);
        }
    }
    auto add_op = [&](Id c, const Json &op, Id b) {
        data[c].ops.push_back(op);
        data[c].op_subgraph[field_id(op)] = b;
    };
    // Python 字典按方案映射的插入顺序遍历；生成 COPY ID 前保留此顺序。
    for (auto it = result.plan_view.at("mapping").begin();
         it != result.plan_view.at("mapping").end(); ++it) {
        Id op = std::stoll(it.key());
        add_op(core_of[op], ops.at(op), mapping[op]);
    }
    Id next = max_record_id(graph) + 1;
    auto add_copy = [&](Id c, Id local, Id size, Id b, bool out,
                        std::optional<Id> supplied = std::nullopt) {
        Id ddr = supplied ? *supplied : next++, copy = next++;
        data[c].tensors[key(ddr)] = {{"id", ddr}, {"pos", "DDR"}, {"size", size}};
        add_op(c,
               {{"id", copy},
                {"op", out ? "COPY_OUT" : "COPY_IN"},
                {"pipe", out ? "PIPE_MTE3" : "PIPE_MTE2"},
                {"cycles", copy_duration(size, bandwidth)}},
               b);
        data[c].edges.push_back({{"source", out ? local : ddr}, {"target", copy}});
        data[c].edges.push_back({{"source", copy}, {"target", out ? ddr : local}});
        return copy;
    };
    auto boundary_block = [&](const Ids &operations, Id c, bool latest) {
        std::optional<Id> best;
        for (Id op : operations) {
            if (core_of.at(op) == c) {
                Id b = mapping.at(op);
                if (!best ||
                    (latest ? rank[{c, b}] > rank[{c, *best}] : rank[{c, b}] < rank[{c, *best}])) {
                    best = b;
                }
            }
        }
        return best.value();
    };
    auto link = [&](Id tid, Id size, Id src, Id dst, Id out, Id in) {
        result.links.push_back({{"tensor_id", tid},
                                {"size", size},
                                {"source_core", src},
                                {"target_core", dst},
                                {"source_copy_out_id", out},
                                {"target_copy_in_id", in}});
        result.cross_traffic += size;
    };
    for (const auto &[tid, tensor] : tensors) {
        Ids producers, consumers;
        IdSet pc, cc, touched;
        for (Id op : views.producers[tid]) {
            if (mapping.count(op)) {
                producers.push_back(op);
                pc.insert(core_of[op]);
            }
        }
        for (Id op : views.consumers[tid]) {
            if (mapping.count(op)) {
                consumers.push_back(op);
                cc.insert(core_of[op]);
            }
        }
        touched = pc;
        touched.insert(cc.begin(), cc.end());
        if (touched.empty()) {
            continue;
        }
        Json local = tensor;
        if (local.at("pos") == "DDR") {
            local["pos"] = "UB";
        }
        Id size = field_id(tensor, "size");
        for (Id c : touched) {
            data[c].tensors[key(tid)] = local;
            for (Id op : producers) {
                if (core_of[op] == c) {
                    data[c].edges.push_back({{"source", op}, {"target", tid}});
                }
            }
            for (Id op : consumers) {
                if (core_of[op] == c) {
                    data[c].edges.push_back({{"source", tid}, {"target", op}});
                }
            }
        }
        if (!consumers.empty() && producers.empty()) {
            for (Id c : cc) {
                add_copy(c, tid, size, boundary_block(consumers, c, false), false);
            }
        }
        bool original_out = false;
        for (Id op : views.consumers[tid]) {
            if (ops.at(op).at("op") == "COPY_OUT") {
                original_out = true;
            }
        }
        if (!producers.empty() && (original_out || consumers.empty())) {
            for (Id c : pc) {
                add_copy(c, tid, size, boundary_block(producers, c, true), true);
            }
        }
        for (Id src : pc) {
            for (Id dst : cc) {
                if (src != dst) {
                    Id ddr = next++;
                    Id out =
                        add_copy(src, tid, size, boundary_block(producers, src, true), true, ddr);
                    Id in =
                        add_copy(dst, tid, size, boundary_block(consumers, dst, false), false, ddr);
                    link(tid, size, src, dst, out, in);
                }
            }
        }
    }
    for (const auto &e : graph.at("edges")) {
        Id a = field_id(e, "source"), b = field_id(e, "target");
        if (a == b || !mapping.count(a) || !mapping.count(b)) {
            continue;
        }
        Id src = core_of[a], dst = core_of[b];
        if (src == dst) {
            data[src].edges.push_back(e);
            continue;
        }
        Id size = std::max<Id>(0, e.value("data_size", Id(0))), local = next++, ddr = next++;
        Json tensor = {{"id", local}, {"pos", "UB"}, {"size", size}};
        data[src].tensors[key(local)] = tensor;
        data[dst].tensors[key(local)] = tensor;
        data[src].edges.push_back({{"source", a}, {"target", local}});
        data[dst].edges.push_back({{"source", local}, {"target", b}});
        Id out = add_copy(src, local, size, mapping[a], true, ddr),
           in = add_copy(dst, local, size, mapping[b], false, ddr);
        link(local, size, src, dst, out, in);
    }
    Id task_copy = 0, spill_copy = 0;
    for (Id c = 0; c < cores; ++c) {
        auto &d = data[c];
        Json local = {{"ops", d.ops}, {"tensors", array()}, {"edges", d.edges}};
        for (const auto &t : d.tensors) {
            local["tensors"].push_back(t);
        }
        task_copy += copy_traffic_bytes(local);
        if (boundary_only) {
            PreparedTask task;
            task.raw_graph = local;
            task.raw_op_subgraph = d.op_subgraph;
            task.subgraphs = d.order;
            result.tasks[c] = std::move(task);
            continue;
        }
        auto seq = step1_schedule(local);
        std::stable_sort(seq.begin(), seq.end(), [&](Id a, Id b) {
            return rank[{c, d.op_subgraph.at(a)}] < rank[{c, d.op_subgraph.at(b)}];
        });
        if (!check_topo(local, seq)) {
            throw SceneBEvaluationError(
                "subgraph priority order violates an intra-core dependency");
        }
        auto spills = step2_spill_insertion(local, seq, capacity);
        auto op_subgraph = d.op_subgraph;
        for (const auto &spill : spills.at("spill_records")) {
            spill_copy +=
                field_id(spill, "size") * (1 + int(spill.at("spill_out_copies_data").get<bool>()));
            Id next_b = op_subgraph.at(field_id(spill, "next_use_op"));
            if (!spill.at("spill_out_id").is_null()) {
                op_subgraph[field_id(spill, "spill_out_id")] =
                    spill.at("prev_use_op").is_null()
                        ? next_b
                        : op_subgraph.at(field_id(spill, "prev_use_op"));
            }
            op_subgraph[field_id(spill, "spill_in_id")] = next_b;
        }
        PreparedTask task;
        if (skip_execution) {
            task.graph = build_extended_graph(local, spills);
            task.sequence = spills.at("seq_ext").get<Ids>();
        } else {
            task =
                prepare_step3_execution(build_extended_graph(local, spills), capacity, bandwidth);
        }
        task.raw_graph = local;
        task.raw_sequence = seq;
        task.spill_result = spills;
        task.raw_op_subgraph = d.op_subgraph;
        task.subgraphs = d.order;
        task.op_subgraph = op_subgraph;
        result.tasks[c] = std::move(task);
    }
    Id original = copy_traffic_bytes(graph), partition = task_copy - original;
    result.traffic = {{"original_graph_copy_bytes", original},
                      {"scheduled_copy_bytes", task_copy + spill_copy},
                      {"added_copy_bytes", partition + spill_copy},
                      {"partition_added_copy_bytes", partition},
                      {"spill_added_copy_bytes", spill_copy}};
    return result;
}
void validate_execution(const std::map<Id, PreparedTask> &tasks, const Json &links) {
    if (links.empty()) {
        return;
    }
    std::map<std::pair<Id, Id>, Id> ids;
    Ids nodes;
    for (const auto &[core, task] : tasks) {
        for (const auto &[op, row] : task.ops) {
            Id id = Id(ids.size());
            ids[{core, op}] = id;
            nodes.push_back(id);
        }
    }
    std::vector<std::tuple<Id, Id, std::string>> edges;
    for (const auto &[core, task] : tasks) {
        for (const auto &[op, preds] : task.views.preds) {
            for (Id p : preds) {
                edges.emplace_back(ids.at({core, p}), ids.at({core, op}),
                                   "local data/memory dependency");
            }
        }
        for (const auto &[pipe, order] : task.pipe_ops) {
            for (std::size_t i = 1; i < order.size(); ++i) {
                edges.emplace_back(ids.at({core, order[i - 1]}), ids.at({core, order[i]}),
                                   pipe + " FIFO");
            }
        }
    }
    for (const auto &link : links) {
        edges.emplace_back(
            ids.at({field_id(link, "source_core"), field_id(link, "source_copy_out_id")}),
            ids.at({field_id(link, "target_core"), field_id(link, "target_copy_in_id")}),
            "cross-core COPY");
    }
    check_acyclic(nodes, edges, "global execution (core, op)");
}
Json evaluate_scene_b(const Json &graph, const Json &plan, double bandwidth,
                      const Capacity &capacity, double delay, Id max_iter) {
    return evaluate_scene_b_with_cache(graph, plan, bandwidth, capacity, delay, max_iter, nullptr);
}
Json evaluate_scene_b_with_cache(const Json &graph, const Json &plan, double bandwidth,
                                 const Capacity &capacity, double delay, Id max_iter,
                                 ReadOnlyCache *cache) {
    validate_parameters(bandwidth, capacity, max_iter);
    require_number(delay, "cross_core_copy_delay");
    auto built = build_scene_b_tasks(graph, plan, bandwidth, capacity);
    auto &tasks = built.tasks;
    validate_execution(tasks, built.links);
    Id cores = field_id(built.plan_view, "num_cores");
    using OpKey = std::pair<Id, Id>;
    using PipeKey = std::pair<Id, std::string>;
    std::map<OpKey, std::string> status;
    std::map<OpKey, Id> remaining;
    std::map<OpKey, double> starts, ends, work;
    std::map<OpKey, double> cache_work;
    double cache_last = 0;
    std::map<OpKey, std::vector<OpKey>> external_preds, external_succs;
    std::map<PipeKey, std::size_t> cursor;
    std::map<PipeKey, std::optional<OpKey>> running, ready;
    std::set<std::pair<double, OpKey>> releases;
    std::set<OpKey> scheduled;
    double last = 0, now = 0;
    Id unfinished = 0;
    for (const auto &link : built.links) {
        OpKey a = {field_id(link, "source_core"), field_id(link, "source_copy_out_id")},
              b = {field_id(link, "target_core"), field_id(link, "target_copy_in_id")};
        external_preds[b].push_back(a);
        external_succs[a].push_back(b);
    }
    for (const auto &[c, t] : tasks) {
        for (Id op : t.sequence) {
            status[{c, op}] = "pending";
            remaining[{c, op}] = Id(t.views.preds.at(op).size());
            ++unfinished;
        }
    }
    auto queue = [&](OpKey item) {
        if (status[item] != "pending" || remaining[item]) {
            return;
        }
        auto [c, op] = item;
        auto &task = tasks.at(c);
        auto pipe = field_string(task.ops.at(op), "pipe");
        PipeKey pk = {c, pipe};
        const auto &order = task.pipe_ops.at(pipe);
        if (cursor[pk] >= order.size() || order[cursor[pk]] != op) {
            return;
        }
        double release = 0;
        for (auto pred : external_preds[item]) {
            if (status[pred] != "done") {
                return;
            }
            release = std::max(release, ends.at(pred) + delay);
        }
        if (release > now) {
            if (scheduled.insert(item).second) {
                releases.insert({release, item});
            }
            return;
        }
        scheduled.erase(item);
        status[item] = "ready";
        ready[pk] = item;
    };
    for (const auto &[c, t] : tasks) {
        for (Id op : t.sequence) {
            queue({c, op});
        }
    }
    auto reschedule = [&]() {
        for (auto [item, end] : project_ddr_ends(work, now)) {
            ends[item] = end;
        }
    };
    auto reschedule_cache = [&]() {
        for (auto [item, end] : project_ddr_ends(cache_work, now)) {
            ends[item] = end;
        }
    };
    bool completed = false;
    for (Id iteration = 0; iteration < max_iter; ++iteration) {
        poll_search_deadline();
        advance_ddr_work(work, last, now);
        bool retired_ddr = false;
        std::vector<OpKey> retired;
        advance_ddr_work(cache_work, cache_last, now);
        bool retired_cache = false;
        for (Id c = 0; c < cores; ++c) {
            for (auto pipe : PIPES) {
                PipeKey pk = {c, pipe};
                if (!running[pk] || ends[*running[pk]] > now + 1e-9) {
                    continue;
                }
                auto item = *running[pk];
                Id op = item.second;
                auto &task = tasks.at(c);
                status[item] = "done";
                --unfinished;
                retired.push_back(item);
                if (work.erase(item)) {
                    retired_ddr = true;
                }
                if (cache_work.erase(item)) {
                    retired_cache = true;
                }
                if (cache) {
                    cache->retire(task, c, op, now);
                }
                auto index = ++cursor[pk];
                if (index < task.pipe_ops.at(pipe).size()) {
                    queue({c, task.pipe_ops.at(pipe)[index]});
                }
                for (Id succ : task.views.succs.at(op)) {
                    --remaining[{c, succ}];
                    queue({c, succ});
                }
                running[pk].reset();
            }
        }
        if (retired_ddr) {
            reschedule();
        }
        for (auto item : retired) {
            for (auto target : external_succs[item]) {
                queue(target);
            }
        }
        if (retired_cache) {
            reschedule_cache();
        }
        while (!releases.empty() && releases.begin()->first <= now) {
            auto item = releases.begin()->second;
            releases.erase(releases.begin());
            scheduled.erase(item);
            queue(item);
        }
        for (Id c = 0; c < cores; ++c) {
            for (auto pipe : PIPES) {
                PipeKey pk = {c, pipe};
                if (running[pk] || !ready[pk]) {
                    continue;
                }
                auto item = *ready[pk];
                ready[pk].reset();
                auto &task = tasks.at(c);
                const auto &op = task.ops.at(item.second);
                bool hit = cache && cache->issue(task, c, item.second, now);
                Id duration =
                    op_duration(op, task.views, task.tensors, hit ? cache->bandwidth() : bandwidth);
                status[item] = "running";
                starts[item] = now;
                ends[item] = now + duration;
                running[pk] = item;
                if (hit) {
                    advance_ddr_work(cache_work, cache_last, now);
                    cache_work[item] = double(duration);
                    reschedule_cache();
                } else if (uses_ddr_bandwidth(op, task.views, task.tensors)) {
                    if (cache) {
                        cache->mark_ddr(c, item.second);
                    }
                    advance_ddr_work(work, last, now);
                    work[item] = double(duration);
                    reschedule();
                }
            }
        }
        if (!unfinished) {
            completed = true;
            break;
        }
        double next = std::numeric_limits<double>::infinity();
        for (const auto &[pk, item] : running) {
            if (item) {
                next = std::min(next, ends.at(*item));
            }
        }
        if (!releases.empty()) {
            next = std::min(next, releases.begin()->first);
        }
        if (!std::isfinite(next)) {
            throw SceneBEvaluationError("multicore scheduler deadlock at t=" + std::to_string(now));
        }
        if (next <= now) {
            throw SceneBEvaluationError("multicore scheduler made no progress");
        }
        now = next;
    }
    if (!completed) {
        throw SceneBEvaluationError("max_iter exceeded");
    }
    Json timeline = array(), peaks = object(), steps = object();
    double makespan = 0;
    for (Id c = 0; c < cores; ++c) {
        const auto &task = tasks.at(c);
        Json entries = array(), subgraphs = array(), counts = object();
        double first = task.sequence.empty() ? 0 : std::numeric_limits<double>::infinity(),
               last_end = 0;
        for (Id op : task.sequence) {
            double a = starts.at({c, op}), b = ends.at({c, op});
            first = std::min(first, a);
            last_end = std::max(last_end, b);
            entries.push_back(
                {{"task_id", c},
                 {"op_id", op},
                 {"op", task.ops.at(op).at("op")},
                 {"subgraph_id",
                  task.op_subgraph.count(op) ? Json(task.op_subgraph.at(op)) : Json()},
                 {"pipe", task.ops.at(op).at("pipe")},
                 {"start", a},
                 {"end", b},
                 {"duration", b - a}});
        }
        if (cache) {
            for (auto &entry : entries) {
                cache->annotate(entry, c, field_id(entry, "op_id"));
            }
        }
        std::stable_sort(entries.begin(), entries.end(), [](const Json &a, const Json &b) {
            return std::make_pair(a.at("start").get<double>(), field_id(a, "op_id")) <
                   std::make_pair(b.at("start").get<double>(), field_id(b, "op_id"));
        });
        for (Id b : task.subgraphs) {
            double a = std::numeric_limits<double>::infinity(), end = 0;
            for (const auto &row : entries) {
                if (row.at("subgraph_id") == b) {
                    a = std::min(a, row.at("start").get<double>());
                    end = std::max(end, row.at("end").get<double>());
                }
            }
            if (!std::isfinite(a)) {
                throw SceneBEvaluationError("subgraph has no scheduled op");
            }
            subgraphs.push_back(
                {{"subgraph_id", b}, {"start", a}, {"end", end}, {"duration", end - a}});
        }
        timeline.push_back({{"core_id", c},
                            {"tasks", Json::array({Json{{"task_id", c},
                                                        {"subgraph_ids", task.subgraphs},
                                                        {"start", first},
                                                        {"end", last_end},
                                                        {"duration", last_end - first}}})},
                            {"subgraphs", subgraphs},
                            {"ops", entries}});
        makespan = std::max(makespan, last_end);
        peaks[key(c)] = task.step3.at("memory_peak");
        for (auto pipe : PIPES) {
            counts[pipe] = task.pipe_ops.at(pipe).size();
        }
        steps[key(c)] = {{"local_makespan", task.step3.at("makespan")},
                         {"memory_dependency_count", task.step3.at("memory_dependencies").size()},
                         {"pipe_op_counts", counts}};
    }
    Json transfers = array();
    for (auto link : built.links) {
        OpKey source = {field_id(link, "source_core"), field_id(link, "source_copy_out_id")},
              target = {field_id(link, "target_core"), field_id(link, "target_copy_in_id")};
        link["transfer_id"] = transfers.size();
        link["copy_out_end"] = ends.at(source);
        link["copy_in_release"] = ends.at(source) + delay;
        link["copy_in_start"] = starts.at(target);
        link["copy_in_end"] = ends.at(target);
        transfers.push_back(link);
    }
    Json result = {{"scene", "B"},
                   {"makespan", makespan},
                   {"num_cores", cores},
                   {"bandwidth_bytes_per_cycle", bandwidth},
                   {"capacity_bytes", capacity},
                   {"memory_peak_by_core", peaks},
                   {"step3_by_core", steps},
                   {"cross_core_copy_delay_cycles", delay},
                   {"cross_task_traffic", built.cross_traffic},
                   {"data_movement_bytes", built.traffic},
                   {"task_count", cores},
                   {"task_dependencies", built.links},
                   {"cross_core_transfers", transfers},
                   {"per_core_timeline", timeline}};
    if (cache) {
        cache->finish(result);
    }
    return result;
}
} // namespace problem2
