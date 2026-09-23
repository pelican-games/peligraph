# PT-1a furnace fixture

This standalone, unregistered fixture surrounds a diffuse sphere with a unit
radiance environment. Its sphere has albedo 0.5, so every silhouette pixel
must converge to linear radiance 0.5 after its one diffuse reflection. The sky
remains linear radiance 1.0.

All shader files except `pt_scene.glsl` are byte-identical to the shipping
`projects/pathtrace_demo` shaders.
