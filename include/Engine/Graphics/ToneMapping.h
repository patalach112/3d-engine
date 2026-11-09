#pragma once

#include <glm/common.hpp>
#include <glm/vec3.hpp>

#include <cmath>

namespace engine {

// ---------------------------------------------------------------------------
// Tone mapping & display encoding (Phase 22) — CPU mirror of
// composite.frag. The scene renders into an HDR target; the final
// composite pass multiplies by exposure, folds in bloom, compresses
// the range with the ACES filmic curve, and encodes to sRGB for the
// (non-HDR) swapchain image.
// ---------------------------------------------------------------------------

// ACES filmic tone mapping approximation (Krzysztof Narkowicz fit):
// maps HDR linear radiance to [0, 1) display range with filmic
// highlight rolloff and slight saturation in the toe.
[[nodiscard]] inline glm::vec3 acesFilmic(const glm::vec3& color) {
    const glm::vec3 x = glm::max(color, glm::vec3{0.0f});
    return glm::clamp((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f), 0.0f, 1.0f);
}

// Exact piecewise sRGB electro-optical transfer (linear -> sRGB).
[[nodiscard]] inline float linearToSrgb(float value) {
    const float v = glm::clamp(value, 0.0f, 1.0f);
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

[[nodiscard]] inline glm::vec3 linearToSrgb(const glm::vec3& color) {
    return glm::vec3{linearToSrgb(color.x), linearToSrgb(color.y), linearToSrgb(color.z)};
}

// Exact piecewise inverse (sRGB -> linear).
[[nodiscard]] inline float srgbToLinear(float value) {
    const float v = glm::clamp(value, 0.0f, 1.0f);
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

[[nodiscard]] inline glm::vec3 srgbToLinear(const glm::vec3& color) {
    return glm::vec3{srgbToLinear(color.x), srgbToLinear(color.y), srgbToLinear(color.z)};
}

// Soft-knee bright-pass for bloom (mirrors bloomDown.frag): keeps
// luminance above the threshold, fading in quadratically across the
// knee band so near-threshold highlights bloom without banding.
[[nodiscard]] inline glm::vec3 extractBright(const glm::vec3& color, float threshold, float knee) {
    const float luminance = glm::dot(color, glm::vec3{0.2126f, 0.7152f, 0.0722f});
    const float soft = glm::clamp((luminance - threshold + knee) / glm::max(2.0f * knee, 1e-5f), 0.0f, 1.0f);
    const float weight = glm::max(soft * soft * glm::max(luminance - threshold, 0.0f), 0.0f) /
                         glm::max(luminance, 1e-5f);
    return color * weight;
}

} // namespace engine
