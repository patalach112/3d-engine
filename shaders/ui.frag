#version 450

// Analytic rounded-rectangle UI (Phase 26): no textures, no atlas.
// The rect's pixel size is recovered from the uv derivatives (uv
// spans 0..1 across the quad), so corner radius and border width
// stay in exact pixel units at any rect size. One rounded-box SDF
// evaluation yields the fill, the border ring and anti-aliased
// coverage — pixel-perfect at every resolution.

layout(location = 0) in vec2 fragUv;
layout(location = 1) in vec4 fragColor;
layout(location = 2) in float fragCornerRadius;
layout(location = 3) in float fragBorderWidth;
layout(location = 4) in vec4 fragBorderColor;

layout(location = 0) out vec4 outColor;

float roundedBoxSdf(vec2 point, vec2 halfSize, float radius) {
    vec2 q = abs(point) - halfSize + radius;
    return length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - radius;
}

void main() {
    vec2 rectSize = 1.0 / max(fwidth(fragUv), vec2(1e-5));
    vec2 halfSize = 0.5 * rectSize;
    vec2 point = (fragUv - 0.5) * rectSize;
    float radius = min(fragCornerRadius, min(halfSize.x, halfSize.y));
    float dist = roundedBoxSdf(point, halfSize, radius);
    float aa = max(fwidth(dist), 1e-5);
    float coverage = 1.0 - smoothstep(-aa, aa, dist);
    if (coverage <= 0.0) {
        discard;
    }
    vec4 color = fragColor;
    if (fragBorderWidth > 0.0) {
        float border = min(fragBorderWidth, min(halfSize.x, halfSize.y));
        float inner = roundedBoxSdf(point, halfSize - border, max(radius - border, 0.0));
        float fill = 1.0 - smoothstep(-aa, aa, inner);
        color = mix(fragBorderColor, fragColor, fill);
    }
    outColor = vec4(color.rgb, color.a * coverage);
}
