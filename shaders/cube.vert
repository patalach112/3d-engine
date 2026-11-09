#version 450

layout(set = 0, binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
} ubo;

layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 baseColor;
} push;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec3 fragNormal;

void main() {
    gl_Position = ubo.proj * ubo.view * push.model * vec4(inPosition, 1.0);
    fragColor = push.baseColor.rgb * inColor;
    // Correct world-space normal (inverse-transpose handles non-uniform
    // scale, e.g. the flattened floor, without skewing the lighting).
    fragNormal = mat3(transpose(inverse(push.model))) * inNormal;
}
