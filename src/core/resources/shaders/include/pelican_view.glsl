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

#endif
