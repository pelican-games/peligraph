#pragma once

#include <cstdint>

namespace Pelican {

// LC-1: the graph.loop element a pass or compute task is a copy of, and how
// many elements the loop expanded (pelican_loop.glsl). Pushed at
// PELICAN_PUSH_LOOP_OFFSET to every stage whose push-constant range holds it.
// A pass no loop expanded is element 0 of 1.
struct PassLoopIndex {
    std::uint32_t element = 0;
    std::uint32_t count = 1;

    bool operator==(const PassLoopIndex &) const = default;
};

} // namespace Pelican
