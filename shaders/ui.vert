#version 450

// Screen-space UI vertex shader (Phase 26). Positions arrive in
// pixels (top-left origin, y down) and are converted to clip space
// with the viewport size pushed per flush. uv spans 0..1 across
// the quad; textured primitives (images, SDF text) additionally
// carry an atlas rect (uvRect) and a kind tag (0 = analytic
// shape, 1 = sampled image, 2 = SDF text), resolved in ui.frag.

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec4 inColor;
layout(location = 3) in float inCornerRadius;
layout(location = 4) in float inBorderWidth;
layout(location = 5) in vec4 inBorderColor;
layout(location = 6) in vec4 inUvRect;
layout(location = 7) in float inKind;

layout(push_constant) uniform PushConstants {
    vec2 viewportSize;
} push;

layout(location = 0) out vec2 fragUv;
layout(location = 1) out vec4 fragColor;
layout(location = 2) out float fragCornerRadius;
layout(location = 3) out float fragBorderWidth;
layout(location = 4) out vec4 fragBorderColor;
layout(location = 5) out vec4 fragUvRect;
layout(location = 6) flat out float fragKind;

void main() {
    vec2 ndc = inPosition / push.viewportSize * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    fragUv = inUv;
    fragColor = inColor;
    fragCornerRadius = inCornerRadius;
    fragBorderWidth = inBorderWidth;
    fragBorderColor = inBorderColor;
    fragUvRect = inUvRect;
    fragKind = inKind;
}
