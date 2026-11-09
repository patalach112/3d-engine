#version 450

layout(set = 0, binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
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

void main() {
    vec4 worldPosition = push.model * vec4(inPosition, 1.0);
    gl_Position = ubo.proj * ubo.view * worldPosition;
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
