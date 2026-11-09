#version 450

// Bloom downsample: 3x3 tent filter (weights [1 2 1] x [1 2 1] / 16)
// into the next mip of the chain. The first level additionally runs
// the soft-knee bright-pass so only HDR highlights enter the chain.

layout(set = 0, binding = 0) uniform sampler2D sourceTexture;

layout(push_constant) uniform PushConstants {
    vec2 texelSize; // 1 / source resolution
    float threshold; // <= 0 disables the bright-pass (pure downsample)
    float knee;
} push;

layout(location = 0) in vec2 fragUv;

layout(location = 0) out vec4 outColor;

// Soft-knee bright-pass (mirrors Graphics/ToneMapping.h).
vec3 extractBright(vec3 color, float threshold, float knee) {
    float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
    float soft = clamp((luminance - threshold + knee) / max(2.0 * knee, 1e-5), 0.0, 1.0);
    float weight = (soft * soft * max(luminance - threshold, 0.0)) / max(luminance, 1e-5);
    return color * weight;
}

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
    result /= 16.0;
    if (push.threshold > 0.0) {
        result = extractBright(result, push.threshold, push.knee);
    }
    outColor = vec4(result, 1.0);
}
