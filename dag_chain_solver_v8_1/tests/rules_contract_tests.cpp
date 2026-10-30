#include "chain.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
template <class Work>
void rejects(Work work, const std::string &message) {
    try {
        work();
    } catch (const std::runtime_error &error) {
        if (std::string(error.what()).find(message) == std::string::npos)
            throw std::runtime_error("unexpected rejection: " + std::string(error.what()));
        return;
    }
    throw std::runtime_error("expected rejection containing: " + message);
}
} // namespace

int main() {
    try {
        // Sharing two tensors joins three writers transitively. COPY_IN is
        // excluded from the solution, even when it also writes the first tensor.
        const std::string input = R"({
            "tensors":[{"id":10,"pos":"UB","size":16},
                       {"id":11,"pos":"UB","size":16}],
            "ops":[{"id":0,"op":"CUSTOM_A","pipe":"PIPE_M","cycles":1},
                   {"id":1,"op":"CUSTOM_B","pipe":"PIPE_FIX","cycles":1},
                   {"id":2,"op":"CUSTOM_C","pipe":"PIPE_V","cycles":1},
                   {"id":3,"op":"COPY_IN","pipe":"PIPE_MTE2","cycles":-1}],
            "edges":[{"source":0,"target":10},{"source":1,"target":10},
                     {"source":1,"target":11},{"source":2,"target":11},
                     {"source":3,"target":10}]
        })";
        Graph graph(JsonParser(input).parse());
        Solution same;
        same.core = {1, 1, 1};
        same.order = {0, 1, 2};
        validate(graph, same, 2);

        Solution split = same;
        split.core[0] = 0;
        rejects([&] { validate(graph, split, 2); }, "tensor 10 has writers on multiple cores");
        split = same;
        split.core[2] = 0;
        rejects([&] { validate(graph, split, 2); }, "tensor 11 has writers on multiple cores");

        Solution malformed = same;
        malformed.core.pop_back();
        rejects([&] { validate(graph, malformed, 2); }, "incomplete solution");
        malformed = same;
        malformed.order.pop_back();
        rejects([&] { validate(graph, malformed, 2); }, "incomplete solution");
        malformed = same;
        malformed.core[2] = 2;
        rejects([&] { validate(graph, malformed, 2); }, "bad core");
        malformed = same;
        malformed.order[2] = 1;
        rejects([&] { validate(graph, malformed, 2); }, "repeated op");

        std::cout << "rules contract validation checks passed; no evaluator invoked\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

