#ifndef PELICAN_VIEW_GLSL
#define PELICAN_VIEW_GLSL

#if defined(PELICAN_MULTIVIEW)
#extension GL_EXT_multiview : require
#if !defined(PELICAN_VIEW_COUNT)
#error PELICAN_MULTIVIEW requires PELICAN_VIEW_COUNT
#endif
#if PELICAN_VIEW_COUNT < 2 || PELICAN_VIEW_COUNT > 32
#error PELICAN_VIEW_COUNT must be in [2, 32]
#endif
#define PELICAN_VIEW_INDEX uint(gl_ViewIndex)
#else
#define PELICAN_VIEW_INDEX 0u
#endif

// A multiview pass may mix per-view array resources with shared 2D resources
// (for example a stereo G-buffer plus one shared shadow map). The compiler
// supplies PELICAN_INPUT_<binding>_LAYERED for each physical input.
#ifndef PELICAN_INPUT_0_LAYERED
#define PELICAN_INPUT_0_LAYERED 0
#endif
#ifndef PELICAN_INPUT_1_LAYERED
#define PELICAN_INPUT_1_LAYERED 0
#endif
#ifndef PELICAN_INPUT_2_LAYERED
#define PELICAN_INPUT_2_LAYERED 0
#endif
#ifndef PELICAN_INPUT_3_LAYERED
#define PELICAN_INPUT_3_LAYERED 0
#endif
#ifndef PELICAN_INPUT_4_LAYERED
#define PELICAN_INPUT_4_LAYERED 0
#endif
#ifndef PELICAN_INPUT_5_LAYERED
#define PELICAN_INPUT_5_LAYERED 0
#endif
#ifndef PELICAN_INPUT_6_LAYERED
#define PELICAN_INPUT_6_LAYERED 0
#endif
#ifndef PELICAN_INPUT_7_LAYERED
#define PELICAN_INPUT_7_LAYERED 0
#endif

#ifndef PELICAN_INPUT_0_LOCAL_READ
#define PELICAN_INPUT_0_LOCAL_READ 0
#endif
#ifndef PELICAN_INPUT_1_LOCAL_READ
#define PELICAN_INPUT_1_LOCAL_READ 0
#endif
#ifndef PELICAN_INPUT_2_LOCAL_READ
#define PELICAN_INPUT_2_LOCAL_READ 0
#endif
#ifndef PELICAN_INPUT_3_LOCAL_READ
#define PELICAN_INPUT_3_LOCAL_READ 0
#endif
#ifndef PELICAN_INPUT_4_LOCAL_READ
#define PELICAN_INPUT_4_LOCAL_READ 0
#endif
#ifndef PELICAN_INPUT_5_LOCAL_READ
#define PELICAN_INPUT_5_LOCAL_READ 0
#endif
#ifndef PELICAN_INPUT_6_LOCAL_READ
#define PELICAN_INPUT_6_LOCAL_READ 0
#endif
#ifndef PELICAN_INPUT_7_LOCAL_READ
#define PELICAN_INPUT_7_LOCAL_READ 0
#endif

