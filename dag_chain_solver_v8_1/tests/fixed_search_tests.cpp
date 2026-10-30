#include "anytime.hpp"
#include "search_limits.hpp"
#include "tensor_neighborhood.hpp"

namespace {
void require(bool condition, const string &message) {
    if (!condition) throw runtime_error(message);
}

// 独立合成图：三条并行链、分叉汇合、跨层跳边，以及共享写者。
// 每条依赖均经 Tensor 表达，不引用公开用例或预计算方案。
Graph fixture() {
    const vector<int> cycles{1700, 27000, 900, 12000, 4000, 600,
                             8000, 2000, 500, 3000, 1000, 7000};
    const vector<pair<int, int>> links{
        {0, 3}, {1, 4}, {2, 5}, {3, 6}, {4, 7}, {5, 8},
        {6, 9}, {7, 9}, {8, 10}, {9, 11}, {10, 11}, {0, 10}};
    ostringstream text;
    text << "{\"ops\":[";
    for (size_t i = 0; i < cycles.size(); ++i) {
        if (i) text << ',';
        text << "{\"id\":" << i << ",\"op\":\"WORK\",\"pipe\":\""
             << (i % 3 == 0 ? "PIPE_V" : "PIPE_M") << "\",\"cycles\":" << cycles[i] << '}';
    }
    text << "],\"tensors\":[";
    for (size_t i = 0; i < links.size(); ++i) {
        if (i) text << ',';
        text << "{\"id\":" << 100 + i << ",\"pos\":\"UB\",\"size\":256}";
    }
    text << ",{\"id\":200,\"pos\":\"UB\",\"size\":1024}],\"edges\":[";
    bool first = true;
    auto add = [&](int a, int b) {
        if (!first) text << ',';
        first = false;
        text << "{\"source\":" << a << ",\"target\":" << b << '}';
    };
    for (size_t i = 0; i < links.size(); ++i) {
        add(links[i].first, int(100 + i));
        add(int(100 + i), links[i].second);
    }
    add(0, 200); add(2, 200); add(200, 9); add(200, 10);
    text << "]}";
    return Graph(JsonParser(text.str()).parse());
}

string fingerprint(const Graph &g) {
    ostringstream text;
    for (const auto &op : g.ops) {
        text << op.id << ':' << op.cycles << ':' << op.pipe << ':';
        for (const auto *values : {&op.pred, &op.succ, &op.inputs, &op.outputs}) {
            for (int v : *values) text << v << ',';
            text << '|';
        }
    }
    for (const auto &tensor : g.tensors) {
        text << tensor.id << ':' << tensor.pos << ':' << tensor.size << ':' << tensor.final_output;
        for (int v : tensor.producers) text << 'p' << v;
        for (int v : tensor.consumers) text << 'c' << v;
    }
    for (int v : g.writer_group) text << 'w' << v;
    for (int v : g.topo) text << 't' << v;
    return text.str();
}

auto merit(const dag_chain::Result &result) {
    return make_pair(result.refined.cycles, result.refined.route + result.refined.spill);
}

auto work_counts(const dag_chain::TimedSearchStats &s) {
    // 耗时不是确定性契约；候选数量、精评次数和终止原因必须相同。
    return make_tuple(s.attempted, s.unique_candidates, s.refined, s.improvements,
                      s.cancelled, s.workers, s.exhausted_lanes, s.cache_limited_lanes,
                      s.deadline_reached);
}

void complete_and_valid(const Graph &g, const dag_chain::Result &result, int cores) {
    validate(g, result.solution, cores);
    require(result.refined.valid, "returned solution lacks a completed refinement");
    const auto recomputed = dag_chain::refine_estimate(g, result.solution, cores);
    require(recomputed.valid && recomputed.cycles == result.refined.cycles &&
                recomputed.route == result.refined.route && recomputed.spill == result.refined.spill,
            "returned refinement differs from an independent full replay");
    for (int v = 0; v < g.size(); ++v)
        require(result.solution.core[v] == result.solution.core[g.writer_group[v]],
                "search split a shared writer group");
}
} // namespace

