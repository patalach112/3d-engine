#version 450

// Metallic-roughness PBR (Phase 22): Cook-Torrance microfacet BRDF
// (GGX distribution, Smith geometry, Schlick Fresnel) for the key
// directional light (cascaded-shadowed) and up to 16 dynamic
// point/spot lights, plus split-sum image-based lighting from the
// prefiltered environment. Mirrors the CPU reference in
// Graphics/Pbr.h formula for formula. Output is linear HDR; tone
// mapping + sRGB happen in the composite pass.

layout(set = 0, binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
    mat4 lightViewProj[4];
    vec4 lightDirection; // xyz toward the key light, w radiance scale
    vec4 cascadeSplits;
    vec4 lightCounts; // x point count, y spot count, z total
    vec4 lightPositionRange[16];
    vec4 lightColorIntensity[16];
    vec4 lightDirectionType[16];
    vec4 lightCone[16];
    vec4 environmentParams; // x intensity, y prefilter max LOD, z skybox on
    // Phase 32 appended fields (same offsets as Renderer.h).
    vec4 directionalColor; // rgb key light tint, w cascade shadows on/off
    mat4 spotShadowViewProj; // shadow-casting spot's light space
    vec4 spotShadowParams; // x packed light index owning the spot shadow (-1 none)
} ubo;

layout(set = 0, binding = 1) uniform sampler2D shadowMaps[4];
layout(set = 0, binding = 7) uniform sampler2D spotShadowMap;
layout(set = 0, binding = 2) uniform sampler2D materialTextures[32];
layout(set = 0, binding = 3) uniform samplerCube irradianceMap;
layout(set = 0, binding = 4) uniform samplerCube prefilteredMap;
layout(set = 0, binding = 5) uniform sampler2D brdfLut;

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec3 fragWorldPosition;
layout(location = 3) in vec3 fragCameraPosition;
layout(location = 4) in vec3 fragPbrParams;
layout(location = 5) flat in ivec4 fragTextureIndices;
layout(location = 6) in float viewDepth;
layout(location = 7) in vec2 fragTexCoord;

layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;

// --- Cook-Torrance terms (mirror Graphics/Pbr.h) -------------------
float distributionGGX(float noH, float roughness) {
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float denom = noH * noH * (alpha2 - 1.0) + 1.0;
    return alpha2 / (PI * denom * denom);
}

float geometrySchlickGGX(float noX, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return noX / (noX * (1.0 - k) + k);
}

float geometrySmith(float noV, float noL, float roughness) {
    return geometrySchlickGGX(noV, roughness) * geometrySchlickGGX(noL, roughness);
}

vec3 fresnelSchlick(float cosTheta, vec3 f0) {
    float m = clamp(1.0 - cosTheta, 0.0, 1.0);
    float m2 = m * m;
    return f0 + (vec3(1.0) - f0) * (m2 * m2 * m);
}

vec3 fresnelSchlickRoughness(float cosTheta, vec3 f0, float roughness) {
    float m = clamp(1.0 - cosTheta, 0.0, 1.0);
    float m2 = m * m;
    return f0 + (max(vec3(1.0 - roughness), f0) - f0) * (m2 * m2 * m);
}

// Outgoing radiance for one light (radiance already includes color,
// intensity, attenuation and the spot cone factor).
vec3 evaluateDirectLight(vec3 normal, vec3 viewDir, vec3 lightDir, vec3 radiance, vec3 albedo, float metallic,
                         float roughness) {
    vec3 halfVec = normalize(lightDir + viewDir);
    float noL = clamp(dot(normal, lightDir), 0.0, 1.0);
    float noV = clamp(dot(normal, viewDir), 0.0, 1.0);
    float noH = clamp(dot(normal, halfVec), 0.0, 1.0);
    float voH = clamp(dot(viewDir, halfVec), 0.0, 1.0);
    if (noL <= 0.0 || noV <= 0.0) {
        return vec3(0.0);
    }
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 fresnel = fresnelSchlick(voH, f0);
    float distribution = distributionGGX(noH, roughness);
    float geometry = geometrySmith(noV, noL, roughness);
    vec3 specular = (distribution * geometry) * fresnel / (4.0 * noV * noL);
    vec3 diffuseWeight = (vec3(1.0) - fresnel) * (1.0 - metallic);
    return (diffuseWeight * albedo / PI + specular) * radiance * noL;
}

// --- Cascaded shadows (Phase 21) ------------------------------------
int cascadeIndexFor(float depth) {
    if (depth <= ubo.cascadeSplits.x) return 0;
    if (depth <= ubo.cascadeSplits.y) return 1;
    if (depth <= ubo.cascadeSplits.z) return 2;
    return 3;
}

float shadowFactor(vec3 worldPosition, vec3 normal, vec3 lightDirection, float depth) {
    int cascade = cascadeIndexFor(depth);
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

// --- Spot shadows (Phase 32): perspective reproject + 3x3 PCF --
// Mirrors the cascade path, but the light space is a perspective
// frustum (the spot cone), so the divide does the cone falloff.
float spotShadowFactor(vec3 worldPosition, vec3 normal, vec3 lightDirection) {
    vec4 lightPosition = ubo.spotShadowViewProj * vec4(worldPosition + normal * 0.02, 1.0);
    vec3 projected = lightPosition.xyz / lightPosition.w;
    vec2 uv = projected.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || projected.z > 1.0 || projected.z < 0.0) {
        return 1.0;
    }
    float facing = clamp(dot(normal, lightDirection), 0.0, 1.0);
    float bias = max(0.002 * (1.0 - facing), 0.0005);
    float receiverDepth = projected.z - bias;
    vec2 texelSize = vec2(1.0) / vec2(textureSize(spotShadowMap, 0));
    float shadow = 0.0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            float occluderDepth = texture(spotShadowMap, uv + vec2(x, y) * texelSize).r;
            shadow += receiverDepth > occluderDepth ? 0.0 : 1.0;
        }
    }
    return shadow / 9.0;
}

