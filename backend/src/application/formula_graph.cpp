#include "journalseed/application/formula_graph.h"

#include <algorithm>
#include <functional>
#include <unordered_map>

namespace journalseed::application {

std::expected<std::vector<ColumnId>, FormulaGraphError>
topological_formula_order(std::span<const FormulaNode> nodes) {
    enum class Mark : std::uint8_t { unseen, visiting, visited };

    std::unordered_map<ColumnId, const FormulaNode *> by_id;
    std::unordered_map<ColumnId, Mark> marks;
    by_id.reserve(nodes.size());
    marks.reserve(nodes.size());
    for (const auto &node : nodes) {
        by_id.emplace(node.id, &node);
        marks.emplace(node.id, Mark::unseen);
    }

    std::vector<ColumnId> order;
    std::vector<ColumnId> stack;
    order.reserve(nodes.size());
    stack.reserve(nodes.size());

    std::function<std::expected<void, FormulaGraphError>(ColumnId)> visit;
    visit = [&](ColumnId id) -> std::expected<void, FormulaGraphError> {
        auto node_iterator = by_id.find(id);
        if (node_iterator == by_id.end()) {
            return {};
        }

        auto &mark = marks[id];
        if (mark == Mark::visited) {
            return {};
        }
        if (mark == Mark::visiting) {
            const auto cycle_start = std::find(stack.begin(), stack.end(), id);
            std::vector<ColumnId> cycle(cycle_start, stack.end());
            cycle.push_back(id);
            return std::unexpected(FormulaGraphError{
                .message = "公式列依赖形成循环",
                .cycle = std::move(cycle),
            });
        }

        mark = Mark::visiting;
        stack.push_back(id);

        auto dependencies = node_iterator->second->dependencies;
        std::ranges::sort(dependencies);
        for (const auto dependency : dependencies) {
            auto result = visit(dependency);
            if (!result) {
                return result;
            }
        }

        stack.pop_back();
        mark = Mark::visited;
        order.push_back(id);
        return {};
    };

    std::vector<ColumnId> sorted_ids;
    sorted_ids.reserve(nodes.size());
    for (const auto &node : nodes) {
        sorted_ids.push_back(node.id);
    }
    std::ranges::sort(sorted_ids);

    for (const auto id : sorted_ids) {
        auto result = visit(id);
        if (!result) {
            return std::unexpected(std::move(result.error()));
        }
    }
    return order;
}

}  // namespace journalseed::application
