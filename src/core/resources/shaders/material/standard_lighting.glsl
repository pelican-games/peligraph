vec3 pelican_lighting_v1(in PelicanSurfaceV1 surface, in PelicanSurfaceInputV1 surface_input) {
    vec3 color = surface.emissive + surface.base_color.rgb * pelican_env_ambient(surface.normal);
    for (uint i = 0u; i < pelican_light_count(); ++i) {
        PelicanLightV1 light = pelican_light(i, surface_input.world_position);
        float ndotl = max(dot(surface.normal, light.direction), 0.0);
        float visibility = pelican_shadow(i, surface_input.world_position);
        color += surface.base_color.rgb * light.radiance *
                 (ndotl * light.attenuation * visibility);
    }
    return color;
}
