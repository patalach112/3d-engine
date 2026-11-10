#pragma once
#include "Core/Export.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace engine {

// ---------------------------------------------------------------------------
// SDF font atlas (Phase 26 Step 2)
//
// Converts the built-in 5x7 bitmap font data (HudFont) into a
// signed-distance-field texture atlas: each glyph is rasterized
// into its own atlas cell and an exact signed distance transform
// (in texels) is baked into an 8-bit page (128 = glyph edge,
// >128 inside, <128 outside, clamped at kSdfSpread texels). The
// fragment shader thresholds the field with a derivative-based
// band, so text stays crisp at any scale from one small texture —
// no per-size rasterization, no mip bleeding (linear filtering of
// an SDF is well-behaved inside the spread band).
//
// Layout is monospace, matching the source font: fixed advance and
// line height in font pixels (1 font pixel = kSdfGlyphScale
// texels). The atlas is one page of 16x6 cells; all 95 printable
// ASCII glyphs fit. saveSdfFontAtlas/loadSdfFontAtlas round-trip
// the baked page through a small binary blob (magic "ESDF"), so a
// future asset pipeline can cache generated atlases to disk
// instead of regenerating them at startup.
// ---------------------------------------------------------------------------

inline constexpr uint32_t kSdfGlyphScale = 4;   // texels per font pixel
inline constexpr uint32_t kSdfCellWidth = 24;   // texels (6 font pixels)
inline constexpr uint32_t kSdfCellHeight = 32;  // texels (8 font pixels)
inline constexpr uint32_t kSdfSpread = 6;       // SDF clamp distance, texels
inline constexpr uint32_t kSdfFirstChar = 32;   // space
inline constexpr uint32_t kSdfLastChar = 126;   // '~'
inline constexpr uint32_t kSdfGlyphCount = kSdfLastChar - kSdfFirstChar + 1;

struct SdfGlyph {
    float u0 = 0.0f; // atlas cell rect, normalized 0..1
    float v0 = 0.0f;
    float u1 = 0.0f;
    float v1 = 0.0f;
    float advance = 0.0f; // font pixels
};

struct SdfFontAtlas {
    uint32_t width = 0;  // page size, texels
    uint32_t height = 0;
    std::vector<uint8_t> pixels; // width*height, R8 SDF values
    std::array<SdfGlyph, kSdfGlyphCount> glyphs{};

    float advance = 6.0f;    // monospace advance, font pixels
    float lineHeight = 10.0f; // font pixels
    float cellWidth = 6.0f;  // cell size in font pixels
    float cellHeight = 8.0f;
    float cellInset = 0.5f;  // glyph box offset inside the cell, font pixels

    [[nodiscard]] bool empty() const { return pixels.empty() || width == 0 || height == 0; }
    // Glyph for a character; anything outside printable ASCII maps
    // to '?', mirroring the bitmap font's fallback. The explicit
    // export is load-bearing: this out-of-line member is consumed
    // across the DLL boundary (UIRenderer internally, the test
    // suite directly), and — unlike everything else the tests
    // import — it previously carried no *_API annotation, so
    // MinGW produced no export for it and engine-core-tests
    // failed to link (undefined reference to glyphFor(char)).
    [[nodiscard]] ENGINE_GRAPHICS_API const SdfGlyph& glyphFor(char character) const;
};

// Bakes the built-in font data into an SDF atlas page (CPU only).
[[nodiscard]] ENGINE_GRAPHICS_API SdfFontAtlas generateSdfFontAtlas();

// Serializes a baked atlas (header + glyph table + page pixels).
[[nodiscard]] ENGINE_GRAPHICS_API std::vector<uint8_t> saveSdfFontAtlas(const SdfFontAtlas& atlas);
// Loads a blob produced by saveSdfFontAtlas. Returns false (and
// leaves `out` untouched) on bad magic, version or truncation.
ENGINE_GRAPHICS_API bool loadSdfFontAtlas(std::span<const uint8_t> blob, SdfFontAtlas& out);

} // namespace engine
