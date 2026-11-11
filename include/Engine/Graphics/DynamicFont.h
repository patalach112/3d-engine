#pragma once
#include "Core/Export.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {

// ---------------------------------------------------------------------------
// Dynamic UI font (TrueType, exact-size glyph cache)
//
// The editor UI is typeset from a real sans-serif TrueType font
// (Segoe UI on Windows, Liberation/Noto/DejaVu Sans on Linux and
// macOS, with the bundled assets/fonts/DejaVuSans.ttf as the
// always-available fallback). There is no single baked atlas and no
// distance field:
//
//   * Every glyph is rasterized by stb_truetype AT THE EXACT PIXEL
//     SIZE it will be drawn at (fontPixelSizeForScale). A glyph quad
//     is exactly its coverage bitmap in pixels -- texel:pixel is
//     1:1, so coverage sampling can neither magnify (blur) nor
//     minify (shimmer) the ink. Changing window size, UI scale or
//     zoom simply rasterizes at the new size; every size stays
//     pixel-true forever. Sizes are cached side by side, so
//     oscillating between two zoom levels never re-rasterizes.
//   * Glyph quads are emitted on whole pixels (std::round on the
//     origin in UIBatch::drawText, exact integer size), so stems
//     land on pixel boundaries with the coverage the rasterizer
//     computed -- letters never distort, stretch or grey out.
//   * Coverage (0 = no ink, 255 = solid) alpha-blends linearly:
//     filtering preserves ink mass at the edges, unlike a
//     thresholded distance field, whose thin strokes ('i', 'l',
//     '.') wash out below threshold at label sizes. No halo: glyph
//     rects are the exact bitmaps, packed with a 2-texel zero gap
//     so bilinear edge taps never reach a neighbour's ink.
//
// Rasterized glyphs live in a small set of large R8 atlas pages
// (kFontPageSize squared, kFontTexturePages pages, shelf-packed).
// The cache is bounded: if every page fills -- unreachable in
// normal editor use, which needs a few hundred glyphs total --
// new glyphs degrade to '?' for the rest of the frame and the
// cache restarts empty at the next UIBatch::begin, so memory is
// capped without ever corrupting a batch mid-frame.
//
// Metrics are pixels at the rasterized size: proportional advances,
// pairwise kerning, and true baseline layout (ascent/descent/line
// gap from the font's own vertical metrics scaled to the size), so
// descenders and capitals align the way the typeface designed
// them. Any codepoint the face carries can be typeset (the batch
// decodes UTF-8); codepoints the face lacks resolve to '?'.
//
// The built-in 5x7 bitmap font survives as a synthetic fallback
// face (used only when no readable font file exists anywhere), so
// text layout code always has exactly one DynamicFont to talk to.
// ---------------------------------------------------------------------------

// "Font units" remain the UI's scale space: a drawText scale of 1
// typesets a kUiFontUnitsPerEm-pixel em, so every widget's existing
// scale constant keeps its meaning from the fixed-bake era.
inline constexpr float kUiFontUnitsPerEm = 10.0f;

// Pixel-size cache policy. Glyphs rasterize at whole-pixel em
// heights (identity mapping to the snapped quads), floored for
// legibility and capped so a pathological scale cannot demand
// atlas-sized glyphs.
inline constexpr int kMinFontPixelSize = 6;
inline constexpr int kMaxFontPixelSize = 256;
inline constexpr int kReferenceFontPixelSize = 18; // editor label size (scale 1.8)

inline constexpr uint32_t kFontPageSize = 2048;     // texels per atlas page edge
inline constexpr uint32_t kFontTexturePages = 4;    // atlas pages mirrored to the GPU

// The whole-pixel em height a text scale rasterizes (and is laid
// out) at. Layout and rendering share this quantization, so a
// measured string is always the width of the rendered one.
[[nodiscard]] inline int fontPixelSizeForScale(float scale) {
    return std::clamp(static_cast<int>(std::lround(kUiFontUnitsPerEm * scale)), kMinFontPixelSize,
                      kMaxFontPixelSize);
}

// Printable-ASCII range, retained for tests and diagnostics.
inline constexpr uint32_t kFontFirstChar = 32;  // space
inline constexpr uint32_t kFontLastChar = 126;  // '~'