int main() {
    try {
        const auto g = fixture();
        const auto before = fingerprint(g);
        size_t total_attempted = 0, total_refined = 0;
        for (int cores = 2; cores <= 5; ++cores) {
            vector<dag_chain::Result> portfolio;
            const auto base = dag_chain::solve(g, cores, false, 7, &portfolio);
            const auto without_portfolio = dag_chain::solve(g, cores);
            require(solution_json(g, base.solution, cores) ==
                        solution_json(g, without_portfolio.solution, cores),
                    "exposing the seed portfolio changed the original winner");
            require(portfolio.size() == size_t(cores - 1), "missing feasible core-budget seeds");
            size_t sum_candidates = 0, sum_refined = 0;
            for (size_t i = 0; i < portfolio.size(); ++i) {
                const auto &seed = portfolio[i];
                require(seed.generated_for_cores == int(i + 2), "seed budgets lost ascending order");
                complete_and_valid(g, seed, int(i + 2));
                validate(g, seed.solution, cores);
                sum_candidates += seed.candidates;
                sum_refined += seed.refined_candidates;
            }
            require(base.candidates == sum_candidates && base.refined_candidates == sum_refined,
                    "aggregate seed work no longer includes all independent core budgets");

            dag_chain::TimedSearchStats first_stats, second_stats;
            const auto first = dag_chain::solve_fixed_work(g, cores, &first_stats);
            const auto second = dag_chain::solve_fixed_work(g, cores, &second_stats);
            complete_and_valid(g, base, cores);
            complete_and_valid(g, first, cores);
            complete_and_valid(g, second, cores);
            require(solution_json(g, first.solution, cores) == solution_json(g, second.solution, cores),
                    "fixed work changed output bytes on a repeated identical request");
            require(work_counts(first_stats) == work_counts(second_stats),
                    "fixed work changed its explored candidate counts or termination reason");
            require(merit(first) <= merit(base), "fixed continuation discarded a better initial winner");
            require(first_stats.workers == size_t(dag_chain::search_limits::timed_workers),
                    "unexpected fixed-search worker count");
            require(first_stats.attempted > 0 && first_stats.unique_candidates > 0 && first_stats.refined > 0,
                    "fixture never entered useful continuation search");
            const size_t rounds = dag_chain::search_limits::fixed_rounds_small;
            const size_t coarse_limit = first_stats.workers * rounds *
                                        dag_chain::search_limits::timed_coarse_per_round;
            const size_t refine_limit = first_stats.workers * rounds *
                                        dag_chain::search_limits::timed_refine_per_round;
            require(first_stats.unique_candidates <= coarse_limit, "fixed coarse-evaluation quota exceeded");
            require(first_stats.attempted <= coarse_limit * 8, "fixed candidate-attempt quota exceeded");
            require(first_stats.refined <= refine_limit && first_stats.improvements <= first_stats.refined,
                    "fixed refinement quota exceeded");
            require(!first_stats.deadline_reached && first_stats.cancelled == 0,
                    "fixed search consulted the wall-clock deadline");
            require(first.candidates == base.candidates + first_stats.unique_candidates &&
                        first.refined_candidates == base.refined_candidates + first_stats.refined,
                    "fixed-search work counters omit seed work or continuation work");
            require(fingerprint(g) == before, "search mutated graph dependencies or writer bindings");
            total_attempted += first_stats.attempted;
            total_refined += first_stats.refined;
        }

        // 直接验证新消费者邻域确实能生成合法移动；无需依赖启发式恰好选中它。
        dag_chain::TensorNeighborhoods moves(g);
        Solution spread;
        spread.order = g.topo;
        spread.core.resize(g.size());
        for (int v = 0; v < g.size(); ++v) spread.core[v] = g.writer_group[v] % 3;
        validate(g, spread, 3);
        size_t legal_moves = 0;
        for (uint64_t i = 0; i < moves.count(3); ++i) {
            Solution candidate;
            if (!moves.propose(g, spread, 3, i, nullptr, candidate)) continue;
            validate(g, candidate, 3);
            require(candidate.order == spread.order, "plain consumer move changed topological order");
            require(candidate.core != spread.core, "consumer move published a no-op");
            ++legal_moves;
        }
        require(legal_moves > 0, "consumer-group fixture did not exercise a valid migration");
        cout << "fixed search: 2..5 cores, byte-identical repeats, complete replay, seed budgets, quotas passed; "
             << total_attempted << " attempts, " << total_refined << " refinements, "
             << legal_moves << " legal consumer moves\n";
        return 0;
    } catch (const exception &error) {
        cerr << error.what() << '\n';
        return 1;
    }
}
