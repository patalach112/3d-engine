#version 450

// Bloom upsample: 3x3 tent filter from the next-smaller mip,
// accumulated additively (pipeline blend ONE/ONE) onto the current
// level, rebuilding a wide multi-scale glow.

layout(set = 0, binding = 0) uniform sampler2D sourceTexture;

layout(push_constant) uniform PushConstants {
    vec2 texelSize; // 1 / source resolution
    float padding0;
    float padding1;
} push;

layout(location = 0) in vec2 fragUv;

layout(location = 0) out vec4 outColor;

void main() {
    vec2 uv = fragUv;
    vec2 ts = push.texelSize;
    vec3 result = texture(sourceTexture, uv).rgb * 4.0;
    result += texture(sourceTexture, uv + vec2(-1.0, 0.0) * ts).rgb * 2.0;
    result += texture(sourceTexture, uv + vec2(1.0, 0.0) * ts).rgb * 2.0;
    result += texture(sourceTexture, uv + vec2(0.0, -1.0) * ts).rgb * 2.0;
    result += texture(sourceTexture, uv + vec2(0.0, 1.0) * ts).rgb * 2.0;
    result += texture(sourceTexture, uv + vec2(-1.0, -1.0) * ts).rgb;
    result += texture(sourceTexture, uv + vec2(1.0, -1.0) * ts).rgb;
    result += texture(sourceTexture, uv + vec2(-1.0, 1.0) * ts).rgb;
    result += texture(sourceTexture, uv + vec2(1.0, 1.0) * ts).rgb;
    outColor = vec4(result / 16.0, 1.0);
}
