#version 460

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

// G-Buffer inputs
layout(set = 0, binding = 0) uniform sampler2D worldPosSampler;
layout(set = 0, binding = 1) uniform sampler2D normalSampler;

// Push constants with camera matrices
layout(push_constant) uniform PushConstants {
    mat4 proj;
    mat4 view;
} pc;

// Parameters
const int KERNEL_SIZE = 32;
const float KERNEL_RADIUS = 0.5;
const float BIAS = 0.025;

// Hardcoded SSAO sample kernel
const vec3 SSAO_KERNEL[KERNEL_SIZE] = vec3[32](
    vec3(0.5381, 0.1856, -0.4319), vec3(0.1379, 0.2486, 0.4430),
    vec3(0.3371, 0.5679, -0.0057), vec3(-0.6999, -0.0451, -0.0019),
    vec3(0.0689, -0.1598, -0.8547), vec3(0.0560, 0.0069, -0.1843),
    vec3(-0.0146, 0.1402, 0.0762), vec3(0.0100, -0.1924, -0.0344),
    vec3(-0.3577, -0.5301, -0.4358), vec3(-0.3169, 0.1063, 0.0158),
    vec3(0.0103, -0.5869, 0.0046), vec3(-0.0897, -0.4940, 0.3287),
    vec3(0.7119, -0.0154, -0.0918), vec3(-0.0533, 0.0596, -0.5411),
    vec3(0.0352, -0.0631, 0.5460), vec3(-0.4776, 0.2847, -0.5617),
    vec3(-0.5523, 0.3016, -0.2981), vec3(0.5866, -0.5577, 0.0125),
    vec3(0.0009, -0.1703, 0.2038), vec3(0.6225, 0.1332, 0.1240),
    vec3(0.0019, 0.2248, -0.2104), vec3(0.0003, 0.5564, 0.3860),
    vec3(0.0004, -0.1932, 0.1502), vec3(0.0003, -0.4137, 0.2820),
    vec3(0.0004, 0.1705, -0.4882), vec3(0.0004, 0.4439, 0.3384),
    vec3(0.0001, -0.0016, 0.2195), vec3(0.0001, 0.2185, -0.3243),
    vec3(0.0001, -0.5186, 0.1853), vec3(0.0001, -0.2142, -0.3803),
    vec3(0.0000, 0.0016, -0.282), vec3(0.0000, -0.0008, 0.4939)
);

// Simple pseudo-random noise function
vec3 random(vec2 co) {
    return vec3(
        fract(sin(dot(co.xy, vec2(12.9898, 78.233))) * 43758.5453),
        fract(cos(dot(co.xy, vec2(12.9898, 78.233))) * 43758.5453),
        fract(sin(dot(co.xy, vec2(4.898, 7.23))) * 23458.5453)
    );
}

void main() {
    // Get G-Buffer data
    vec3 fragPos = (pc.view * vec4(texture(worldPosSampler, inUV).xyz, 1.0)).xyz;
    vec3 normal = normalize((pc.view * vec4(texture(normalSampler, inUV).xyz * 2.0 - 1.0, 0.0)).xyz);
    
    // Random rotation for sampling kernel
    vec3 randomVec = normalize(random(inUV * 1000.0));
    vec3 tangent = normalize(randomVec - normal * dot(randomVec, normal));
    vec3 bitangent = cross(normal, tangent);
    mat3 TBN = mat3(tangent, bitangent, normal);

    float occlusion = 0.0;
    for (int i = 0; i < KERNEL_SIZE; ++i) {
        // Get sample position
        vec3 samplePos = TBN * SSAO_KERNEL[i]; // From tangent to view-space
        samplePos = fragPos + samplePos * KERNEL_RADIUS;

        // Project sample position to screen space
        vec4 offset = vec4(samplePos, 1.0);
        offset = pc.proj * offset; // from view to clip-space
        offset.xyz /= offset.w; // perspective divide
        offset.xyz = offset.xyz * 0.5 + 0.5; // transform to [0,1] range

        // Get sample depth from G-Buffer
        vec3 occluderPos = (pc.view * vec4(texture(worldPosSampler, offset.xy).xyz, 1.0)).xyz;

        // Check if sample is within range and occluded
        float occluderDepth = occluderPos.z;
        float sampleDepth = samplePos.z;

        // Add to occlusion factor (range check helps prevent artifacts from samples outside view frustum)
        float rangeCheck = smoothstep(0.0, 1.0, KERNEL_RADIUS / abs(fragPos.z - occluderPos.z));
        occlusion += (occluderDepth >= sampleDepth + BIAS ? 1.0 : 0.0) * rangeCheck;
    }

    occlusion = 1.0 - (occlusion / KERNEL_SIZE);
    
    outColor = vec4(occlusion, occlusion, occlusion, 1.0);
}