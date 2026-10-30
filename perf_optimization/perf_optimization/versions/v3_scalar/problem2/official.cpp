// 对应 official.py。
// 把官方评估器包装成搜索可用的统一接口，并提取搬运与生命周期信息。
#include "official.hpp"
#include "utils.hpp"
#include "official_code/evaluation_validation.hpp"
#include "official_code/multicore_cut_evaluate_problem_2.hpp"
#include "official_code/schedule_step2.hpp"
#include "official_code/stub_multicore_cut_and_schedule.hpp"
namespace problem2::official {
Json load_graph(const fs::path &path) {
    auto graph = read_json_strict(path);
    validate_graph(graph);
    return graph;
}
Settings load_settings(const fs::path &path) {
    auto settings = read_evaluation_config(path);
    settings.cross_core_copy_delay =
        read_required_settings(path, "multicore_scene_b", {"cross_core_copy_delay_cycles"})
            .at("cross_core_copy_delay_cycles");
    return settings;
}
namespace {
std::string error_type(const std::exception &e) {
    if (dynamic_cast<const EvaluationValidationError *>(&e)) {
        return "EvaluationValidationError";
    }
    if (dynamic_cast<const MulticoreCutError *>(&e)) {
        return "MulticoreCutError";
    }
    if (dynamic_cast<const SceneBEvaluationError *>(&e)) {
        return "SceneBEvaluationError";
    }
    if (dynamic_cast<const Step2SchedulingError *>(&e)) {
        return "Step2SchedulingError";
    }
    if (dynamic_cast<const Step3SchedulingError *>(&e)) {
        return "Step3SchedulingError";
    }
    return {};
}
Json failure(const std::exception &e) {
    if (error_type(e).empty()) {
        throw;
    }
    std::string message = e.what();
    return {
        {"status", message.find("max_iter exceeded") != std::string::npos ? "timeout" : "invalid"},
        {"error", message},
        {"error_type", error_type(e)}};
}
// 同时保留逻辑序列位置和实际执行时间，避免把估算生命周期当作物理驻留。
Json lifetimes(const Json &graph, const Ids &sequence, const std::map<Id, Id> &blocks, Id core,
               const Records &timelines = {}) {
    auto views = build_graph_views(graph.at("ops"), graph.at("edges"));
    std::map<Id, Id> positions;
    for (std::size_t i = 0; i < sequence.size(); ++i) {
        positions[sequence[i]] = Id(i);
    }
    Json records = array();
    for (const auto &tensor : graph.at("tensors")) {
        Id tid = field_id(tensor);
        auto pos = field_string(tensor, "pos");
        if (pos != "L1" && pos != "UB") {
            continue;
        }
        auto sorted = [&](const IdSet &set) {
            auto list = as_vector(set);
            std::sort(list.begin(), list.end(),
                      [&](Id a, Id b) { return positions.at(a) < positions.at(b); });
            return list;
        };
        auto producers = sorted(views.producers[tid]), consumers = sorted(views.consumers[tid]);
        IdSet all = views.producers[tid];
        all.insert(views.consumers[tid].begin(), views.consumers[tid].end());
        auto uses = sorted(all);
        if (uses.empty()) {
            continue;
        }
        Id first = uses.front(), last = uses.back();
        IdSet pb, cb;
        for (Id op : producers) {
            if (blocks.count(op)) {
                pb.insert(blocks.at(op));
            }
        }
        for (Id op : consumers) {
            if (blocks.count(op)) {
                cb.insert(blocks.at(op));
            }
        }
        Json row = {{"tensor_id", tid},
                    {"logical_tid", tensor.value("logical_tid", tid)},
                    {"size", tensor.at("size")},
                    {"pos", pos},
                    {"core", core},
                    {"producer_ops", producers},
                    {"consumer_ops", consumers},
                    {"producer_blocks", pb},
                    {"consumer_blocks", cb},
                    {"first_use_op", first},
                    {"last_use_op", last},
                    {"last_use_block", blocks.count(last) ? Json(blocks.at(last)) : Json()},
                    {"first_index", positions.at(first)},
                    {"last_index", positions.at(last)},
                    {"spill_bytes", 0}};
        Ids actual_prod, actual_cons;
        for (Id op : producers) {
            if (timelines.count(op)) {
                actual_prod.push_back(op);
            }
        }
        for (Id op : consumers) {
            if (timelines.count(op)) {
                actual_cons.push_back(op);
            }
        }
        if (!actual_prod.empty() || !actual_cons.empty()) {
            double allocation = actual_prod.empty() ? 0 : std::numeric_limits<double>::infinity(),
                   release = 0;
            for (Id op : actual_prod) {
                allocation = std::min(allocation, timelines.at(op).at("start").get<double>());
            }
            const auto &release_ops = actual_cons.empty() ? actual_prod : actual_cons;
            Id actual_last = release_ops.front();
            for (Id op : release_ops) {
                if (timelines.at(op).at("end").get<double>() > release) {
                    release = timelines.at(op).at("end").get<double>();
                    actual_last = op;
                }
            }
            row["allocation_time"] = allocation;
            row["release_time"] = release;
            row["actual_last_use_op"] = actual_last;
        }
        records.push_back(row);
    }
    return records;
}
Json boundary_result(const Json &graph, const SceneBTasks &built) {
    Json tasks = object(), links = array();
    std::map<Id, Json> ledger;
    std::set<std::pair<Id, Id>> cross_copies;
    auto original_ops = index_records(graph.at("ops"));
    auto original_tensors = index_records(graph.at("tensors"));
    std::vector<Json> direct;
    for (const auto &e : graph.at("edges")) {
        if (original_ops.count(field_id(e, "source")) &&
            original_ops.count(field_id(e, "target")) &&
            field_id(e, "source") != field_id(e, "target")) {
            direct.push_back(e);
        }
    }
    for (auto link : built.links) {
        Id src = field_id(link, "source_core"), dst = field_id(link, "target_core"),
           out = field_id(link, "source_copy_out_id"), in = field_id(link, "target_copy_in_id"),
           tid = field_id(link, "tensor_id");
        link["source_block"] = built.tasks.at(src).raw_op_subgraph.at(out);
        link["target_block"] = built.tasks.at(dst).raw_op_subgraph.at(in);
        links.push_back(link);
        cross_copies.insert({src, out});
        cross_copies.insert({dst, in});
        Json row = link;
        row["bytes"] = 2 * field_id(link, "size");
        if (original_tensors.count(tid)) {
            row["kind"] = "cross";
            row["key"] = {"cross", tid, src, dst};
        } else {
            auto sv = build_graph_views(built.tasks.at(src).raw_graph.at("ops"),
                                        built.tasks.at(src).raw_graph.at("edges")),
                 dv = build_graph_views(built.tasks.at(dst).raw_graph.at("ops"),
                                        built.tasks.at(dst).raw_graph.at("edges"));
            Id a = -1, b = -1;
            for (Id op : sv.producers[tid]) {
                if (original_ops.count(op)) {
                    a = op;
                }
            }
            for (Id op : dv.consumers[tid]) {
                if (original_ops.count(op)) {
                    b = op;
                }
            }
            Id index = 0;
            for (; index < Id(direct.size()); ++index) {
                if (field_id(direct[index], "source") == a &&
                    field_id(direct[index], "target") == b) {
                    break;
                }
            }
            row["kind"] = "direct";
            row["key"] = {"direct", index, a, b, src, dst};
        }
        ledger[out] = row;
    }
    for (const auto &[c, task] : built.tasks) {
        tasks[key(c)] = {{"op_subgraph", id_map_json(task.raw_op_subgraph)},
                         {"subgraph_order", task.subgraphs},
                         {"graph", task.raw_graph}};
        auto views = build_graph_views(task.raw_graph.at("ops"), task.raw_graph.at("edges"));
        auto tensors = index_records(task.raw_graph.at("tensors"));
        for (const auto &op : task.raw_graph.at("ops")) {
            Id id = field_id(op);
            if (!is_copy(op) || cross_copies.count({c, id})) {
                continue;
            }
            bool out = op.at("op") == "COPY_OUT";
            Id tid = (out ? views.inputs : views.outputs).at(id).front();
            Json row = {{"key", {out ? "output" : "input", tid, c}},
                        {"kind", out ? "output" : "input"},
                        {"tensor_id", tid},
                        {out ? "source_core" : "target_core", c},
                        {out ? "source_block" : "target_block", task.raw_op_subgraph.at(id)},
                        {out ? "source_copy_out_id" : "target_copy_in_id", id},
                        {"bytes", tensors.at(tid).at("size")}};
            ledger[id] = row;
        }
    }
    Json rows = array();
    Id total = 0;
    for (const auto &[id, row] : ledger) {
        rows.push_back(row);
        total += field_id(row, "bytes");
    }
    Id original = copy_traffic_bytes(graph);
    return {{"tasks", tasks},
            {"cross_links", links},
            {"plan_view", built.plan_view},
            {"route",
             {{"total_ddr_bytes", total},
              {"original_copy_bytes", original},
              {"partition_added_copy_bytes", total - original},
              {"cross_task_traffic", built.cross_traffic},
              {"ledger", rows}}}};
}
} // namespace
// 使用与正式评估相同的 Task 构造规则，准确分配新增 COPY 和张量 ID。
Json build_boundary_graphs(const Json &graph, const Json &plan, double bandwidth) {
    auto built = build_scene_b_tasks(graph, plan, bandwidth, {}, true, true);
    return boundary_result(graph, built);
}
Json route_cost(const Json &graph, const Json &plan) {
    return build_boundary_graphs(graph, plan).at("route");
}
// 展开完整 Task，再提取热点、时间线和生命周期，供局部搜索定位修改区域。
Json build_profile(const Json &graph, const Json &plan, const Settings &settings,
                   const Json &wrapped) {
    try {
        auto built = build_scene_b_tasks(graph, plan, settings.bandwidth, settings.capacity, true);
        auto boundary = boundary_result(graph, built);
        Json result = wrapped.is_object() && wrapped.contains("official_result")
                          ? wrapped.at("official_result")
                          : wrapped;
        std::map<Id, Records> timelines;
        if (result.is_object()) {
            for (const auto &core : result.value("per_core_timeline", array())) {
                for (const auto &op : core.at("ops")) {
                    timelines[field_id(core, "core_id")][field_id(op, "op_id")] = op;
                }
            }
        }
        Json profile = {{"status", "ok"},
                        {"tensors", array()},
                        {"physical_tensors", array()},
                        {"cores", object()},
                        {"spill_records", array()},
                        {"cross_links", array()},
                        {"route_bytes", boundary.at("route").at("total_ddr_bytes")},
                        {"spill_bytes", 0},
                        {"total_ddr_bytes", 0}};
        Id total_spill = 0;
        for (const auto &[c, task] : built.tasks) {
            std::map<Id, Id> spill_by_tid;
            for (auto row : task.spill_result.at("spill_records")) {
                Id bytes =
                    field_id(row, "size") * (1 + int(row.at("spill_out_copies_data").get<bool>()));
                spill_by_tid[field_id(row, "logical_tid")] += bytes;
                row["core"] = c;
                row["bytes"] = bytes;
                profile["spill_records"].push_back(row);
            }
            auto logical = lifetimes(task.raw_graph, task.raw_sequence, task.raw_op_subgraph, c),
                 physical = lifetimes(task.graph, task.sequence, task.op_subgraph, c, timelines[c]);
            std::map<std::string, std::map<Id, Id>> events;
            for (auto [pos, size] : settings.capacity) {
                events[pos];
            }
            for (auto &row : logical) {
                row["spill_bytes"] = spill_by_tid[field_id(row, "tensor_id")];
                auto pos = field_string(row, "pos");
                events[pos][field_id(row, "first_index")] += field_id(row, "size");
                events[pos][field_id(row, "last_index") + 1] -= field_id(row, "size");
            }
            Json pressure = object(), hot = object();
            for (auto [pos, changes] : events) {
                Id current = 0, peak = 0, at = 0;
                for (auto [index, delta] : changes) {
                    current += delta;
                    if (current > peak) {
                        peak = current;
                        at = index;
                    }
                }
                pressure[pos] = peak;
                hot[pos] = at;
            }
            Id spills = 0;
            for (auto [tid, bytes] : spill_by_tid) {
                spills += bytes;
            }
            total_spill += spills;
            profile["cores"][key(c)] = {{"sequence", task.raw_sequence},
                                        {"expanded_sequence", task.sequence},
                                        {"subgraph_order", task.subgraphs},
                                        {"no_spill_pressure_peak", pressure},
                                        {"pressure_peak_index", hot},
                                        {"spill_bytes", spills},
                                        {"physical_tensor_count", physical.size()}};
            append_json(profile["tensors"], logical);
            append_json(profile["physical_tensors"], physical);
        }
        for (auto link : boundary.at("cross_links")) {
            if (result.is_object()) {
                for (const auto &transfer : result.value("cross_core_transfers", array())) {
                    if (transfer.at("source_core") == link.at("source_core") &&
                        transfer.at("source_copy_out_id") == link.at("source_copy_out_id") &&
                        transfer.at("target_core") == link.at("target_core") &&
                        transfer.at("target_copy_in_id") == link.at("target_copy_in_id")) {
                        for (auto it = transfer.begin(); it != transfer.end(); ++it) {
                            link[it.key()] = it.value();
                        }
                    }
                }
            }
            profile["cross_links"].push_back(link);
        }
        profile["spill_bytes"] = total_spill;
        profile["total_ddr_bytes"] = field_id(profile, "route_bytes") + total_spill;
        auto hotspots = profile.at("tensors");
        std::stable_sort(hotspots.begin(), hotspots.end(), [](const Json &a, const Json &b) {
            auto score = [](const Json &row) {
                return std::make_tuple(-field_id(row, "spill_bytes"),
                                       -field_id(row, "size") * (field_id(row, "last_index") -
                                                                 field_id(row, "first_index")),
                                       field_id(row, "core"), field_id(row, "tensor_id"));
            };
            return score(a) < score(b);
        });
        while (hotspots.size() > 16) {
            hotspots.erase(hotspots.end() - 1);
        }
        profile["lifetime_hotspots"] = hotspots;
        if (result.is_object()) {
            if (profile.at("total_ddr_bytes") !=
                result.at("data_movement_bytes").at("scheduled_copy_bytes")) {
                throw std::logic_error(
                    "profile boundary/spill bytes disagree with official evaluation");
            }
            profile["memory_peak_by_core"] = result.at("memory_peak_by_core");
        }
        return profile;
    } catch (const std::exception &e) {
        return failure(e);
    }
}
// 只有官方评估成功的完整方案才能成为最优解；预期的输入错误返回结构化状态。
Json evaluate(const Json &graph, const Json &plan, const Settings &settings, bool profile,
              double deadline) {
    if (deadline > 0 && monotonic() >= deadline) {
        return {
            {"status", "timeout"}, {"makespan", nullptr}, {"error", "evaluation deadline reached"}};
    }
    double started = monotonic();
    Json result;
    try {
        result = evaluate_scene_b(graph, plan, settings.bandwidth, settings.capacity,
                                  settings.cross_core_copy_delay);
    } catch (const std::exception &e) {
        auto failed = failure(e);
        failed["makespan"] = nullptr;
        failed["elapsed_seconds"] = monotonic() - started;
        return failed;
    }
    const auto &m = result.at("data_movement_bytes");
    Json l1 = object(), ub = object();
    for (auto it = result.at("memory_peak_by_core").begin();
         it != result.at("memory_peak_by_core").end(); ++it) {
        l1[it.key()] = it.value().at("L1");
        ub[it.key()] = it.value().at("UB");
    }
    Json wrapped = {{"status", "ok"},
                    {"makespan", result.at("makespan")},
                    {"added_copy_bytes", m.at("added_copy_bytes")},
                    {"total_ddr_bytes", m.at("scheduled_copy_bytes")},
                    {"spill_bytes", m.at("spill_added_copy_bytes")},
                    {"route_bytes",
                     field_id(m, "scheduled_copy_bytes") - field_id(m, "spill_added_copy_bytes")},
                    {"peak_l1_by_core", l1},
                    {"peak_ub_by_core", ub},
                    {"official_result", result},
                    {"execution_profile", nullptr}};
    if (profile) {
        wrapped["execution_profile"] = build_profile(graph, plan, settings, result);
    }
    wrapped["elapsed_seconds"] = monotonic() - started;
    return wrapped;
}
} // namespace problem2::official