// One rasterized glyph, in pixels at its rasterized size.
struct FontGlyph {
    uint32_t page = 0; // atlas page index (texture slot on the GPU)
    float u0 = 0.0f;   // atlas rect on that page, normalized 0..1
    float v0 = 0.0f;
    float u1 = 0.0f;
    float v1 = 0.0f;
    float advance = 0.0f;  // pen advance, pixels (proportional)
    // Baseline layout, pixels. bearingX is the offset from the pen
    // to the quad's left edge; bearingY from the baseline to the
    // quad's top edge (negative above the baseline); width/height
    // are the quad's size -- exactly the rasterized coverage
    // bitmap, no halo. Blank glyphs (space, zero-size marks) have
    // a zero size and emit no quad.
    float bearingX = 0.0f;
    float bearingY = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

class ENGINE_GRAPHICS_API DynamicFont {
public:
    // A font with no usable TrueType face: the synthetic 5x7
    // fallback face. Never empty, always typesets something.
    DynamicFont();
    ~DynamicFont();
    DynamicFont(DynamicFont&&) noexcept;
    DynamicFont& operator=(DynamicFont&&) noexcept;
    DynamicFont(const DynamicFont&) = delete;
    DynamicFont& operator=(const DynamicFont&) = delete;

    // Builds a font from in-memory sfnt bytes (TTF/OTF/TTC, first
    // face). Unusable bytes degrade to the fallback face, with
    // `name` kept for diagnostics. pageSize/pageCount shrink the
    // atlas for tests; production keeps the defaults (the GPU
    // mirror supports at most kFontTexturePages pages).
    [[nodiscard]] static DynamicFont fromBytes(std::vector<uint8_t> bytes, std::string name,
                                               uint32_t pageSize = kFontPageSize,
                                               uint32_t pageCount = kFontTexturePages);

    // The glyph for a codepoint rasterized at `pixelSize`,
    // rasterizing and caching it on first use. Codepoints the face
    // lacks resolve to the '?' glyph; a face without even that
    // yields a blank space-width glyph. The reference stays valid
    // until the cache restarts (saturation, beginFrame).
    [[nodiscard]] const FontGlyph& glyphFor(uint32_t codepoint, int pixelSize = kReferenceFontPixelSize);
    // Pen advance without rasterizing (measure passes): layout
    // never has to bake ink it may not draw.
    [[nodiscard]] float advanceFor(uint32_t codepoint, int pixelSize = kReferenceFontPixelSize);
    // Kerning adjustment between adjacent codepoints, pixels at
    // `pixelSize` (0 when the face kerns nothing).
    [[nodiscard]] float kernFor(uint32_t first, uint32_t second, int pixelSize = kReferenceFontPixelSize);
    // Vertical metrics, pixels at `pixelSize`.
    [[nodiscard]] float ascent(int pixelSize = kReferenceFontPixelSize);
    [[nodiscard]] float descent(int pixelSize = kReferenceFontPixelSize);
    [[nodiscard]] float lineHeight(int pixelSize = kReferenceFontPixelSize);

    // Frame boundary hook (UIBatch::begin calls this): applies a
    // pending cache restart, so restarts can never invalidate
    // glyph rects a half-built batch already emitted.
    void beginFrame();

    // Atlas pages for the GPU mirror. pagePixels(i) is empty when
    // page i does not exist yet; pageRevision(i) changes on every
    // mutation of page i (monotonic across restarts), which is
    // how the mirror knows a re-upload is due.
    [[nodiscard]] uint32_t pageSize() const;
    [[nodiscard]] uint32_t pageCount() const;
    [[nodiscard]] const std::vector<uint8_t>& pagePixels(uint32_t page) const;
    [[nodiscard]] uint64_t pageRevision(uint32_t page) const;

    [[nodiscard]] const std::string& fontName() const;
    [[nodiscard]] bool trueType() const;
    // Diagnostics: distinct cached (codepoint, size) entries and
    // distinct rasterized sizes currently held.
    [[nodiscard]] size_t cachedGlyphCount() const;
    [[nodiscard]] size_t cachedSizeCount() const;

private:
    struct Impl;
    explicit DynamicFont(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

// The UI's font: the first readable candidate from
// uiFontCandidatePaths() (system sans-serif first, bundled
// fallback last), or the synthetic 5x7 face when no font file
// exists at all. Every supported platform finds a TrueType face.
[[nodiscard]] ENGINE_GRAPHICS_API DynamicFont createUiFont();

// Candidate font files in priority order for this platform.
// Exposed so hosts and tests can report which typography the UI
// will use.
[[nodiscard]] ENGINE_GRAPHICS_API std::vector<std::string> uiFontCandidatePaths();

} // namespace engine
