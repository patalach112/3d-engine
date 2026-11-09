#version 450

layout(set = 0, binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
    mat4 lightViewProj;
    vec4 lightDirection;
} ubo;

layout(set = 0, binding = 1) uniform sampler2D shadowMap;

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec3 fragWorldPosition;
layout(location = 3) in vec3 fragCameraPosition;
layout(location = 4) in vec4 fragSpecular;
layout(location = 5) in vec4 fragSurface;

layout(location = 0) out vec4 outColor;

// Directional shadow lookup with a 3x3 PCF kernel. Returns 1.0 when the
// fragment is lit, 0.0 when fully shadowed. Fragments outside the light
// frustum are treated as lit, and the slope-scaled bias keeps surfaces
// that face the light from shadowing themselves (acne), complementing
// the shadow pass's front-face culling and depth bias.
float shadowFactor(vec3 worldPosition, vec3 normal, vec3 lightDirection) {
    // Small normal offset pushes the sample point off the surface along
    // its normal, the standard cure for acne on grazing faces.
    vec4 lightPosition = ubo.lightViewProj * vec4(worldPosition + normal * 0.02, 1.0);
    vec3 projected = lightPosition.xyz / lightPosition.w;
    vec2 uv = projected.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || projected.z > 1.0) {
        return 1.0;
    }
    float facing = clamp(dot(normal, lightDirection), 0.0, 1.0);
    float bias = max(0.0016 * (1.0 - facing), 0.0004);
    float depth = projected.z - bias;
    vec2 texelSize = vec2(1.0) / vec2(textureSize(shadowMap, 0));
    float shadow = 0.0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            float occluderDepth = texture(shadowMap, uv + vec2(x, y) * texelSize).r;
            shadow += depth > occluderDepth ? 0.0 : 1.0;
        }
    }
    return shadow / 9.0;
}

void main() {
    // Directional lighting model: ambient + Lambert diffuse + Blinn-Phong
    // specular + a subtle rim term, attenuated by the shadow map for the
    // direct terms. One fixed key light, everything else is per-material
    // data from the push constants, so distinct materials separate by
    // hue, brightness gradient, highlight, and silhouette rim while the
    // draw stays a single cheap pass. Because the pipeline writes depth
    // and culls back faces, a face is only ever shaded when it is the
    // nearest visible surface — light and color cannot bleed through
    // solid geometry.
    vec3 normal = normalize(fragNormal);
    if (!gl_FrontFacing) {
        normal = -normal; // double-sided materials light their back faces correctly
    }
    vec3 lightDirection = normalize(ubo.lightDirection.xyz); // toward the key light
    vec3 lightColor = vec3(1.0, 0.96, 0.88);
    vec3 viewDirection = normalize(fragCameraPosition - fragWorldPosition);
    float shadow = shadowFactor(fragWorldPosition, normal, lightDirection);

    float diffuseFactor = max(dot(normal, lightDirection), 0.0);
    vec3 ambient = fragColor * fragSurface.w;
    vec3 diffuse = fragColor * lightColor * (diffuseFactor * 0.82) * shadow;

    vec3 halfDirection = normalize(lightDirection + viewDirection);
    float specularFactor = pow(max(dot(normal, halfDirection), 0.0), max(fragSpecular.a, 1.0)) * diffuseFactor;
    vec3 specular = fragSpecular.rgb * lightColor * (specularFactor * fragSurface.x) * shadow;

    float rimBase = 1.0 - clamp(dot(normal, viewDirection), 0.0, 1.0);
    float rimFactor = pow(rimBase, max(fragSurface.z, 0.1));
    vec3 rim = fragSpecular.rgb * (rimFactor * fragSurface.y * (0.30 + 0.70 * diffuseFactor));

    outColor = vec4(ambient + diffuse + specular + rim, 1.0);
}
