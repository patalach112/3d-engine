#version 450

// Per-frame uniform block — byte-identical with the C++
// UniformBufferObject (Renderer.h): camera matrices, cascaded shadow
// matrices, the packed dynamic-light arrays, and environment
// parameters (Phase 21/22).
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
    vec4 environmentParams; // x intensity, y prefilter max LOD, z skybox on
} ubo;

layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 baseColor;
    vec4 pbrParams; // x metallic, y roughness, z occlusion strength
    ivec4 textureIndices; // albedo, metallicRoughness, normal, occlusion (-1 = none)
} push;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;
layout(location = 3) in vec2 inTexCoord;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec3 fragWorldPosition;
layout(location = 3) out vec3 fragCameraPosition;
layout(location = 4) out vec3 fragPbrParams; // metallic, roughness, occlusion strength
layout(location = 5) flat out ivec4 fragTextureIndices;
layout(location = 6) out float viewDepth; // positive distance in front of the camera
layout(location = 7) out vec2 fragTexCoord;

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
    fragPbrParams = push.pbrParams.xyz;
    fragTextureIndices = push.textureIndices;
    fragTexCoord = inTexCoord;
}
