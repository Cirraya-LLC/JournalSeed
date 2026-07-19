#include "journalseed/application/formula_graph.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

using namespace journalseed::application;

TEST_CASE("formula graph emits dependencies before dependants") {
    const std::vector nodes{
        FormulaNode{.id = 30, .dependencies = {20, 10}},
        FormulaNode{.id = 20, .dependencies = {10}},
        FormulaNode{.id = 10, .dependencies = {}},
    };
    const auto order = topological_formula_order(nodes);
    REQUIRE(order);
    REQUIRE(*order == std::vector<ColumnId>{10, 20, 30});
}

TEST_CASE("formula graph reports a concrete cycle path") {
    const std::vector nodes{
        FormulaNode{.id = 1, .dependencies = {2}},
        FormulaNode{.id = 2, .dependencies = {3}},
        FormulaNode{.id = 3, .dependencies = {1}},
    };
    const auto order = topological_formula_order(nodes);
    REQUIRE_FALSE(order);
    REQUIRE(order.error().cycle.front() == order.error().cycle.back());
    REQUIRE(order.error().cycle.size() == 4);
}
