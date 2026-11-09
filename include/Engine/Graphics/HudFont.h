#pragma once

#include "Core/Export.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace engine {

// ---------------------------------------------------------------------------
// Minimal 5x7 bitmap font + HUD text layout (pure CPU, no Vulkan).
//
// Text is converted into colored triangles in normalized device
// coordinates, ready to upload into a dynamic vertex buffer. Glyphs are
// 5x7 pixels with 1 px spacing; each filled pixel becomes one quad (6
// vertices). Layout happens only when the HUD text changes (a few times
// per second), so the per-frame cost is a single draw of pre-built
// vertices — no textures, no descriptor sets, no per-frame allocation
// once the caller's buffer is warm.
// ---------------------------------------------------------------------------

struct HudVertex {
    float x = 0.0f; // NDC
    float y = 0.0f; // NDC (Vulkan framebuffer space: -1 top, +1 bottom)
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
};

struct HudColor {
    float r = 1.0f;
    float g = 0.9f;
    float b = 0.35f;
    float a = 1.0f;
};

inline constexpr uint32_t kHudGlyphWidth = 5;
inline constexpr uint32_t kHudGlyphHeight = 7;
inline constexpr uint32_t kHudGlyphAdvance = 6;  // glyph + 1 px tracking
inline constexpr uint32_t kHudLineAdvance = 10;  // glyph + 3 px leading

using HudGlyph = std::array<std::string_view, kHudGlyphHeight>;

// Returns the glyph rows for a character ('#' = filled pixel). Lowercase
// maps to uppercase; unknown characters render as '?'.
[[nodiscard]] ENGINE_GRAPHICS_API const HudGlyph& hudGlyphFor(char character);

// Builds triangle vertices for `text` (supports '\n') with the top-left
// corner at (xPixels, yPixels), each font pixel scaled by `pixelScale`
// screen pixels, mapped into the given viewport.
[[nodiscard]] ENGINE_GRAPHICS_API std::vector<HudVertex> buildHudVertices(std::string_view text, float xPixels, float yPixels,
                                                      float pixelScale, uint32_t viewportWidth,
                                                      uint32_t viewportHeight, HudColor color = {});

// Composes the standard engine HUD text (uppercase, font-safe charset).
[[nodiscard]] ENGINE_GRAPHICS_API std::string formatHudText(double framesPerSecond, double frameMilliseconds, uint32_t workerThreads,
                                        uint64_t vramUsedMiB, uint64_t vramBudgetMiB, uint32_t drawnCount,
                                        uint32_t culledCount);

} // namespace engine
