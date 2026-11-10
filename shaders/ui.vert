#version 450

// Screen-space UI vertex shader (Phase 26). Positions arrive in
// pixels (top-left origin, y down) and are converted to clip space
// with the viewport size pushed per flush; styling attributes pass
// through untouched for the analytic fragment stage. uv spans 0..1
// across the quad, which is all the fragment shader needs to
// recover the rect's exact pixel size (see ui.frag).

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec4 inColor;
layout(location = 3) in float inCornerRadius;
layout(location = 4) in float inBorderWidth;
layout(location = 5) in vec4 inBorderColor;

layout(push_constant) uniform PushConstants {
    vec2 viewportSize;
} push;

layout(location = 0) out vec2 fragUv;
layout(location = 1) out vec4 fragColor;
layout(location = 2) out float fragCornerRadius;
layout(location = 3) out float fragBorderWidth;
layout(location = 4) out vec4 fragBorderColor;

void main() {
    vec2 ndc = inPosition / push.viewportSize * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    fragUv = inUv;
    fragColor = inColor;
    fragCornerRadius = inCornerRadius;
    fragBorderWidth = inBorderWidth;
    fragBorderColor = inBorderColor;
}
