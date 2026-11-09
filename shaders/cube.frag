#version 450

// Per-frame uniform block — byte-identical with the C++
// UniformBufferObject (Renderer.h): camera matrices, cascaded shadow
// matrices, and the packed dynamic-light arrays (Phase 21).
layout(set = 0, binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
    mat4 lightViewProj[4];
    vec4 lightDirection;
    vec4 cascadeSplits;
    vec4 lightCounts; // x point count, y spot count, z total
    vec4 lightPositionRange[16];
    vec4 lightColorIntensity[16];
    vec4 lightDirectionType[16];
    vec4 lightCone[16];
} ubo;

layout(set = 0, binding = 1) uniform sampler2D shadowMaps[4];

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec3 fragWorldPosition;
layout(location = 3) in vec3 fragCameraPosition;
layout(location = 4) in vec4 fragSpecular;
layout(location = 5) in vec4 fragSurface;
layout(location = 6) in float viewDepth;

layout(location = 0) out vec4 outColor;

// Picks the cascade that owns this fragment's view depth: the first
// slice whose far plane is at or beyond the fragment.
int cascadeIndexFor(float depth) {
    if (depth <= ubo.cascadeSplits.x) return 0;
    if (depth <= ubo.cascadeSplits.y) return 1;
    if (depth <= ubo.cascadeSplits.z) return 2;
    return 3;
}

// Directional shadow lookup with a 3x3 PCF kernel against one
// cascade. Returns 1.0 when the fragment is lit, 0.0 when fully
// shadowed. Fragments outside the light frustum are treated as lit,
// and the slope-scaled bias keeps surfaces that face the light from
// shadowing themselves (acne), complementing the shadow pass's
// front-face culling and depth bias.
float shadowFactor(vec3 worldPosition, vec3 normal, vec3 lightDirection, float depth) {
    int cascade = cascadeIndexFor(depth);
    // Small normal offset pushes the sample point off the surface
    // along its normal, the standard cure for acne on grazing faces.
    vec4 lightPosition = ubo.lightViewProj[cascade] * vec4(worldPosition + normal * 0.02, 1.0);
    vec3 projected = lightPosition.xyz / lightPosition.w;
    vec2 uv = projected.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || projected.z > 1.0 || projected.z < 0.0) {
        return 1.0;
    }
    float facing = clamp(dot(normal, lightDirection), 0.0, 1.0);
    float bias = max(0.0016 * (1.0 - facing), 0.0004);
    float receiverDepth = projected.z - bias;
    vec2 texelSize = vec2(1.0) / vec2(textureSize(shadowMaps[cascade], 0));
    float shadow = 0.0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            float occluderDepth = texture(shadowMaps[cascade], uv + vec2(x, y) * texelSize).r;
            shadow += receiverDepth > occluderDepth ? 0.0 : 1.0;
        }
    }
    return shadow / 9.0;
}

// Smooth windowed inverse-square attenuation — mirrors the CPU-side
// lightAttenuation() exactly (LightingSystem.h).
float attenuation(float dist, float range) {
    if (range <= 0.0) return 0.0;
    float ratio = clamp(dist / range, 0.0, 1.0);
    float window = clamp(1.0 - ratio * ratio * ratio * ratio, 0.0, 1.0);
    return (window * window) / (dist * dist + 1.0);
}

void main() {
    // Lighting model: ambient + Lambert diffuse + Blinn-Phong
    // specular + a subtle rim term. The directional key light is
    // attenuated by its cascade shadow; dynamic point/spot lights
    // add unshadowed local terms. Per-material data arrives through
    // push constants, so distinct materials separate by hue,
    // brightness gradient, highlight, and silhouette rim while the
    // draw stays a single cheap pass. Because the pipeline writes
    // depth and culls back faces, a face is only ever shaded when it
    // is the nearest visible surface — light and color cannot bleed
    // through solid geometry.
    vec3 normal = normalize(fragNormal);
    if (!gl_FrontFacing) {
        normal = -normal; // double-sided materials light their back faces correctly
    }
    vec3 lightDirection = normalize(ubo.lightDirection.xyz); // toward the key light
    vec3 lightColor = vec3(1.0, 0.96, 0.88);
    vec3 viewDirection = normalize(fragCameraPosition - fragWorldPosition);
    float shadow = shadowFactor(fragWorldPosition, normal, lightDirection, viewDepth);

    float diffuseFactor = max(dot(normal, lightDirection), 0.0);
    vec3 ambient = fragColor * fragSurface.w;
    vec3 diffuse = fragColor * lightColor * (diffuseFactor * 0.82) * shadow;

    vec3 halfDirection = normalize(lightDirection + viewDirection);
    float specularFactor = pow(max(dot(normal, halfDirection), 0.0), max(fragSpecular.a, 1.0)) * diffuseFactor;
    vec3 specular = fragSpecular.rgb * lightColor * (specularFactor * fragSurface.x) * shadow;

    float rimBase = 1.0 - clamp(dot(normal, viewDirection), 0.0, 1.0);
    float rimFactor = pow(rimBase, max(fragSurface.z, 0.1));
    vec3 rim = fragSpecular.rgb * (rimFactor * fragSurface.y * (0.30 + 0.70 * diffuseFactor));

    // Dynamic lights: nearest-first packed arrays; type 0 = point,
    // type 1 = spot (cone fade between the packed cosines).
    int lightTotal = int(ubo.lightCounts.z + 0.5);
    for (int i = 0; i < lightTotal; ++i) {
        vec3 lightPosition = ubo.lightPositionRange[i].xyz;
        float range = ubo.lightPositionRange[i].w;
        vec3 toLight = lightPosition - fragWorldPosition;
        float dist = length(toLight);
        float falloff = attenuation(dist, range);
        if (falloff <= 0.0) {
            continue;
        }
        vec3 dirToLight = toLight / max(dist, 1e-4);
        float spotFactor = 1.0;
        if (ubo.lightDirectionType[i].w > 0.5) {
            float cosTheta = dot(-dirToLight, normalize(ubo.lightDirectionType[i].xyz));
            float cosInner = ubo.lightCone[i].x;
            float cosOuter = ubo.lightCone[i].y;
            spotFactor = clamp((cosTheta - cosOuter) / max(cosInner - cosOuter, 1e-4), 0.0, 1.0);
            if (spotFactor <= 0.0) {
                continue;
            }
        }
        vec3 lightRadiance = ubo.lightColorIntensity[i].rgb * (ubo.lightColorIntensity[i].w * falloff * spotFactor);
        float nDotL = max(dot(normal, dirToLight), 0.0);
        diffuse += fragColor * lightRadiance * nDotL;
        vec3 halfVec = normalize(dirToLight + viewDirection);
        float spec = pow(max(dot(normal, halfVec), 0.0), max(fragSpecular.a, 1.0)) * nDotL;
        specular += fragSpecular.rgb * lightRadiance * spec * fragSurface.x;
    }

    outColor = vec4(ambient + diffuse + specular + rim, 1.0);
}
