#ifndef PELICAN_LOOP_GLSL
#define PELICAN_LOOP_GLSL

#include "pelican_sets.glsl"

// LC-1: which element of a graph.loop this pass or compute task is.
//
//   uint pelican_loop_element()  the element that expanded this copy (0-based)
//   uint pelican_loop_count()    how many elements the loop expanded
//
// The engine pushes {element, count} as two uints at PELICAN_PUSH_LOOP_OFFSET
// (112) to every stage of a fullscreen / raster pass or a compute task whose
// push-constant range holds [112, 120). A pass that no loop expanded gets
// element 0 and count 1. The copies of one loop share one compiled shader:
// the element is a value, not a define.
//
// By default this include declares the push-constant block itself. A stage
// has only one push-constant block, so a shader with push constants of its
// own defines PELICAN_LOOP_CUSTOM_PUSH before the include, puts
// PELICAN_LOOP_PUSH_FIELDS last in its block, and names the block's instance
// to PELICAN_LOOP_FUNCTIONS after it:
//
//   #define PELICAN_LOOP_CUSTOM_PUSH
//   #include "pelican_loop.glsl"
//   layout(push_constant) uniform Push {
//       vec4 my_tint;
//       PELICAN_LOOP_PUSH_FIELDS
//   } push;
//   PELICAN_LOOP_FUNCTIONS(push)
//
// A push-constant range that covers only part of [112, 120) is refused when
// the pipeline is built (shader.loop_push_overlap@1).

#define PELICAN_LOOP_PUSH_FIELDS                                              \
    layout(offset = PELICAN_PUSH_LOOP_OFFSET) uint pelican_loop_element_value; \
    uint pelican_loop_count_value;

#define PELICAN_LOOP_FUNCTIONS(instance)                                       \
    uint pelican_loop_element() { return instance.pelican_loop_element_value; } \
    uint pelican_loop_count() { return instance.pelican_loop_count_value; }

#ifndef PELICAN_LOOP_CUSTOM_PUSH
layout(push_constant) uniform PelicanLoopPush {
    layout(offset = PELICAN_PUSH_LOOP_OFFSET) uint element;
    uint count;
} pelican_loop_push;

uint pelican_loop_element() { return pelican_loop_push.element; }
uint pelican_loop_count() { return pelican_loop_push.count; }
#endif

#endif
