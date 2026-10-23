#version 460

// BS-1: engine://fallback_clear. The one pass of the engine's built-in
// fallback generation: when a launch with --render-fallback cannot compose
// the project's rendering configuration at start, the engine runs on a graph
// that draws this flat colour to `display` and nothing else, until a fixed
// configuration is published (reload_render_documents or a render edit).
// The colour is deliberately not one a Blank project draws (its present pass
// draws a UV gradient), so a frame says which generation ran.

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(0.24, 0.04, 0.16, 1.0);
}