// Smooth windowed inverse-square attenuation (mirrors LightingSystem.h).
float attenuation(float dist, float range) {
    if (range <= 0.0) return 0.0;
    float ratio = clamp(dist / range, 0.0, 1.0);
    float window = clamp(1.0 - ratio * ratio * ratio * ratio, 0.0, 1.0);
    return (window * window) / (dist * dist + 1.0);
}

// Tangent-space normal mapping without vertex tangents: build the
// cotangent frame from screen-space derivatives of position and UV.
vec3 perturbNormal(vec3 normal, vec3 worldPosition, vec2 uv, vec3 tangentNormal) {
    vec3 q0 = dFdx(worldPosition);
    vec3 q1 = dFdy(worldPosition);
    vec2 st0 = dFdx(uv);
    vec2 st1 = dFdy(uv);
    vec3 tangent = normalize(q0 * st1.t - q1 * st0.t);
    vec3 bitangent = normalize(-q0 * st1.s + q1 * st0.s);
    return normalize(mat3(tangent, bitangent, normal) * tangentNormal);
}

void main() {
    vec3 albedo = fragColor;
    float metallic = clamp(fragPbrParams.x, 0.0, 1.0);
    float roughness = clamp(fragPbrParams.y, 0.045, 1.0);
    float occlusion = 1.0;

    if (fragTextureIndices.x >= 0) {
        // Albedo maps are authored sRGB; decode to linear.
        albedo *= pow(texture(materialTextures[fragTextureIndices.x], fragTexCoord).rgb, vec3(2.2));
    }
    if (fragTextureIndices.y >= 0) {
        vec3 metallicRoughness = texture(materialTextures[fragTextureIndices.y], fragTexCoord).rgb;
        roughness = clamp(roughness * metallicRoughness.g, 0.045, 1.0);
        metallic = clamp(metallic * metallicRoughness.b, 0.0, 1.0);
    }

    vec3 normal = normalize(fragNormal);
    if (!gl_FrontFacing) {
        normal = -normal;
    }
    if (fragTextureIndices.z >= 0) {
        vec3 tangentNormal = texture(materialTextures[fragTextureIndices.z], fragTexCoord).rgb * 2.0 - 1.0;
        normal = perturbNormal(normal, fragWorldPosition, fragTexCoord, tangentNormal);
    }
    if (fragTextureIndices.w >= 0) {
        occlusion = mix(1.0, texture(materialTextures[fragTextureIndices.w], fragTexCoord).r, fragPbrParams.z);
    }

    vec3 viewDir = normalize(fragCameraPosition - fragWorldPosition);
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 outgoing = vec3(0.0);

    // Key directional light, cascaded-shadowed.
    vec3 lightDirection = normalize(ubo.lightDirection.xyz);
    vec3 keyRadiance = ubo.directionalColor.rgb * ubo.lightDirection.w;
    float shadow = ubo.directionalColor.w > 0.5 ? shadowFactor(fragWorldPosition, normal, lightDirection, viewDepth)
                                                : 1.0;
    outgoing += evaluateDirectLight(normal, viewDir, lightDirection, keyRadiance, albedo, metallic, roughness) *
                shadow;

    // Dynamic point/spot lights (nearest 16, gathered per frame).
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
        vec3 radiance = ubo.lightColorIntensity[i].rgb * (ubo.lightColorIntensity[i].w * falloff * spotFactor);
        if (float(i) == ubo.spotShadowParams.x) {
            radiance *= spotShadowFactor(fragWorldPosition, normal, dirToLight);
        }
        outgoing += evaluateDirectLight(normal, viewDir, dirToLight, radiance, albedo, metallic, roughness);
    }

    // Image-based lighting (split-sum): irradiance for diffuse,
    // roughness-blurred reflection modulated by the BRDF LUT.
    float noV = clamp(dot(normal, viewDir), 0.0, 1.0);
    vec3 fresnel = fresnelSchlickRoughness(noV, f0, roughness);
    vec3 diffuseWeight = (vec3(1.0) - fresnel) * (1.0 - metallic);
    vec3 irradiance = texture(irradianceMap, normal).rgb;
    vec3 diffuseIbl = irradiance * albedo * diffuseWeight;

    vec3 reflection = reflect(-viewDir, normal);
    vec3 prefiltered = textureLod(prefilteredMap, reflection, roughness * ubo.environmentParams.y).rgb;
    vec2 brdf = texture(brdfLut, vec2(noV, roughness)).rg;
    vec3 specularIbl = prefiltered * (fresnel * brdf.x + vec3(brdf.y));

    vec3 ambient = (diffuseIbl + specularIbl) * occlusion * ubo.environmentParams.x;

    outColor = vec4(ambient + outgoing, 1.0);
}
