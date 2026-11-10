#version 450

// Screen-space UI fragment shader (Phase 26). Three primitive
// kinds share one pipeline:
//   0 analytic shape — one rounded-box SDF yields fill, border
//     ring and anti-aliased coverage; no texture involved.
//   1 sampled image — texture * tint, clipped by the same SDF so
//     icons get pixel-perfect rounded corners for free.
//   2 SDF text — the baked distance field (uiFont atlas, red
//     channel) is thresholded with a derivative-sized band, which
//     stays crisp at any scale.
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
        // SDF text: 0.5 is the glyph edge in the baked field.
        // Convert the field to a *screen-space* signed distance in
        // pixels (field units divided by the field's own gradient)
        // and anti-alias over exactly one pixel either side of the
        // edge. The old form -- smoothstep over a +/-fwidth band --
        // reaches full opacity only one whole pixel *inside* the
        // edge, so at editor text sizes (drawn at roughly a quarter
        // of the 48 px bake) thin strokes like 'i', 'l' and '.'
        // never get there and render washed-out grey. Here, any
        // coverage at least half a pixel inside the edge is fully
        // opaque and the ramp is symmetric, so letters stay solid
        // at any scale while edges remain smooth.
        float field = texture(uiTexture, atlasUv).r;
        float gradient = max(fwidth(field), 1e-4);
        float distancePx = (field - 0.5) / gradient;
        float alpha = clamp(distancePx + 0.5, 0.0, 1.0);
        if (alpha <= 0.0) {
            discard;
        }
        outColor = vec4(fragColor.rgb, fragColor.a * alpha);
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
