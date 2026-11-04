#include "graphcanvaslayertool.hpp"

#include <algorithm>
#include <utility>

namespace PelicanStudio {
namespace {

std::string addedLayer(const nlohmann::json &ops, std::string_view stack,
                       const std::vector<GraphCanvasStackRow> &before) {
    for (const auto &op : ops)
        if (op.is_object() && op.value("op", std::string{}) == "set_node_parameters" &&
            op.value("node", std::string{}) == stack)
            for (const auto &element : op["parameters"].value("layers", nlohmann::json::array()))
                if (const auto input = element.value("input", std::string{});
                    !input.empty() && std::ranges::find(before, input, &GraphCanvasStackRow::input) == before.end())
                    return input;
    return {};
}

} // namespace

LayerStackTool::LayerStackTool(std::vector<GraphCanvasNode> nodes, std::string scope, std::string stack,
                               std::string picked_input)
    : nodes_(std::move(nodes)), scope_(std::move(scope)), stack_(std::move(stack)),
      picked_input_(std::move(picked_input)) {}

std::optional<std::size_t> LayerStackTool::above() const {
    if (picked_input_.empty()) return std::nullopt;
    for (const auto &row : graphCanvasLayerRows(nodes_, stack_))
        if (row.input == picked_input_) return row.index;
    return std::nullopt;
}

nlohmann::json LayerStackTool::addOps(std::string_view what) const { return addOpsAt(what, above()); }

nlohmann::json LayerStackTool::addOpsAt(std::string_view what, std::optional<std::size_t> at) const {
    if (what == "rect" || what == "ellipse")
        return graphCanvasAddShapeLayerOps(nodes_, scope_, stack_, what, at);
    if (what == "group") return graphCanvasAddGroupLayerOps(nodes_, scope_, stack_, at);
    if (what.starts_with("adjustment:"))
        return graphCanvasAddAdjustmentLayerOps(nodes_, scope_, stack_, at, what.substr(11));
    return nlohmann::json::array();
}

std::string LayerStackTool::addedBy(const nlohmann::json &ops) const {
    return addedLayer(ops, stack_, graphCanvasLayerRows(nodes_, stack_));
}

} // namespace PelicanStudio