// Engine fullscreen shaders use PELICAN_DECLARE_INPUT_N so the same source
// can compile either to a sampled-image descriptor or to a same-pixel input
// attachment selected by the physical rendering-scope compiler.
#if PELICAN_INPUT_0_LOCAL_READ
#define PELICAN_DECLARE_INPUT_0(name) layout(input_attachment_index = 0, set = PELICAN_SET_PASS_INPUT, binding = 0) uniform subpassInput name
#define PELICAN_TEXTURE_2D_0(value, uv) subpassLoad(value)
#elif PELICAN_INPUT_0_LAYERED
#define PELICAN_SAMPLER_2D_0 sampler2DArray
#define PELICAN_DECLARE_INPUT_0(name) layout(set = PELICAN_SET_PASS_INPUT, binding = 0) uniform PELICAN_SAMPLER_2D_0 name
#define PELICAN_TEXTURE_2D_0(value, uv) texture(value, vec3((uv), float(PELICAN_VIEW_INDEX)))
#else
#define PELICAN_SAMPLER_2D_0 sampler2D
#define PELICAN_DECLARE_INPUT_0(name) layout(set = PELICAN_SET_PASS_INPUT, binding = 0) uniform PELICAN_SAMPLER_2D_0 name
#define PELICAN_TEXTURE_2D_0(value, uv) texture(value, (uv))
#endif
#if PELICAN_INPUT_1_LOCAL_READ
#define PELICAN_DECLARE_INPUT_1(name) layout(input_attachment_index = 1, set = PELICAN_SET_PASS_INPUT, binding = 1) uniform subpassInput name
#define PELICAN_TEXTURE_2D_1(value, uv) subpassLoad(value)
#elif PELICAN_INPUT_1_LAYERED
#define PELICAN_SAMPLER_2D_1 sampler2DArray
#define PELICAN_DECLARE_INPUT_1(name) layout(set = PELICAN_SET_PASS_INPUT, binding = 1) uniform PELICAN_SAMPLER_2D_1 name
#define PELICAN_TEXTURE_2D_1(value, uv) texture(value, vec3((uv), float(PELICAN_VIEW_INDEX)))
#else
#define PELICAN_SAMPLER_2D_1 sampler2D
#define PELICAN_DECLARE_INPUT_1(name) layout(set = PELICAN_SET_PASS_INPUT, binding = 1) uniform PELICAN_SAMPLER_2D_1 name
#define PELICAN_TEXTURE_2D_1(value, uv) texture(value, (uv))
#endif
#if PELICAN_INPUT_2_LOCAL_READ
#define PELICAN_DECLARE_INPUT_2(name) layout(input_attachment_index = 2, set = PELICAN_SET_PASS_INPUT, binding = 2) uniform subpassInput name
#define PELICAN_TEXTURE_2D_2(value, uv) subpassLoad(value)
#elif PELICAN_INPUT_2_LAYERED
#define PELICAN_SAMPLER_2D_2 sampler2DArray
#define PELICAN_DECLARE_INPUT_2(name) layout(set = PELICAN_SET_PASS_INPUT, binding = 2) uniform PELICAN_SAMPLER_2D_2 name
#define PELICAN_TEXTURE_2D_2(value, uv) texture(value, vec3((uv), float(PELICAN_VIEW_INDEX)))
#else
#define PELICAN_SAMPLER_2D_2 sampler2D
#define PELICAN_DECLARE_INPUT_2(name) layout(set = PELICAN_SET_PASS_INPUT, binding = 2) uniform PELICAN_SAMPLER_2D_2 name
#define PELICAN_TEXTURE_2D_2(value, uv) texture(value, (uv))
#endif
#if PELICAN_INPUT_3_LOCAL_READ
#define PELICAN_DECLARE_INPUT_3(name) layout(input_attachment_index = 3, set = PELICAN_SET_PASS_INPUT, binding = 3) uniform subpassInput name
#define PELICAN_TEXTURE_2D_3(value, uv) subpassLoad(value)
#elif PELICAN_INPUT_3_LAYERED
#define PELICAN_SAMPLER_2D_3 sampler2DArray
#define PELICAN_DECLARE_INPUT_3(name) layout(set = PELICAN_SET_PASS_INPUT, binding = 3) uniform PELICAN_SAMPLER_2D_3 name
#define PELICAN_TEXTURE_2D_3(value, uv) texture(value, vec3((uv), float(PELICAN_VIEW_INDEX)))
#else
#define PELICAN_SAMPLER_2D_3 sampler2D
#define PELICAN_DECLARE_INPUT_3(name) layout(set = PELICAN_SET_PASS_INPUT, binding = 3) uniform PELICAN_SAMPLER_2D_3 name
#define PELICAN_TEXTURE_2D_3(value, uv) texture(value, (uv))
#endif
#if PELICAN_INPUT_4_LOCAL_READ
#define PELICAN_DECLARE_INPUT_4(name) layout(input_attachment_index = 4, set = PELICAN_SET_PASS_INPUT, binding = 4) uniform subpassInput name
#define PELICAN_TEXTURE_2D_4(value, uv) subpassLoad(value)
#elif PELICAN_INPUT_4_LAYERED
#define PELICAN_SAMPLER_2D_4 sampler2DArray
#define PELICAN_DECLARE_INPUT_4(name) layout(set = PELICAN_SET_PASS_INPUT, binding = 4) uniform PELICAN_SAMPLER_2D_4 name
#define PELICAN_TEXTURE_2D_4(value, uv) texture(value, vec3((uv), float(PELICAN_VIEW_INDEX)))
#else
#define PELICAN_SAMPLER_2D_4 sampler2D
#define PELICAN_DECLARE_INPUT_4(name) layout(set = PELICAN_SET_PASS_INPUT, binding = 4) uniform PELICAN_SAMPLER_2D_4 name
#define PELICAN_TEXTURE_2D_4(value, uv) texture(value, (uv))
#endif
#if PELICAN_INPUT_5_LOCAL_READ
#define PELICAN_DECLARE_INPUT_5(name) layout(input_attachment_index = 5, set = PELICAN_SET_PASS_INPUT, binding = 5) uniform subpassInput name
#define PELICAN_TEXTURE_2D_5(value, uv) subpassLoad(value)
#elif PELICAN_INPUT_5_LAYERED
#define PELICAN_SAMPLER_2D_5 sampler2DArray
#define PELICAN_DECLARE_INPUT_5(name) layout(set = PELICAN_SET_PASS_INPUT, binding = 5) uniform PELICAN_SAMPLER_2D_5 name
#define PELICAN_TEXTURE_2D_5(value, uv) texture(value, vec3((uv), float(PELICAN_VIEW_INDEX)))
#else
#define PELICAN_SAMPLER_2D_5 sampler2D
#define PELICAN_DECLARE_INPUT_5(name) layout(set = PELICAN_SET_PASS_INPUT, binding = 5) uniform PELICAN_SAMPLER_2D_5 name
#define PELICAN_TEXTURE_2D_5(value, uv) texture(value, (uv))
#endif
#if PELICAN_INPUT_6_LOCAL_READ
#define PELICAN_DECLARE_INPUT_6(name) layout(input_attachment_index = 6, set = PELICAN_SET_PASS_INPUT, binding = 6) uniform subpassInput name
#define PELICAN_TEXTURE_2D_6(value, uv) subpassLoad(value)
#elif PELICAN_INPUT_6_LAYERED
#define PELICAN_SAMPLER_2D_6 sampler2DArray
#define PELICAN_DECLARE_INPUT_6(name) layout(set = PELICAN_SET_PASS_INPUT, binding = 6) uniform PELICAN_SAMPLER_2D_6 name
#define PELICAN_TEXTURE_2D_6(value, uv) texture(value, vec3((uv), float(PELICAN_VIEW_INDEX)))
#else
#define PELICAN_SAMPLER_2D_6 sampler2D
#define PELICAN_DECLARE_INPUT_6(name) layout(set = PELICAN_SET_PASS_INPUT, binding = 6) uniform PELICAN_SAMPLER_2D_6 name
#define PELICAN_TEXTURE_2D_6(value, uv) texture(value, (uv))
#endif
#if PELICAN_INPUT_7_LOCAL_READ
#define PELICAN_DECLARE_INPUT_7(name) layout(input_attachment_index = 7, set = PELICAN_SET_PASS_INPUT, binding = 7) uniform subpassInput name
#define PELICAN_TEXTURE_2D_7(value, uv) subpassLoad(value)
#elif PELICAN_INPUT_7_LAYERED
#define PELICAN_SAMPLER_2D_7 sampler2DArray
#define PELICAN_DECLARE_INPUT_7(name) layout(set = PELICAN_SET_PASS_INPUT, binding = 7) uniform PELICAN_SAMPLER_2D_7 name
#define PELICAN_TEXTURE_2D_7(value, uv) texture(value, vec3((uv), float(PELICAN_VIEW_INDEX)))
#else
#define PELICAN_SAMPLER_2D_7 sampler2D
#define PELICAN_DECLARE_INPUT_7(name) layout(set = PELICAN_SET_PASS_INPUT, binding = 7) uniform PELICAN_SAMPLER_2D_7 name
#define PELICAN_TEXTURE_2D_7(value, uv) texture(value, (uv))
#endif

