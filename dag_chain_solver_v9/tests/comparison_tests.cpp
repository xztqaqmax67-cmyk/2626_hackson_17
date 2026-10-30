#include "solution_key.hpp"
#include <type_traits>

namespace {
struct RowCase {
    double primary = 0, movement = 0;
    int budget = 0;
    Solution solution;
};

auto legacy_key(const RowCase &row) {
    return make_tuple(row.primary, row.movement, row.solution.core, row.solution.order);
}
auto legacy_budget_key(const RowCase &row) {
    return make_tuple(row.primary, row.movement, row.budget, row.solution.core, row.solution.order);
}
auto borrowed_key(const RowCase &row) {
    return dag_chain::detail::solution_key(row.primary, row.movement, row.solution);
}
auto borrowed_budget_key(const RowCase &row) {
    return dag_chain::detail::solution_key(row.primary, row.movement, row.budget, row.solution);
}
void require(bool value, const char *message) {
    if (!value) throw runtime_error(message);
}
} // namespace

int main() {
    try {
        using Key = decltype(borrowed_key(declval<const RowCase &>()));
        using BudgetKey = decltype(borrowed_budget_key(declval<const RowCase &>()));
        static_assert(is_same<tuple_element_t<0, Key>, double>::value, "scalar must be owned");
        static_assert(is_same<tuple_element_t<2, Key>, const vector<int> &>::value,
                      "core assignment must be borrowed without copying");
        static_assert(is_same<tuple_element_t<3, Key>, const vector<int> &>::value,
                      "operation order must be borrowed without copying");
        static_assert(is_same<tuple_element_t<3, BudgetKey>, const vector<int> &>::value,
                      "budget key must also borrow the assignment");

        // Include exact ties, empty/prefix vectors, varied budgets, fractional
        // values, signed zero and infinity. Every old tie-break must survive.
        const vector<double> values{-1.0, -0.0, 0.0, 0.25, 1.0, 1.0, INF,
                                    numeric_limits<double>::infinity()};
        const vector<vector<int>> vectors{{}, {0}, {0, 0}, {0, 1}, {1, 0}, {1}};
        vector<RowCase> rows;
        for (size_t i = 0; i < 168; ++i) {
            RowCase row;
            row.primary = values[(i / 4) % values.size()];
            row.movement = values[(i / 7) % values.size()];
            row.budget = int(i % 4) + 2;
            row.solution.core = vectors[(i / 3) % vectors.size()];
            row.solution.order = vectors[(i / 5) % vectors.size()];
            rows.push_back(move(row));
        }
        rows.push_back(rows[12]);
        rows.push_back(rows[12]);
        for (const auto &a : rows)
            for (const auto &b : rows) {
                require((legacy_key(a) < legacy_key(b)) == (borrowed_key(a) < borrowed_key(b)),
                        "reference key changed the old comparator");
                require((legacy_budget_key(a) < legacy_budget_key(b)) ==
                            (borrowed_budget_key(a) < borrowed_budget_key(b)),
                        "reference budget key changed the old comparator");
            }
        vector<size_t> old_order(rows.size()), new_order(rows.size());
        for (bool include_budget : {false, true}) {
            iota(old_order.begin(), old_order.end(), size_t(0));
            new_order = old_order;
            stable_sort(old_order.begin(), old_order.end(), [&](size_t a, size_t b) {
                return include_budget ? legacy_budget_key(rows[a]) < legacy_budget_key(rows[b])
                                      : legacy_key(rows[a]) < legacy_key(rows[b]);
            });
            stable_sort(new_order.begin(), new_order.end(), [&](size_t a, size_t b) {
                return include_budget ? borrowed_budget_key(rows[a]) < borrowed_budget_key(rows[b])
                                      : borrowed_key(rows[a]) < borrowed_key(rows[b]);
            });
            require(old_order == new_order, "stable sorting or complete-tie order changed");
        }
        Solution large;
        large.core.resize(10000, 2);
        large.order.resize(10000);
        iota(large.order.begin(), large.order.end(), 0);
        const auto key = dag_chain::detail::solution_key(1.0 + 2.0, 4.0 + 5.0, large);
        require(get<0>(key) == 3 && get<1>(key) == 9, "temporary scalar values were not owned");
        require(get<2>(key).data() == large.core.data() && get<3>(key).data() == large.order.data(),
                "comparison key copied a large solution vector");
        cout << "comparison keys preserve scalar/vector tie-breaks and stable order without vector copies\n";
        return 0;
    } catch (const exception &error) {
        cerr << error.what() << '\n';
        return 1;
    }
}
