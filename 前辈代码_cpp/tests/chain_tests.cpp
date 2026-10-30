#include "chain.hpp"
#include <iostream>

using namespace problem2;
namespace {
void require(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}
GraphInfo graph(const Ids &weights, const std::vector<std::pair<Id, Id>> &edges) {
    Json ops = array(), links = array();
    for (std::size_t i = 0; i < weights.size(); ++i)
        ops.push_back({{"id", Id(i)}, {"op", "COMPUTE"}, {"pipe", "PIPE_M"},
                       {"cycles", weights[i]}});
    for (auto [a, b] : edges) links.push_back({{"source", a}, {"target", b}});
    return GraphInfo({{"ops", ops}, {"tensors", array()}, {"edges", links}});
}
void partition_invariants(const GraphInfo &g, const ChainDecomposition &d) {
    std::map<Id, int> paths, segments;
    for (const auto &path : d.paths) {
        require(!path.empty(), "empty chain");
        for (std::size_t i = 0; i < path.size(); ++i) {
            ++paths[path[i]];
            if (i) require(g.succs.at(path[i - 1]).count(path[i]), "nonexistent chain edge");
        }
    }
    for (std::size_t b = 0; b < d.segments.size(); ++b)
        for (Id v : d.segments[b]) {
            ++segments[v];
            require(d.segment_of.at(v) == Id(b), "segment index differs from coverage");
        }
    require(paths.size() == g.ids.size() && segments.size() == g.ids.size(), "incomplete cover");
    for (Id v : g.ids) require(paths[v] == 1 && segments[v] == 1, "duplicate op in cover");
}
} // namespace
int main() {
    try {
        const auto cfg = submission_settings();
        // 较慢的累积前驱优先；最末单个算子的 cycles 不能替代路径耗时。
        const auto cumulative = graph({100, 10, 200, 1}, {{2, 1}, {0, 3}, {1, 3}});
        const auto c = chain_decompose(cumulative, 2, cfg, 1);
        partition_invariants(cumulative, c);
        bool found = false;
        for (const auto &path : c.paths)
            for (std::size_t i = 1; i < path.size(); ++i)
                found |= path[i - 1] == 1 && path[i] == 3;
        require(found, "critical parent does not use cumulative work");
        // 两个源和两个汇完全交叉；路径合法不保证缩成两个块之后仍无环。
        const auto crossing = graph({40, 30, 20, 10}, {{0, 2}, {0, 3}, {1, 2}, {1, 3}});
        for (Id cores : {2, 5}) {
            for (bool complete : {false, true}) {
                const auto d = chain_decompose(crossing, cores, cfg, .25, .25, complete);
                partition_invariants(crossing, d);
            }
            auto a = chain_seed_plans(crossing, cores, cfg, 0, 0);
            auto b = chain_seed_plans(crossing, cores, cfg, 0, 0);
            require(!a.empty() && a.size() == b.size(), "no deterministic seed pool");
            std::set<std::string> fingerprints;
            for (std::size_t i = 0; i < a.size(); ++i) {
                require(a[i].first.rfind("chain_", 0) == 0, "non-chain seed mixed into pool");
                a[i].second.validate(crossing);
                require(a[i].second.to_dict() == b[i].second.to_dict(), "non-deterministic plan");
                require(fingerprints.insert(a[i].second.fingerprint()).second, "duplicate output");
                for (const auto &[id, nodes] : a[i].second.blocks) {
                    (void)id;
                    require(nodes.size() == 1, "chain contraction incorrectly became a block");
                }
            }
        }
        // 即使写入者分属不同依赖链，也必须共同分核。
        const GraphInfo partial(Json::parse(R"({"tensors":[{"id":100,"pos":"UB","size":4096}],"ops":[{"id":0,"op":"M","pipe":"PIPE_M","cycles":20},{"id":1,"op":"M","pipe":"PIPE_M","cycles":30},{"id":2,"op":"V","pipe":"PIPE_V","cycles":10}],"edges":[{"source":0,"target":100},{"source":1,"target":100},{"source":100,"target":2}]})"));
        const auto d = chain_decompose(partial, 4, cfg, .25);
        require(d.segment_of.at(0) == d.segment_of.at(1), "partial writers are split");
        for (const auto &[name, plan] : chain_seed_plans(partial, 4, cfg, 0, 0)) {
            (void)name;
            require(plan.core_of.at(0) == plan.core_of.at(1), "writer affinity lost after placement");
        }
        // 0 与 3 分属两条链且共同写入，合并分核段后有 A(0)->B(1)->A(3)。
        // 分核段的商图有环，但原始算子 DAG 无环，不能把段直接当成调度子图。
        const GraphInfo writer_crossing(Json::parse(R"({"tensors":[{"id":100,"pos":"UB","size":4096}],"ops":[{"id":0,"op":"M","pipe":"PIPE_M","cycles":10},{"id":1,"op":"M","pipe":"PIPE_M","cycles":10},{"id":2,"op":"M","pipe":"PIPE_M","cycles":200},{"id":3,"op":"M","pipe":"PIPE_M","cycles":10},{"id":4,"op":"V","pipe":"PIPE_V","cycles":10}],"edges":[{"source":0,"target":1},{"source":1,"target":3},{"source":2,"target":3},{"source":0,"target":100},{"source":3,"target":100},{"source":100,"target":4}]})"));
        const auto wc = chain_decompose(writer_crossing, 4, cfg, .25);
        partition_invariants(writer_crossing, wc);
        std::map<Id, std::size_t> chain_of;
        for (std::size_t i = 0; i < wc.paths.size(); ++i)
            for (Id v : wc.paths[i]) chain_of[v] = i;
        require(chain_of.at(0) != chain_of.at(3), "writer fixture did not span chains");
        require(wc.segment_of.at(0) == wc.segment_of.at(3), "cross-chain writer closure lost");
        require(wc.segment_of.at(0) != wc.segment_of.at(1), "cyclic quotient fixture collapsed");
        const auto writer_seeds = chain_seed_plans(writer_crossing, 4, cfg, 0, 0);
        require(!writer_seeds.empty(), "writer closure removed all seeds");
        for (const auto &[name, plan] : writer_seeds) {
            (void)name;
            plan.validate(writer_crossing);
            require(plan.blocks.size() == writer_crossing.ids.size(), "writer segment became a block");
            require(plan.core_of.at(0) == plan.core_of.at(3), "cross-chain writers use different cores");
            std::map<Id, std::size_t> pos;
            for (std::size_t i = 0; i < plan.order.size(); ++i) pos[plan.order[i]] = i;
            require(pos.at(0) < pos.at(1) && pos.at(1) < pos.at(3) &&
                        pos.at(2) < pos.at(3) && pos.at(3) < pos.at(4),
                    "writer closure lost an original dependency");
        }
        const auto line = graph({20, 20, 20, 20, 20, 20}, {{0,1},{1,2},{2,3},{3,4},{4,5}});
        const auto l = chain_decompose(line, 2, cfg, .25);
        require(l.paths.size() == 1 && l.segments.size() > 1, "heavy chain cannot be split");
        const auto empty = graph({}, {});
        const auto e = chain_seed_plans(empty, 4, cfg);
        require(e.size() == 1 && e.front().second.blocks.empty(), "empty graph seed");
        bool expired = false;
        try { chain_seed_plans(crossing, 4, cfg, monotonic() - 1); }
        catch (const TimeoutError &) { expired = true; }
        require(expired, "chain construction ignores deadline");
        // deadline=0 只取消调用者局部截止，不能取消搜索线程继承的总截止。
        const double saved_deadline = execution_deadline;
        execution_deadline = monotonic() - 1;
        bool inherited_expired = false;
        try { chain_seed_plans(crossing, 4, cfg, 0); }
        catch (const TimeoutError &) { inherited_expired = true; }
        catch (...) { execution_deadline = saved_deadline; throw; }
        execution_deadline = saved_deadline;
        require(inherited_expired, "explicit zero bypasses inherited deadline");
        std::cout << "chain coverage, dependency preservation, writer affinity, deterministic seeds and deadline: passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
