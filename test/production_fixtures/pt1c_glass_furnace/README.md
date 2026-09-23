# PT-1c glass furnace fixture

A lossless, perfectly smooth dielectric sphere (IOR 1.5, transmission 1,
base color 1) under a unit radiance environment, seen by an orthographic
camera. Every path that enters the sphere leaves it toward the sky, and the
Fresnel reflection and refraction weights sum to one, so every silhouette
pixel must read linear radiance 1.0 when enough bounces are allowed
(`PT_MAX_BOUNCES` is 32 here). This checks energy conservation of the
transmission lobe and the eta^2 radiance scaling; it does not check the
refraction direction.

All shader files except `pt_scene.glsl` are byte-identical to the shipping
`projects/pathtrace_demo` shaders.
