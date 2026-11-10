#version 450

// Screen-space UI fragment shader (Phase 26). Three primitive
// kinds share one pipeline:
//   0 analytic shape — one rounded-box SDF yields fill, border
//     ring and anti-aliased coverage; no texture involved.
//   1 sampled image — texture * tint, clipped by the same SDF so
//     icons get pixel-perfect rounded corners for free.
//   2 bitmap text — the font atlas (red channel) stores glyph
//     coverage baked at the target pixel size; the sampled
//     coverage is the text alpha directly. Coverage filters
//     linearly, so thin strokes keep their ink at small sizes
//     (a thresholded distance field washed them out).
// The rect's pixel size is recovered from the uv derivatives (uv
// spans 0..1 across the quad), keeping radius/border widths in
// exact pixel units without extra attributes.

layout(location = 0) in vec2 fragUv;
layout(location = 1) in vec4 fragColor;
layout(location = 2) in float fragCornerRadius;
layout(location = 3) in float fragBorderWidth;
layout(location = 4) in vec4 fragBorderColor;
layout(location = 5) in vec4 fragUvRect;
layout(location = 6) flat in float fragKind;

layout(set = 0, binding = 0) uniform sampler2D uiTexture;

layout(location = 0) out vec4 outColor;

float roundedBoxSdf(vec2 point, vec2 halfSize, float radius) {
    vec2 q = abs(point) - halfSize + radius;
    return length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - radius;
}

void main() {
    vec2 atlasUv = mix(fragUvRect.xy, fragUvRect.zw, fragUv);

    if (fragKind > 1.5) {
        // Bitmap text: the atlas stores coverage (0 = no ink,
        // 1 = solid) rasterized at the target pixel size, and the
        // quads are emitted snapped to whole pixels, so a stem
        // lands on pixel boundaries with its baked coverage
        // intact. The two-texel zero gap packed around every glyph
        // keeps bilinear edge taps free of neighbour ink. A small
        // cutout drops the faintest fringe texels so text cannot
        // halo against the panels; everything above it blends as
        // ordinary alpha.
        float coverage = texture(uiTexture, atlasUv).r;
        if (coverage <= 0.1) {
            discard;
        }
        outColor = vec4(fragColor.rgb, fragColor.a * coverage);
        return;
    }

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

    if (fragKind > 0.5) {
        // Sampled image, SDF-clipped, tinted.
        vec4 texel = texture(uiTexture, atlasUv);
        outColor = vec4(texel.rgb * fragColor.rgb, texel.a * fragColor.a * coverage);
        return;
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
