#pragma once
#include "Core/Export.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace engine {

// ---------------------------------------------------------------------------
// UI font atlas (TrueType SDF)
//
// The editor UI is typeset from a real sans-serif TrueType font
// (Segoe UI on Windows, Liberation/Noto/DejaVu Sans on Linux and
// macOS, with the bundled assets/fonts/DejaVuSans.ttf as the
// always-available fallback) rasterized with stb_truetype. Each glyph
// is baked at a large pixel size (48 px em) into a signed-distance
// field, so the existing derivative-threshold text shader stays crisp
// from 10 px labels to full-screen titles, with:
//
//   * mixed case ('a' and 'A' are different glyphs),
//   * proportional advances and pairwise kerning,
//   * true baseline layout (ascent/descent/line gap from the font's
//     own vertical metrics), so descenders and capitals align the way
//     the typeface designed them to.
//
// Metrics are reported in "font units" where the em is
// kUiFontUnitsPerEm (10): drawText(scale = 1) therefore produces a
// ~10 px em, and the UI's customary scale of 1.5 produces the ~15 px
// text the panels are laid out for. Glyph quads are per-glyph sized
// (width/height/bearing), never fixed cells.
//
// The old 5x7 bitmap-derived bake survives only as a last-resort
// fallback (generateBitmapFallbackAtlas) for a machine with no
// readable font at all; generateSdfFontAtlas() prefers TrueType and
// every supported platform finds one.
// ---------------------------------------------------------------------------

inline constexpr uint32_t kSdfFirstChar = 32;  // space
inline constexpr uint32_t kSdfLastChar = 126;  // '~'
inline constexpr uint32_t kSdfGlyphCount = kSdfLastChar - kSdfFirstChar + 1;
inline constexpr float kUiFontUnitsPerEm = 10.0f; // font units per em

// Legacy bitmap-bake geometry, retained for the fallback path and
// for save/load compatibility checks.
inline constexpr uint32_t kSdfGlyphScale = 4;  // texels per font pixel
inline constexpr uint32_t kSdfCellWidth = 24;  // texels (6 font pixels)
inline constexpr uint32_t kSdfCellHeight = 32; // texels (8 font pixels)
inline constexpr uint32_t kSdfSpread = 6;      // SDF clamp distance, texels

struct SdfGlyph {
    float u0 = 0.0f; // atlas rect, normalized 0..1
    float v0 = 0.0f;
    float u1 = 0.0f;
    float v1 = 0.0f;
    float advance = 0.0f; // pen advance, font units (proportional)
    // TrueType layout (font units). bearingX is the offset from the
    // pen to the quad's left edge; bearingY is the offset from the
    // baseline to the quad's top edge (negative above the baseline);
    // width/height are the quad's size. Blank glyphs (space) have a
    // zero size and emit no quad.
    float bearingX = 0.0f;
    float bearingY = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct SdfFontAtlas {
    uint32_t width = 0;  // page size, texels
    uint32_t height = 0;
    std::vector<uint8_t> pixels; // width*height, R8 SDF values (128 = edge)
    std::array<SdfGlyph, kSdfGlyphCount> glyphs{};

    float advance = 6.0f;    // nominal advance (legacy monospace value)
    float lineHeight = 12.0f; // baseline-to-baseline, font units
    float ascent = 8.0f;     // baseline to line top, font units
    float descent = -2.0f;   // baseline to line bottom, font units
    // Pairwise kerning in font units for ASCII pairs (0 when the font
    // has no kern entry). Indexed [first - 32][second - 32].
    std::array<std::array<float, kSdfGlyphCount>, kSdfGlyphCount> kerning{};
    std::string fontName; // human-readable source ("" for the bitmap fallback)
    bool trueType = false;

    // Legacy cell metrics (bitmap fallback layout); unused by the
    // TrueType text path except for serialization.
    float cellWidth = 6.0f;
    float cellHeight = 8.0f;
    float cellInset = 0.5f;

    [[nodiscard]] bool empty() const { return pixels.empty() || width == 0 || height == 0; }
    // Glyph for a character; anything outside printable ASCII maps
    // to '?'. Case is significant: 'a' and 'A' are distinct glyphs.
    // The explicit export is load-bearing on Windows/MinGW: this
    // out-of-line member is consumed across the DLL boundary (the
    // test suite links it directly), so it must carry the module
    // API annotation like every other cross-DLL symbol.
    [[nodiscard]] ENGINE_GRAPHICS_API const SdfGlyph& glyphFor(char character) const;
    // Kerning adjustment between two adjacent characters, font units.
    [[nodiscard]] float kernFor(char first, char second) const {
        const auto a = static_cast<uint32_t>(static_cast<unsigned char>(first));
        const auto b = static_cast<uint32_t>(static_cast<unsigned char>(second));
        if (a < kSdfFirstChar || a > kSdfLastChar || b < kSdfFirstChar || b > kSdfLastChar) {
            return 0.0f;
        }
        return kerning[a - kSdfFirstChar][b - kSdfFirstChar];
    }
};

// The atlas the UI actually uses: a TrueType bake when any candidate
// font can be read (system sans-serif first, bundled fallback last),
// otherwise the bitmap fallback below.
[[nodiscard]] ENGINE_GRAPHICS_API SdfFontAtlas generateSdfFontAtlas();

// Bakes a TrueType/OpenType font held in memory into an SDF atlas at
// the given em pixel size (higher = crisper, larger page). Returns an
// empty atlas when the bytes are not a usable font.
[[nodiscard]] ENGINE_GRAPHICS_API SdfFontAtlas generateTrueTypeFontAtlas(std::span<const uint8_t> fontBytes,
                                                                         float pixelHeight = 48.0f);

// Last-resort bake of the built-in 5x7 bitmap font (the pre-TrueType
// behaviour, monospace all-caps). Only used when no font file exists.
[[nodiscard]] ENGINE_GRAPHICS_API SdfFontAtlas generateBitmapFallbackAtlas();

// Candidate font files in priority order for this platform (system
// sans-serifs, then the engine's bundled fallback). Exposed so hosts
// and tests can report which typography the UI will use.
[[nodiscard]] ENGINE_GRAPHICS_API std::vector<std::string> uiFontCandidatePaths();

// Serializes a baked atlas (header + glyph table + kerning + page).
[[nodiscard]] ENGINE_GRAPHICS_API std::vector<uint8_t> saveSdfFontAtlas(const SdfFontAtlas& atlas);
// Loads a blob produced by saveSdfFontAtlas. Returns false (and
// leaves `out` untouched) on bad magic, version or truncation.
ENGINE_GRAPHICS_API bool loadSdfFontAtlas(std::span<const uint8_t> blob, SdfFontAtlas& out);

} // namespace engine
