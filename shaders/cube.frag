#version 450

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec3 fragNormal;

layout(location = 0) out vec4 outColor;

void main() {
    // Solid-material diffuse shading: ambient floor plus a fixed key light.
    // Because the pipeline writes depth and culls back faces, a face is
    // only ever shaded when it is the nearest visible surface — light and
    // color cannot bleed through solid geometry.
    vec3 normal = normalize(fragNormal);
    vec3 lightDirection = normalize(vec3(0.35, 0.8, 0.45));
    float diffuse = max(dot(normal, lightDirection), 0.0);
    vec3 shaded = fragColor * (0.35 + 0.75 * diffuse);
    outColor = vec4(shaded, 1.0);
}
