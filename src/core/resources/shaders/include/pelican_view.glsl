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

#if PELICAN_INPUT_0_LAYERED
#define PELICAN_SAMPLER_2D_0 sampler2DArray
#define PELICAN_TEXTURE_2D_0(value, uv) texture(value, vec3((uv), float(PELICAN_VIEW_INDEX)))
#else
#define PELICAN_SAMPLER_2D_0 sampler2D
#define PELICAN_TEXTURE_2D_0(value, uv) texture(value, (uv))
#endif
#if PELICAN_INPUT_1_LAYERED
#define PELICAN_SAMPLER_2D_1 sampler2DArray
#define PELICAN_TEXTURE_2D_1(value, uv) texture(value, vec3((uv), float(PELICAN_VIEW_INDEX)))
#else
#define PELICAN_SAMPLER_2D_1 sampler2D
#define PELICAN_TEXTURE_2D_1(value, uv) texture(value, (uv))
#endif
#if PELICAN_INPUT_2_LAYERED
#define PELICAN_SAMPLER_2D_2 sampler2DArray
#define PELICAN_TEXTURE_2D_2(value, uv) texture(value, vec3((uv), float(PELICAN_VIEW_INDEX)))
#else
#define PELICAN_SAMPLER_2D_2 sampler2D
#define PELICAN_TEXTURE_2D_2(value, uv) texture(value, (uv))
#endif
#if PELICAN_INPUT_3_LAYERED
#define PELICAN_SAMPLER_2D_3 sampler2DArray
#define PELICAN_TEXTURE_2D_3(value, uv) texture(value, vec3((uv), float(PELICAN_VIEW_INDEX)))
#else
#define PELICAN_SAMPLER_2D_3 sampler2D
#define PELICAN_TEXTURE_2D_3(value, uv) texture(value, (uv))
#endif
#if PELICAN_INPUT_4_LAYERED
#define PELICAN_SAMPLER_2D_4 sampler2DArray
#define PELICAN_TEXTURE_2D_4(value, uv) texture(value, vec3((uv), float(PELICAN_VIEW_INDEX)))
#else
#define PELICAN_SAMPLER_2D_4 sampler2D
#define PELICAN_TEXTURE_2D_4(value, uv) texture(value, (uv))
#endif
#if PELICAN_INPUT_5_LAYERED
#define PELICAN_SAMPLER_2D_5 sampler2DArray
#define PELICAN_TEXTURE_2D_5(value, uv) texture(value, vec3((uv), float(PELICAN_VIEW_INDEX)))
#else
#define PELICAN_SAMPLER_2D_5 sampler2D
#define PELICAN_TEXTURE_2D_5(value, uv) texture(value, (uv))
#endif
#if PELICAN_INPUT_6_LAYERED
#define PELICAN_SAMPLER_2D_6 sampler2DArray
#define PELICAN_TEXTURE_2D_6(value, uv) texture(value, vec3((uv), float(PELICAN_VIEW_INDEX)))
#else
#define PELICAN_SAMPLER_2D_6 sampler2D
#define PELICAN_TEXTURE_2D_6(value, uv) texture(value, (uv))
#endif
#if PELICAN_INPUT_7_LAYERED
#define PELICAN_SAMPLER_2D_7 sampler2DArray
#define PELICAN_TEXTURE_2D_7(value, uv) texture(value, vec3((uv), float(PELICAN_VIEW_INDEX)))
#else
#define PELICAN_SAMPLER_2D_7 sampler2D
#define PELICAN_TEXTURE_2D_7(value, uv) texture(value, (uv))
#endif

#endif
