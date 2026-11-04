#pragma once

// SDK-1b: the + menu of a layer stack, as one tool. It reads the same
// snapshot both callers draw from -- the scene's rows (which may hold the
// shape the gesture just left) and the dock's model rows -- so the place a
// layer goes and the layer picked next agree with what was on screen.

#include "graphcanvaslayerstack.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace PelicanStudio {

class LayerStackTool {
  public:
    // `nodes` is the scope as the caller sees it; `picked_input` is the row
    // that was picked in `stack`, empty when none was.
    LayerStackTool(std::vector<GraphCanvasNode> nodes, std::string scope, std::string stack,
                   std::string picked_input);

    // The index in `layers` a new layer goes above: the picked row's, or
    // nothing when no row is picked (or it is gone) -- on top.
    [[nodiscard]] std::optional<std::size_t> above() const;

    // `what`: "rect", "ellipse", "group" or "adjustment:<op>". Text and
    // image stay dialogs. Empty when the stack or the op refuses.
    [[nodiscard]] nlohmann::json addOps(std::string_view what) const;
    // The same at an explicit place: `at` is the row to go above, nullopt on
    // top. Callers that already know the place (a drop) use this.
    [[nodiscard]] nlohmann::json addOpsAt(std::string_view what, std::optional<std::size_t> at) const;

    // The input of the layer `ops` adds to this stack, or empty: the new
    // `layers` entry the snapshot did not have (the dock's rule).
    [[nodiscard]] std::string addedBy(const nlohmann::json &ops) const;

  private:
    std::vector<GraphCanvasNode> nodes_;
    std::string scope_;
    std::string stack_;
    std::string picked_input_;
};

} // namespace PelicanStudio
