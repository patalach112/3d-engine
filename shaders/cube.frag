#version 450

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec3 fragWorldPosition;
layout(location = 3) in vec3 fragCameraPosition;
layout(location = 4) in vec4 fragSpecular;
layout(location = 5) in vec4 fragSurface;

layout(location = 0) out vec4 outColor;

void main() {
    // Directional lighting model: ambient + Lambert diffuse + Blinn-Phong
    // specular + a subtle rim term. One fixed key light, everything else
    // is per-material data from the push constants, so distinct materials
    // separate by hue, brightness gradient, highlight, and silhouette rim
    // while the draw stays a single cheap pass. Because the pipeline
    // writes depth and culls back faces, a face is only ever shaded when
    // it is the nearest visible surface — light and color cannot bleed
    // through solid geometry.
    vec3 normal = normalize(fragNormal);
    if (!gl_FrontFacing) {
        normal = -normal; // double-sided materials light their back faces correctly
    }
    vec3 lightDirection = normalize(vec3(0.35, 0.8, 0.45)); // toward the key light
    vec3 lightColor = vec3(1.0, 0.96, 0.88);
    vec3 viewDirection = normalize(fragCameraPosition - fragWorldPosition);

    float diffuseFactor = max(dot(normal, lightDirection), 0.0);
    vec3 ambient = fragColor * fragSurface.w;
    vec3 diffuse = fragColor * lightColor * (diffuseFactor * 0.82);

    vec3 halfDirection = normalize(lightDirection + viewDirection);
    float specularFactor = pow(max(dot(normal, halfDirection), 0.0), max(fragSpecular.a, 1.0)) * diffuseFactor;
    vec3 specular = fragSpecular.rgb * lightColor * (specularFactor * fragSurface.x);

    float rimBase = 1.0 - clamp(dot(normal, viewDirection), 0.0, 1.0);
    float rimFactor = pow(rimBase, max(fragSurface.z, 0.1));
    vec3 rim = fragSpecular.rgb * (rimFactor * fragSurface.y * (0.30 + 0.70 * diffuseFactor));

    outColor = vec4(ambient + diffuse + specular + rim, 1.0);
}
