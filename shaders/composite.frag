#version 450

// Composite (Phase 22): HDR scene + bloom, exposure, ACES filmic
// tone mapping, sRGB encoding — the only pass that writes to the
// swapchain. Mirrors Graphics/ToneMapping.h.

layout(set = 0, binding = 0) uniform sampler2D sceneTexture;
layout(set = 0, binding = 1) uniform sampler2D bloomTexture;

layout(push_constant) uniform PushConstants {
    float exposure;
    float bloomStrength;
    float bloomEnabled;
    float chromaticAberration;
} push;

layout(location = 0) in vec2 fragUv;

layout(location = 0) out vec4 outColor;

vec3 acesFilmic(vec3 color) {
    vec3 x = max(color, vec3(0.0));
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

float linearToSrgb(float value) {
    float v = clamp(value, 0.0, 1.0);
    return v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
}

void main() {
    // Chromatic aberration: R/B channels split radially from the
    // centre, scaled by the (script-driven) aberration amount.
    vec2 caOffset = (fragUv - 0.5) * push.chromaticAberration * 0.02;
    vec3 hdr;
    hdr.r = texture(sceneTexture, fragUv + caOffset).r;
    hdr.g = texture(sceneTexture, fragUv).g;
    hdr.b = texture(sceneTexture, fragUv - caOffset).b;
    if (push.bloomEnabled > 0.5) {
        hdr += texture(bloomTexture, fragUv).rgb * push.bloomStrength;
    }
    vec3 mapped = acesFilmic(hdr * push.exposure);
    outColor = vec4(linearToSrgb(mapped.r), linearToSrgb(mapped.g), linearToSrgb(mapped.b), 1.0);
}
