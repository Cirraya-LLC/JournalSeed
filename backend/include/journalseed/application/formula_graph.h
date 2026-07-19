#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace journalseed::application {

using ColumnId = std::int64_t;

struct FormulaNode {
    ColumnId id;
    std::vector<ColumnId> dependencies;
};

struct FormulaGraphError {
    std::string message;
    std::vector<ColumnId> cycle;
};

[[nodiscard]] std::expected<std::vector<ColumnId>, FormulaGraphError>
topological_formula_order(std::span<const FormulaNode> nodes);

}  // namespace journalseed::application
