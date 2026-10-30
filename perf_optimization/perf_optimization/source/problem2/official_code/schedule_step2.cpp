// 对应 official_code/schedule_step2.py。
// 步骤 2：模拟片上内存，必要时插入换出/换入操作及重载张量。
#include "schedule_step2.hpp"
namespace problem2 {
TensorUses build_tensor_uses(const Json &graph, const Ids &sequence) {
    TensorUses uses;
    auto ops = index_records(graph.at("ops"));
    std::map<Id, Id> step;
    for (std::size_t i = 0; i < sequence.size(); ++i) {
        step[sequence[i]] = Id(i);
    }
    for (const auto &edge : graph.at("edges")) {
        Id a = field_id(edge, "source"), b = field_id(edge, "target"), tid, op;
        if (ops.count(a) && !ops.count(b)) {
            tid = b;
            op = a;
        } else if (!ops.count(a) && ops.count(b)) {
            tid = a;
            op = b;
        } else {
            continue;
        }
        if (step.count(op)) {
            uses[tid].emplace_back(step[op], op);
        }
    }
    for (auto &[tid, rows] : uses) {
        std::sort(rows.begin(), rows.end());
        rows.erase(std::unique(rows.begin(), rows.end(),
                               [](auto a, auto b) { return a.first == b.first; }),
                   rows.end());
    }
    return uses;
}
std::map<Id, Id> find_copy_in_backings(const Json &graph) {
    auto tensors = index_records(graph.at("tensors"));
    auto views = build_graph_views(graph.at("ops"), graph.at("edges"));
    Adjacency candidates;
    for (const auto &op : graph.at("ops")) {
        if (op.at("op") == "COPY_IN") {
            Id id = field_id(op);
            Ids ddr;
            for (Id tid : views.inputs[id]) {
                if (tensors.count(tid) && tensors.at(tid).at("pos") == "DDR") {
                    ddr.push_back(tid);
                }
            }
            if (ddr.size() != 1) {
                continue;
            }
            for (Id tid : views.outputs[id]) {
                if (tensors.count(tid) && tensors.at(tid).at("pos") != "DDR") {
                    candidates[tid].insert(ddr[0]);
                }
            }
        }
    }
    std::map<Id, Id> result;
    for (auto &[tid, backings] : candidates) {
        if (backings.size() == 1) {
            result[tid] = *backings.begin();
        }
    }
    return result;
}
Json step2_spill_insertion(const Json &graph, const Ids &sequence, const Capacity &capacity) {
    auto uses = build_tensor_uses(graph, sequence);
    auto tensors = index_records(graph.at("tensors"));
    auto backing = find_copy_in_backings(graph);
    std::map<Id, std::string> backing_origin;
    for (auto [tid, id] : backing) {
        backing_origin[tid] = "original_copy_in";
    }
    std::vector<std::vector<std::pair<Id, std::size_t>>> uses_at(sequence.size());
    Ids lifecycle;
    // 保留原 tensor 数组的插入顺序：Belady 遇到同样远的下次使用时依赖这一顺序。
    for (const auto &tensor : graph.at("tensors")) {
        Id tid = field_id(tensor);
        if (tensor.at("pos") == "DDR" || !uses.count(tid)) {
            continue;
        }
        lifecycle.push_back(tid);
        for (std::size_t i = 0; i < uses[tid].size(); ++i) {
            uses_at[uses[tid][i].first].emplace_back(tid, i);
        }
    }
    struct Active {
        Id tid;
        std::optional<Id> next;
        std::size_t used;
    };
    std::map<std::string, std::vector<Active>> active;
    Capacity resident;
    std::map<Id, std::vector<std::pair<Id, std::size_t>>> reload;
    Json pending = array(), overflow = array();
    auto put = [&](const std::string &pos, Id tid, std::optional<Id> next, std::size_t count) {
        for (auto &a : active[pos]) {
            if (a.tid == tid) {
                a.next = next;
                a.used = count;
                return;
            }
        }
        active[pos].push_back({tid, next, count});
    };
    auto erase = [&](const std::string &pos, Id tid) {
        auto &list = active[pos];
        auto i =
            std::find_if(list.begin(), list.end(), [&](const Active &a) { return a.tid == tid; });
        if (i == list.end()) {
            return false;
        }
        list.erase(i);
        return true;
    };
    auto next_use = [&](Id tid, std::size_t index) -> std::optional<Id> {
        if (index < uses[tid].size()) {
            return uses[tid][index].first;
        }
        return {};
    };
    for (std::size_t step = 0; step < sequence.size(); ++step) {
        poll_search_deadline();
        std::map<std::string, IdSet> current;
        for (auto [tid, index] : uses_at[step]) {
            current[field_string(tensors.at(tid), "pos")].insert(tid);
        }
        for (auto [tid, index] : reload[Id(step)]) {
            auto pos = field_string(tensors.at(tid), "pos");
            bool exists = false;
            for (auto a : active[pos]) {
                if (a.tid == tid) {
                    exists = true;
                }
            }
            if (!exists) {
                put(pos, tid, next_use(tid, index + 1), index);
                resident[pos] += field_id(tensors.at(tid), "size");
            }
        }
        std::vector<std::pair<std::string, Id>> release;
        for (auto [tid, index] : uses_at[step]) {
            auto pos = field_string(tensors.at(tid), "pos");
            if (index == 0) {
                put(pos, tid, next_use(tid, 1), 1);
                resident[pos] += field_id(tensors.at(tid), "size");
            } else if (index + 1 < uses[tid].size()) {
                for (auto &a : active[pos]) {
                    if (a.tid == tid) {
                        a.next = next_use(tid, index + 1);
                        ++a.used;
                    }
                }
            }
            if (index + 1 == uses[tid].size()) {
                release.emplace_back(pos, tid);
            }
        }
        // 必须在输出分配之后、输入释放之前检查容量，才能计入瞬时内存峰值。
        for (auto [pos, limit] : capacity) {
            while (resident[pos] > limit) {
                overflow.push_back(
                    {{"step", step}, {"type", pos}, {"resid", resident[pos]}, {"capacity", limit}});
                auto &list = active[pos];
                std::size_t victim = list.size();
                for (std::size_t i = 0; i < list.size(); ++i) {
                    if (list[i].next && !current[pos].count(list[i].tid)) {
                        if (victim == list.size() || *list[i].next > *list[victim].next) {
                            victim = i;
                        }
                    }
                }
                if (victim == list.size()) {
                    throw Step2SchedulingError(
                        "[STEP2 ERROR] no spill victim: step=" + std::to_string(step) +
                        " op=" + key(sequence[step]) + " type=" + pos);
                }
                auto v = list[victim];
                auto &u = uses[v.tid];
                Id size = field_id(tensors.at(v.tid), "size");
                pending.push_back({{"out_after_step", v.used ? u[v.used - 1].first : u[0].first},
                                   {"in_before_step", *v.next},
                                   {"tid", v.tid},
                                   {"pos", pos},
                                   {"size", size},
                                   {"prev_use_op", v.used ? Json(u[v.used - 1].second) : Json()},
                                   {"next_use_op", u.at(v.used).second}});
                reload[*v.next].emplace_back(v.tid, v.used);
                list.erase(list.begin() + victim);
                resident[pos] -= size;
            }
        }
        for (auto [pos, tid] : release) {
            if (erase(pos, tid)) {
                resident[pos] -= field_id(tensors.at(tid), "size");
            }
        }
    }
    Json new_ops = array(), new_tensors = array(), new_edges = array(), removed = array(),
         records = array();
    std::map<Id, Ids> after, before;
    std::map<Id, Id> incarnation, version;
    std::map<Id, Json> by_tid;
    for (Id tid : lifecycle) {
        incarnation[tid] = tid;
    }
    Id next_id = max_record_id(graph) + 1;
    for (const auto &sp : pending) {
        Id tid = field_id(sp, "tid"), from = incarnation[tid], size = field_id(sp, "size");
        bool copies = !backing.count(tid);
        Json out_id = copies ? Json(next_id++) : Json();
        Id in_id = next_id++;
        std::string source;
        if (copies) {
            backing[tid] = next_id++;
            backing_origin[tid] = "spill_out";
            new_tensors.push_back({{"id", backing[tid]}, {"pos", "DDR"}, {"size", size}});
            source = "new_spill_out";
        } else {
            source =
                backing_origin[tid] == "original_copy_in" ? "original_copy_in" : "reused_spill_out";
        }
        Id to = next_id++;
        incarnation[tid] = to;
        ++version[tid];
        new_tensors.push_back({{"id", to},
                               {"logical_tid", tid},
                               {"version", version[tid]},
                               {"pos", sp.at("pos")},
                               {"size", size}});
        auto copy_op = [&](Id id, bool out) {
            return Json{{"id", id},
                        {"op", out ? "COPY_OUT" : "COPY_IN"},
                        {"pipe", out ? "PIPE_MTE3" : "PIPE_MTE2"},
                        {"cycles", std::max<Id>(1, size / 64)},
                        {"transfer_bytes", size},
                        {"spill_logical_tid", tid}};
        };
        if (copies) {
            Id out = number(out_id);
            new_ops.push_back(copy_op(out, true));
            new_edges.push_back({from, out});
            new_edges.push_back({out, backing[tid]});
            after[field_id(sp, "out_after_step")].push_back(out);
        }
        new_ops.push_back(copy_op(in_id, false));
        new_edges.push_back({backing[tid], in_id});
        new_edges.push_back({in_id, to});
        before[field_id(sp, "in_before_step")].push_back(in_id);
        Json record = {{"spill_out_id", out_id},
                       {"spill_in_id", in_id},
                       {"swap_tid", backing[tid]},
                       {"backing_tid", backing[tid]},
                       {"backing_source", source},
                       {"spill_out_copies_data", copies},
                       {"tid", tid},
                       {"logical_tid", tid},
                       {"from_tid", from},
                       {"to_tid", to},
                       {"version", version[tid]},
                       {"pos", sp.at("pos")},
                       {"size", size},
                       {"prev_use_op", sp.at("prev_use_op")},
                       {"next_use_op", sp.at("next_use_op")},
                       {"prev_use_step", sp.at("out_after_step")},
                       {"next_use_step", sp.at("in_before_step")}};
        records.push_back(record);
        if (!by_tid.count(tid)) {
            by_tid[tid] = array();
        }
        by_tid[tid].push_back(record);
    }
    std::map<std::pair<Id, Id>, Id> consumer_incarnation;
    for (auto &[tid, rows] : by_tid) {
        std::size_t index = 0;
        Id physical = tid;
        for (auto [step, op] : uses[tid]) {
            while (index < rows.size() && step >= field_id(rows[index], "next_use_step")) {
                physical = field_id(rows[index++], "to_tid");
            }
            consumer_incarnation[{tid, op}] = physical;
        }
    }
    Json ext_edges = array();
    for (auto e : graph.at("edges")) {
        Id a = field_id(e, "source"), b = field_id(e, "target");
        auto it = consumer_incarnation.find({a, b});
        if (it != consumer_incarnation.end() && it->second != a) {
            removed.push_back({a, b});
            e["source"] = it->second;
        }
        ext_edges.push_back(e);
    }
    for (const auto &e : new_edges) {
        ext_edges.push_back(edge(number(e[0]), number(e[1])));
    }
    Ids extended;
    for (std::size_t i = 0; i < sequence.size(); ++i) {
        if (i) {
            append(extended, after[Id(i) - 1]);
        }
        append(extended, before[Id(i)]);
        extended.push_back(sequence[i]);
    }
    append(extended, after[Id(sequence.size()) - 1]);
    return {{"seq_ext", extended},         {"spill_records", records}, {"new_edges", new_edges},
            {"removed_edges", removed},    {"ext_edges", ext_edges},   {"new_ops", new_ops},
            {"new_tensors", new_tensors},  {"overflow_log", overflow}, {"capacity", capacity},
            {"_debug_over_count", array()}};
}
Json build_extended_graph(const Json &graph, const Json &result) {
    Json output = {{"ops", graph.at("ops")},
                   {"tensors", graph.at("tensors")},
                   {"edges", result.value("ext_edges", graph.at("edges"))},
                   {"seq_ext", result.at("seq_ext")}};
    append_json(output["ops"], result.at("new_ops"));
    append_json(output["tensors"], result.at("new_tensors"));
    return output;
}
Json simulate_occupancy(const Json &graph) {
    auto views = build_graph_views(graph.at("ops"), graph.at("edges"));
    auto tensors = index_records(graph.at("tensors"));
    auto seq = graph.at("seq_ext").get<Ids>();
    std::map<Id, Id> count;
    for (auto [tid, users] : views.consumers) {
        count[tid] = Id(users.size());
    }
    IdSet active;
    Capacity used = {{"L1", 0}, {"UB", 0}};
    Json events = array();
    for (std::size_t i = 0; i < seq.size(); ++i) {
        events.push_back(array());
    }
    auto alloc = [&](Id tid, std::size_t step, const char *kind) {
        if (!tensors.count(tid)) {
            return;
        }
        auto &t = tensors.at(tid);
        auto pos = field_string(t, "pos");
        if (!used.count(pos) || active.count(tid)) {
            return;
        }
        active.insert(tid);
        used[pos] += field_id(t, "size");
        if (!seq.empty()) {
            events[step].push_back({tid, pos, t.at("size"), kind});
        }
    };
    auto release = [&](Id tid, std::size_t step) {
        if (!active.erase(tid)) {
            return;
        }
        auto &t = tensors.at(tid);
        auto pos = field_string(t, "pos");
        used[pos] -= field_id(t, "size");
        events[step].push_back({tid, pos, t.at("size"), "free"});
    };
    for (auto [tid, n] : count) {
        if (n && views.producers[tid].empty()) {
            alloc(tid, 0, "initial_alloc");
        }
    }
    Ids l1a, uba, l1, ub;
    for (std::size_t i = 0; i < seq.size(); ++i) {
        Id op = seq[i];
        for (Id tid : as_set(views.outputs[op])) {
            alloc(tid, i, "alloc");
        }
        l1a.push_back(used["L1"]);
        uba.push_back(used["UB"]);
        for (Id tid : as_set(views.inputs[op])) {
            if (count.count(tid) && --count[tid] == 0) {
                release(tid, i);
            }
        }
        for (Id tid : as_set(views.outputs[op])) {
            if (count[tid] == 0) {
                release(tid, i);
            }
        }
        l1.push_back(used["L1"]);
        ub.push_back(used["UB"]);
    }
    Id lp = l1a.empty() ? 0 : *std::max_element(l1a.begin(), l1a.end()),
       up = uba.empty() ? 0 : *std::max_element(uba.begin(), uba.end());
    return Json::array({l1a, uba, l1, ub, events, lp,
                        lp ? Id(std::find(l1a.begin(), l1a.end(), lp) - l1a.begin()) : -1, up,
                        up ? Id(std::find(uba.begin(), uba.end(), up) - uba.begin()) : -1});
}
} // namespace problem2