// VI-1: variadic pass inputs. PELICAN_DECLARE_VARIADIC_INPUTS(first, max)
// declares set PELICAN_SET_PASS_INPUT bindings first .. first+max-1 as 2D
// sampled images named pelican_variadic_input_<k> (k = 0 .. max-1). The
// engine recognises the group by that prefix in reflection. The rules:
//   - the fixed (PELICAN_DECLARE_INPUT_<n>) texture inputs come first and the
//     variadic group is last; one group per shader;
//   - a pass supplies at most first+max texture inputs, and its buffer inputs
//     sit at binding first+max onwards (not after the supplied textures);
//   - a variadic binding the pass does not supply reads the engine's default
//     (zeros) without a warning.
// `first` and `max` must be integer literals (or macros that expand to one);
// max is 1 .. PELICAN_VARIADIC_INPUTS_LIMIT. Use it once, at global scope,
// followed by a semicolon:
//     PELICAN_DECLARE_VARIADIC_INPUTS(3, 8);
//     ... PELICAN_VARIADIC_TEXTURE(k, uv) for k in [0, PELICAN_VARIADIC_INPUT_COUNT)
// PELICAN_VARIADIC_INPUT_COUNT is how many of the group the pass supplies
// (a constant int expression, not a preprocessor number): the engine defines
// PELICAN_PASS_TEXTURE_INPUT_COUNT when it compiles a fullscreen / raster
// pass, and the count is that minus `first`, clamped to [0, max].
// PELICAN_VARIADIC_TEXTURE(k, uv) samples variadic input k (k may be a
// loop variable); k outside [0, max) reads vec4(0).
#ifndef PELICAN_PASS_TEXTURE_INPUT_COUNT
#define PELICAN_PASS_TEXTURE_INPUT_COUNT 0
#endif
#define PELICAN_VARIADIC_INPUTS_LIMIT 16

