#version 450

// Samples the raw environment cubemap (binding 6) as the scene
// background. HDR output; tone mapping happens in the composite.

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
    vec4 environmentParams; // x intensity
} ubo;

layout(set = 0, binding = 6) uniform samplerCube environmentMap;

layout(location = 0) in vec3 fragDirection;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 radiance = texture(environmentMap, normalize(fragDirection)).rgb;
    outColor = vec4(radiance * ubo.environmentParams.x, 1.0);
}
