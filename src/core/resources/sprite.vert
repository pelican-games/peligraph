#version 450
#extension GL_GOOGLE_include_directive : enable

#include "pelican_frame.glsl"

layout(location = 0) in vec2 inCorner;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec4 inWorld0;
layout(location = 3) in vec4 inWorld1;
layout(location = 4) in vec4 inWorld2;
layout(location = 5) in vec4 inWorld3;
layout(location = 6) in vec4 inColor;
layout(location = 7) in uint inBillboard;

layout(location = 0) out vec2 fragUv;
layout(location = 1) out vec4 fragColor;

void main() {
    mat4 world = mat4(inWorld0, inWorld1, inWorld2, inWorld3);
    vec3 local = vec3(inCorner, 0.0);
    vec3 worldPosition;
    if (inBillboard == 0u) {
        worldPosition = (world * vec4(local, 1.0)).xyz;
    } else {
        vec3 center = world[3].xyz;
        float width = length(world[0].xyz);
        float height = length(world[1].xyz);
        vec3 right;
        vec3 up;
        if (inBillboard == 1u) {
            vec3 toCamera = pelicanFrame.camera_position.xyz - center;
            toCamera.y = 0.0;
            toCamera = length(toCamera) > 0.00001 ? normalize(toCamera) : vec3(0.0, 0.0, 1.0);
            right = normalize(cross(vec3(0.0, 1.0, 0.0), toCamera));
            up = vec3(0.0, 1.0, 0.0);
        } else {
            right = vec3(pelicanFrame.view[0][0], pelicanFrame.view[1][0], pelicanFrame.view[2][0]);
            up = vec3(pelicanFrame.view[0][1], pelicanFrame.view[1][1], pelicanFrame.view[2][1]);
        }
        worldPosition = center + right * (local.x * width) + up * (local.y * height);
    }
    gl_Position = pelicanFrame.projection * pelicanFrame.view * vec4(worldPosition, 1.0);
    fragUv = inUv;
    fragColor = inColor;
}