#define PELICAN_VARIADIC_DECLARE_ONE(first, k) \
    layout(set = PELICAN_SET_PASS_INPUT, binding = (first) + k) uniform sampler2D pelican_variadic_input_##k;
#define PELICAN_VARIADIC_CASE_ONE(k, index, uv) \
    if ((index) == k) return texture(pelican_variadic_input_##k, (uv));
#define PELICAN_VARIADIC_DECLARE_1(first) PELICAN_VARIADIC_DECLARE_ONE(first, 0)
#define PELICAN_VARIADIC_DECLARE_2(first) PELICAN_VARIADIC_DECLARE_1(first) PELICAN_VARIADIC_DECLARE_ONE(first, 1)
#define PELICAN_VARIADIC_DECLARE_3(first) PELICAN_VARIADIC_DECLARE_2(first) PELICAN_VARIADIC_DECLARE_ONE(first, 2)
#define PELICAN_VARIADIC_DECLARE_4(first) PELICAN_VARIADIC_DECLARE_3(first) PELICAN_VARIADIC_DECLARE_ONE(first, 3)
#define PELICAN_VARIADIC_DECLARE_5(first) PELICAN_VARIADIC_DECLARE_4(first) PELICAN_VARIADIC_DECLARE_ONE(first, 4)
#define PELICAN_VARIADIC_DECLARE_6(first) PELICAN_VARIADIC_DECLARE_5(first) PELICAN_VARIADIC_DECLARE_ONE(first, 5)
#define PELICAN_VARIADIC_DECLARE_7(first) PELICAN_VARIADIC_DECLARE_6(first) PELICAN_VARIADIC_DECLARE_ONE(first, 6)
#define PELICAN_VARIADIC_DECLARE_8(first) PELICAN_VARIADIC_DECLARE_7(first) PELICAN_VARIADIC_DECLARE_ONE(first, 7)
#define PELICAN_VARIADIC_DECLARE_9(first) PELICAN_VARIADIC_DECLARE_8(first) PELICAN_VARIADIC_DECLARE_ONE(first, 8)
#define PELICAN_VARIADIC_DECLARE_10(first) PELICAN_VARIADIC_DECLARE_9(first) PELICAN_VARIADIC_DECLARE_ONE(first, 9)
#define PELICAN_VARIADIC_DECLARE_11(first) PELICAN_VARIADIC_DECLARE_10(first) PELICAN_VARIADIC_DECLARE_ONE(first, 10)
#define PELICAN_VARIADIC_DECLARE_12(first) PELICAN_VARIADIC_DECLARE_11(first) PELICAN_VARIADIC_DECLARE_ONE(first, 11)
#define PELICAN_VARIADIC_DECLARE_13(first) PELICAN_VARIADIC_DECLARE_12(first) PELICAN_VARIADIC_DECLARE_ONE(first, 12)
#define PELICAN_VARIADIC_DECLARE_14(first) PELICAN_VARIADIC_DECLARE_13(first) PELICAN_VARIADIC_DECLARE_ONE(first, 13)
#define PELICAN_VARIADIC_DECLARE_15(first) PELICAN_VARIADIC_DECLARE_14(first) PELICAN_VARIADIC_DECLARE_ONE(first, 14)
#define PELICAN_VARIADIC_DECLARE_16(first) PELICAN_VARIADIC_DECLARE_15(first) PELICAN_VARIADIC_DECLARE_ONE(first, 15)
#define PELICAN_VARIADIC_CASES_1(index, uv) PELICAN_VARIADIC_CASE_ONE(0, index, uv)
#define PELICAN_VARIADIC_CASES_2(index, uv) PELICAN_VARIADIC_CASES_1(index, uv) PELICAN_VARIADIC_CASE_ONE(1, index, uv)
#define PELICAN_VARIADIC_CASES_3(index, uv) PELICAN_VARIADIC_CASES_2(index, uv) PELICAN_VARIADIC_CASE_ONE(2, index, uv)
#define PELICAN_VARIADIC_CASES_4(index, uv) PELICAN_VARIADIC_CASES_3(index, uv) PELICAN_VARIADIC_CASE_ONE(3, index, uv)
#define PELICAN_VARIADIC_CASES_5(index, uv) PELICAN_VARIADIC_CASES_4(index, uv) PELICAN_VARIADIC_CASE_ONE(4, index, uv)
#define PELICAN_VARIADIC_CASES_6(index, uv) PELICAN_VARIADIC_CASES_5(index, uv) PELICAN_VARIADIC_CASE_ONE(5, index, uv)
#define PELICAN_VARIADIC_CASES_7(index, uv) PELICAN_VARIADIC_CASES_6(index, uv) PELICAN_VARIADIC_CASE_ONE(6, index, uv)
#define PELICAN_VARIADIC_CASES_8(index, uv) PELICAN_VARIADIC_CASES_7(index, uv) PELICAN_VARIADIC_CASE_ONE(7, index, uv)
#define PELICAN_VARIADIC_CASES_9(index, uv) PELICAN_VARIADIC_CASES_8(index, uv) PELICAN_VARIADIC_CASE_ONE(8, index, uv)
#define PELICAN_VARIADIC_CASES_10(index, uv) PELICAN_VARIADIC_CASES_9(index, uv) PELICAN_VARIADIC_CASE_ONE(9, index, uv)
#define PELICAN_VARIADIC_CASES_11(index, uv) PELICAN_VARIADIC_CASES_10(index, uv) PELICAN_VARIADIC_CASE_ONE(10, index, uv)
#define PELICAN_VARIADIC_CASES_12(index, uv) PELICAN_VARIADIC_CASES_11(index, uv) PELICAN_VARIADIC_CASE_ONE(11, index, uv)
#define PELICAN_VARIADIC_CASES_13(index, uv) PELICAN_VARIADIC_CASES_12(index, uv) PELICAN_VARIADIC_CASE_ONE(12, index, uv)
#define PELICAN_VARIADIC_CASES_14(index, uv) PELICAN_VARIADIC_CASES_13(index, uv) PELICAN_VARIADIC_CASE_ONE(13, index, uv)
#define PELICAN_VARIADIC_CASES_15(index, uv) PELICAN_VARIADIC_CASES_14(index, uv) PELICAN_VARIADIC_CASE_ONE(14, index, uv)
#define PELICAN_VARIADIC_CASES_16(index, uv) PELICAN_VARIADIC_CASES_15(index, uv) PELICAN_VARIADIC_CASE_ONE(15, index, uv)

// The group's first declaration is the constant pelican_variadic_first, so a
// second group in one shader fails to compile as its redefinition; the
// engine reports that as multiple_variadic_groups.
#define PELICAN_VARIADIC_DECLARE_GROUP(first, max) \
    const int pelican_variadic_first = (first); \
    const int pelican_variadic_max = (max); \
    PELICAN_VARIADIC_DECLARE_##max(first) \
    vec4 pelican_variadic_texture(int pelican_variadic_index, vec2 pelican_variadic_uv) { \
        PELICAN_VARIADIC_CASES_##max(pelican_variadic_index, pelican_variadic_uv) \
        return vec4(0.0); \
    } \
    const int pelican_variadic_input_count = \
        clamp(int(PELICAN_PASS_TEXTURE_INPUT_COUNT) - (first), 0, (max))
#define PELICAN_DECLARE_VARIADIC_INPUTS(first, max) \
    PELICAN_VARIADIC_DECLARE_GROUP(first, max)
#define PELICAN_VARIADIC_INPUT_COUNT pelican_variadic_input_count
#define PELICAN_VARIADIC_TEXTURE(k, uv) pelican_variadic_texture(int(k), (uv))

#endif
