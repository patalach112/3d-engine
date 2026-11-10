#version 450

// Depth-only cascaded shadow pass (Phase 21): project geometry into
// the current cascade's light space. No fragment stage — the
// pipeline's depth attachment records the nearest light-space depth
// per texel. The renderer renders one pass per cascade; the cascade
// index arrives through the extended push-constant block.

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
    // Phase 32 appended fields (same offsets as Renderer.h).
    vec4 directionalColor;
    mat4 spotShadowViewProj;
} ubo;

// Same 112-byte draw payload as the main pass (only the model matrix
// at its head is consumed) plus the cascade index at byte offset 112.
layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 baseColor;
    vec4 pbrParams;
    ivec4 textureIndices;
    uint cascadeIndex;
} push;

layout(location = 0) in vec3 inPosition;

void main() {
    // Push indices 0..3 select directional cascades; index 4 is
    // the Phase 32 spot shadow pass and uses its own matrix.
    mat4 lightMatrix = push.cascadeIndex >= 4u ? ubo.spotShadowViewProj
                                               : ubo.lightViewProj[min(push.cascadeIndex, 3u)];
    gl_Position = lightMatrix * push.model * vec4(inPosition, 1.0);
}
