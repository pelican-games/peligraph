#pragma once

#include <cstdint>
#include <limits>

namespace Pelican {

enum class GraphicsPipelineViewExecution : std::uint8_t {
    single_view,
    multiview,
};

// Shared by pipeline creation and compiled render-pass execution. Keeping one
// typed contract prevents a view-masked rendering scope from binding a
// single-view pipeline (or the reverse).
struct GraphicsPipelineViewContract {
    GraphicsPipelineViewExecution execution =
        GraphicsPipelineViewExecution::single_view;
    std::uint32_t view_count = 1;
    std::uint32_t view_mask = 0;

    static constexpr GraphicsPipelineViewContract multiview(
        std::uint32_t count) noexcept {
        return GraphicsPipelineViewContract{
            .execution =
                GraphicsPipelineViewExecution::multiview,
            .view_count = count,
            .view_mask =
                count >= 32
                    ? std::numeric_limits<
                          std::uint32_t>::max()
                    : (std::uint32_t{1} << count) - 1u,
        };
    }

    bool operator==(
        const GraphicsPipelineViewContract &) const =
        default;
};

} // namespace Pelican
