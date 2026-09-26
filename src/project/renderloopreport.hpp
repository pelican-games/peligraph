#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Pelican {

// LP-1a: where an expanded pass, compute task, render target or buffer came
// from. `loop` is the graph.loop id, `element` the element that expanded it,
// `node` the node id inside the branch body's definition. The expanded name
// is `<loop>/<element>/<node>`; Studio reads these fields instead of taking
// the name apart.
struct RenderLoopOrigin {
    std::string loop;
    std::int64_t element = 0;
    std::string node;

    bool operator==(const RenderLoopOrigin &) const = default;
};

struct RenderGraphLoopElementReport {
    std::int64_t element = 0;
    std::size_t index = 0;
    std::size_t branch = 0;

    bool operator==(const RenderGraphLoopElementReport &) const = default;
};

// get_frame_plan's `loops[]`: one entry per graph.loop of the composed graph,
// a loop that expanded nothing included (count 0, no elements).
struct RenderGraphLoopReport {
    std::string node;
    std::string tier;
    std::size_t count = 0;
    std::vector<RenderGraphLoopElementReport> elements;
    // Branches no element took, by 0-based index.
    std::vector<std::size_t> unmatched_branches;

    bool operator==(const RenderGraphLoopReport &) const = default;
};

// LP-0d: a graph node a graph variant made inactive because it depends on a
// feature the variant excluded. `reason` is one of
//   target_ref        -- it reads or writes a target of that feature through a
//                        render.target_ref,
//   insert            -- its insert names a pass of it, or a graph pass made
//                        inactive,
//   order             -- its after / before names a pass of it,
//   pass_replacement  -- it replaces a pass of it (pass_replacements),
//   loop_output       -- it reads an output of (or carries from) a loop that
//                        was made inactive; `feature` is that loop's.
struct RenderGraphDeactivatedNode {
    std::string node;
    std::string feature;
    std::string reason;

    bool operator==(const RenderGraphDeactivatedNode &) const = default;
};

} // namespace Pelican
