#version 450

// Skybox (Phase 22): the cube geometry rendered inside-out at the
// far plane. Only the rotation part of the view matrix applies (the
// sky is infinitely far away), and gl_Position's z is pinned to w so
// every fragment lands exactly at depth 1.0 behind the scene.

layout(set = 0, binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
    mat4 lightViewProj[4];
    vec4 lightDirection;
    vec4 cascadeSplits;
    vec4 lightCounts;
    vec4 lightPositionRange[16];
    vec4 lightColorIntensity[16];
    vec4 lightDirectionType[16];
    vec4 lightCone[16];
    vec4 environmentParams;
} ubo;

layout(location = 0) in vec3 inPosition;

layout(location = 0) out vec3 fragDirection;

void main() {
    fragDirection = inPosition;
    vec4 position = ubo.proj * mat4(mat3(ubo.view)) * vec4(inPosition, 1.0);
    gl_Position = position.xyww;
}
