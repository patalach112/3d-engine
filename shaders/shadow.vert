#version 450

// Depth-only shadow pass: project geometry into the key light's
// orthographic space. No fragment stage — the pipeline's depth
// attachment records the nearest light-space depth per texel.

layout(set = 0, binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
    mat4 lightViewProj;
    vec4 lightDirection;
} ubo;

// Same 112-byte push payload as the main pass; only the model matrix at
// its head is consumed here.
layout(push_constant) uniform PushConstants {
    mat4 model;
} push;

layout(location = 0) in vec3 inPosition;

void main() {
    gl_Position = ubo.lightViewProj * push.model * vec4(inPosition, 1.0);
}
