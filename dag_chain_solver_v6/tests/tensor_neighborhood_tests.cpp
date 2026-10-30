#include "tensor_neighborhood.hpp"

namespace {
void require(bool condition, const string &message) {
    if (!condition) throw runtime_error(message);
}
Graph fixture() {
    const string text = R"({"ops":[
        {"id":0,"op":"producer","pipe":"PIPE_M","cycles":5},
        {"id":1,"op":"consumer_a","pipe":"PIPE_V","cycles":20},
        {"id":2,"op":"consumer_b","pipe":"PIPE_V","cycles":30},
        {"id":3,"op":"bound_writer","pipe":"PIPE_V","cycles":10},
        {"id":4,"op":"balanced_compensator","pipe":"PIPE_V","cycles":60},
        {"id":5,"op":"wrong_pipe_compensator","pipe":"PIPE_M","cycles":60},
        {"id":6,"op":"producer_bound_consumer","pipe":"PIPE_V","cycles":1}],
        "tensors":[{"id":100,"pos":"UB","size":100},
                   {"id":101,"pos":"UB","size":1},
                   {"id":102,"pos":"UB","size":1}],
        "edges":[{"source":0,"target":100},{"source":100,"target":1},
                 {"source":100,"target":2},{"source":100,"target":6},
                 {"source":1,"target":101},{"source":3,"target":101},
                 {"source":0,"target":102},{"source":6,"target":102}]})";
    return Graph(JsonParser(text).parse());
}
}
int main() {
    try {
        auto g = fixture();
        Solution base;
        base.core = {0, 1, 1, 1, 0, 0, 0};
        base.order = g.topo;
        validate(g, base, 2);
        const string before = solution_json(g, base, 2);
        const auto original_edges = g.ops[0].succ;
        const auto original_writers = g.writer_group;
        dag_chain::TensorNeighborhoods space(g);
        require(space.count(2) == 12, "fixture must expose exactly one multi-consumer tensor");
        Solution plain, swapped, alternate;
        // tensor 0, source 1, target 0, then variants 0/1/2.
        require(space.propose(g, base, 2, 6, nullptr, plain), "plain consumer move missing");
        require(space.propose(g, base, 2, 7, nullptr, swapped), "balanced exchange missing");
        require(space.propose(g, base, 2, 8, nullptr, alternate), "second exchange missing");
        for (const auto *s : {&plain, &swapped, &alternate}) {
            validate(g, *s, 2);
            require(s->core[0] == 0 && s->core[6] == 0, "producer writer closure moved");
            require(s->core[1] == 0 && s->core[2] == 0 && s->core[3] == 0,
                    "consumer group must include its non-consumer bound writer");
            require(s->order == base.order, "migration changed the topological order");
        }
        require(swapped.core[4] == 1 && swapped.core[5] == 0,
                "compensation must prefer matching pipeline load over equal total work");
        require(alternate.core[5] == 1 && alternate.core[4] == 0,
                "second compensation candidate missing");
        const Config cfg;
        TrafficState old_traffic(g, cfg, 2, base.core), new_traffic(g, cfg, 2, plain.core);
        require(new_traffic.bytes < old_traffic.bytes, "consumer co-location should eliminate the tensor route");
        Solution repeat;
        require(space.propose(g, base, 2, 7, nullptr, repeat) && repeat.core == swapped.core,
                "neighborhood is not deterministic");
        for (uint64_t index = 0; index < space.count(2); ++index) {
            Solution s;
            if (space.propose(g, base, 2, index, nullptr, s)) validate(g, s, 2);
        }
        Solution all_on_zero = base;
        fill(all_on_zero.core.begin(), all_on_zero.core.end(), 0);
        require(!space.propose(g, all_on_zero, 2, 6, nullptr, repeat),
                "consumer membership was cached across parent assignments");
        require(!space.propose(g, base, 2, space.count(2), nullptr, repeat), "out-of-range slot accepted");
        require(!space.propose(g, base, 2, 0, nullptr, repeat), "same-core no-op accepted");
        require(space.count(5) <= 24U * 5U * 5U * 3U, "candidate count is unbounded");
        Deadline expired;
        expired.start = Clock::now() - chrono::seconds(2); expired.limit = 1;
        Solution sentinel = base;
        bool cancelled = false;
        try { space.propose(g, base, 2, 6, &expired, sentinel); }
        catch (const SearchExpired &) { cancelled = true; }
        require(cancelled && sentinel.core == base.core && sentinel.order == base.order,
                "expired candidate was published or cancellation ignored");
        require(solution_json(g, base, 2) == before && g.ops[0].succ == original_edges &&
                    g.writer_group == original_writers, "search modified parent or input graph");
        cout << "tensor neighborhood invariants passed\n";
        return 0;
    } catch (const exception &e) {
        cerr << e.what() << '\n';
        return 1;
    }
}
