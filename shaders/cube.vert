#version 450

// Per-frame uniform block — byte-identical with the C++
// UniformBufferObject (Renderer.h): camera matrices, cascaded shadow
// matrices, and the packed dynamic-light arrays (Phase 21).
layout(set = 0, binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
    mat4 lightViewProj[4];
    vec4 lightDirection;
    vec4 cascadeSplits;
    vec4 lightCounts; // x point count, y spot count, z total
    vec4 lightPositionRange[16];
    vec4 lightColorIntensity[16];
    vec4 lightDirectionType[16];
    vec4 lightCone[16];
} ubo;

layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 baseColor;
    vec4 specular; // rgb: specular color, a: shininess
    vec4 surface;  // x: specular strength, y: rim strength, z: rim power, w: ambient strength
} push;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec3 fragWorldPosition;
layout(location = 3) out vec3 fragCameraPosition;
layout(location = 4) out vec4 fragSpecular;
layout(location = 5) out vec4 fragSurface;
layout(location = 6) out float viewDepth; // positive distance in front of the camera

void main() {
    vec4 worldPosition = push.model * vec4(inPosition, 1.0);
    vec4 viewPosition = ubo.view * worldPosition;
    gl_Position = ubo.proj * viewPosition;
    viewDepth = -viewPosition.z;
    fragColor = push.baseColor.rgb * inColor;
    // Correct world-space normal (inverse-transpose handles non-uniform
    // scale, e.g. the flattened floor, without skewing the lighting).
    fragNormal = mat3(transpose(inverse(push.model))) * inNormal;
    fragWorldPosition = worldPosition.xyz;
    // Camera world position recovered from the view matrix (for a rigid
    // view transform, eye = -R^T * t) — no extra uniform needed.
    fragCameraPosition = -transpose(mat3(ubo.view)) * ubo.view[3].xyz;
    fragSpecular = push.specular;
    fragSurface = push.surface;
}
