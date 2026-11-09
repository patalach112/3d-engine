#version 450

// Fullscreen triangle for the post chain (bloom + composite): one
// oversized triangle covers the framebuffer, UVs map 1:1 onto the
// source textures.

layout(location = 0) out vec2 fragUv;

void main() {
    fragUv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(fragUv * 2.0 - 1.0, 0.0, 1.0);
}
