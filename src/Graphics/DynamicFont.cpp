#include "Graphics/DynamicFont.h"

#include "Graphics/HudFont.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <utility>

// stb_truetype: vendored single-header TrueType rasterizer (see
// third_party/stb). Implementation TU is this one; warnings from the
// third-party header are not ours to fix, so keep them out of the
// engine's zero-warning build without touching the vendored file.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../../third_party/stb/stb_truetype.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#ifndef ENGINE_DEFAULT_FONT_PATH
#define ENGINE_DEFAULT_FONT_PATH ""
#endif

namespace engine {
namespace {

// Neighbouring glyphs keep a 2-texel empty gap and pages a 1-texel
// margin: glyph rects are the exact bitmaps, so a bilinear tap at a
// quad edge lands in zero-coverage texels and no neighbour ink can
// bleed in (no mips exist to widen the footprint -- sampling is 1:1).
constexpr uint32_t kGlyphGapTexels = 2;
constexpr uint32_t kPageMarginTexels = 1;

uint64_t glyphKey(uint32_t codepoint, int pixelSize) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(pixelSize)) << 32) | codepoint;
}

uint64_t kernKey(uint32_t first, uint32_t second, int pixelSize) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(pixelSize)) << 42) |
           (static_cast<uint64_t>(first & 0x1FFFFFu) << 21) | (second & 0x1FFFFFu);
}

std::vector<uint8_t> readWholeFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return {};
    }
    const std::streamoff size = file.tellg();
    if (size <= 0) {
        return {};
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(bytes.data()), size);
    return file ? bytes : std::vector<uint8_t>{};
}

} // namespace

struct DynamicFont::Impl {
    struct SizeMetrics {
        float scale = 1.0f;      // design units -> pixels at this size
        float ascent = 0.0f;     // baseline to line top, pixels
        float descent = 0.0f;    // baseline to line bottom, pixels (<= 0)
        float lineHeight = 0.0f; // baseline to baseline, pixels
    };
    struct Entry {
        FontGlyph glyph{};
        bool resolved = false;   // metrics (advance) computed
        bool rasterized = false; // bitmap packed (or known blank)
    };
    struct Page {
        std::vector<uint8_t> pixels; // pageSize^2 coverage
        uint32_t penX = kPageMarginTexels;
        uint32_t penY = kPageMarginTexels;
        uint32_t shelfHeight = 0;
        uint64_t revision = 0; // 0 = never mutated since creation
    };

    std::vector<uint8_t> bytes; // owned sfnt bytes (face data points here)
    stbtt_fontinfo face{};
    bool hasFace = false;
    std::string name;
    int designAscent = 0;
    int designDescent = 0;
    int designLineGap = 0;

    uint32_t pageSize = kFontPageSize;
    uint32_t pageLimit = kFontTexturePages;
    std::vector<Page> pages;
    std::unordered_map<int, SizeMetrics> sizes;
    std::unordered_map<uint64_t, Entry> glyphs;
    std::unordered_map<uint64_t, float> kerns;
    uint64_t nextRevision = 1; // monotonic forever, survives restarts
    bool resetPending = false;

    [[nodiscard]] int clampSize(int pixelSize) const {
        return std::clamp(pixelSize, kMinFontPixelSize, kMaxFontPixelSize);
    }

    const SizeMetrics& metricsFor(int pixelSize) {
        const int size = clampSize(pixelSize);
        const auto found = sizes.find(size);
        if (found != sizes.end()) {
            return found->second;
        }
        SizeMetrics metrics;
        if (hasFace) {
            metrics.scale = stbtt_ScaleForPixelHeight(&face, static_cast<float>(size));
            metrics.ascent = static_cast<float>(designAscent) * metrics.scale;
            metrics.descent = static_cast<float>(designDescent) * metrics.scale;
            metrics.lineHeight =
                static_cast<float>(designAscent - designDescent + designLineGap) * metrics.scale;
        } else {
            // Synthetic 5x7 face: whole pattern blocks per em slice.
            const float block = static_cast<float>(std::max(1, size / 8));
            metrics.scale = block;
            metrics.ascent = 7.0f * block;
            metrics.descent = 0.0f;
            metrics.lineHeight = 9.0f * block;
        }
        return sizes.emplace(size, metrics).first->second;
    }

    // Shelf-packs one coverage bitmap into the atlas. False when no
    // page can take it (caller degrades to '?' and flags a restart).
    bool packBitmap(int width, int height, const uint8_t* bitmap, FontGlyph& glyph) {
        if (width <= 0 || height <= 0 || bitmap == nullptr) {
            return false;
        }
        const uint32_t w = static_cast<uint32_t>(width);
        const uint32_t h = static_cast<uint32_t>(height);
        if (w + 2 * kPageMarginTexels > pageSize || h + 2 * kPageMarginTexels > pageSize) {
            return false; // a single glyph larger than a whole page
        }
        if (pages.empty()) {
            pages.emplace_back();
            pages.back().pixels.assign(static_cast<size_t>(pageSize) * pageSize, 0);
        }
        Page* page = &pages.back();
        if (page->penX + w + kPageMarginTexels > pageSize) {
            page->penX = kPageMarginTexels;
            page->penY += page->shelfHeight + kGlyphGapTexels;
            page->shelfHeight = 0;
        }
        if (page->penY + h + kPageMarginTexels > pageSize) {
            if (pages.size() >= pageLimit) {
                resetPending = true;
                return false;
            }
            pages.emplace_back();
            pages.back().pixels.assign(static_cast<size_t>(pageSize) * pageSize, 0);
            page = &pages.back();
        }
        for (uint32_t row = 0; row < h; ++row) {
            std::memcpy(&page->pixels[(page->penY + row) * pageSize + page->penX],
                        bitmap + static_cast<size_t>(row) * w, w);
        }
        glyph.page = static_cast<uint32_t>(pages.size() - 1);
        glyph.u0 = static_cast<float>(page->penX) / static_cast<float>(pageSize);
        glyph.v0 = static_cast<float>(page->penY) / static_cast<float>(pageSize);
        glyph.u1 = static_cast<float>(page->penX + w) / static_cast<float>(pageSize);
        glyph.v1 = static_cast<float>(page->penY + h) / static_cast<float>(pageSize);
        page->penX += w + kGlyphGapTexels;
        page->shelfHeight = std::max(page->shelfHeight, h);
        page->revision = nextRevision++;
        return true;
    }

    // Resolves one codepoint at one size; nullptr when the face has
    // no such glyph, or when rasterization was requested but the
    // atlas could not hold the bitmap (restart flagged). Entries
    // are cached per (codepoint, size); metrics-only entries carry
    // the advance and are upgraded in place when ink is requested.
    const FontGlyph* tryGlyph(uint32_t codepoint, int pixelSize, bool rasterize) {
        const SizeMetrics& metrics = metricsFor(pixelSize);
        const uint64_t key = glyphKey(codepoint, clampSize(pixelSize));
        const auto found = glyphs.find(key);
        if (found != glyphs.end() && (!rasterize || found->second.rasterized)) {
            return &found->second.glyph;
        }

        int glyphIndex = 0;
        if (hasFace) {
            glyphIndex = stbtt_FindGlyphIndex(&face, static_cast<int>(codepoint));
            if (glyphIndex == 0) {
                return nullptr; // face has no such glyph
            }
        }
        Entry& entry = glyphs[key];
        FontGlyph& glyph = entry.glyph;
        if (!entry.resolved) {
            entry.resolved = true;
            if (hasFace) {
                int advanceWidth = 0;
                int leftSideBearing = 0;
                stbtt_GetGlyphHMetrics(&face, glyphIndex, &advanceWidth, &leftSideBearing);
                glyph.advance = static_cast<float>(advanceWidth) * metrics.scale;
            } else {
                glyph.advance = 6.0f * metrics.scale; // 5 cells + 1 spacing
            }
        }
        if (!rasterize || entry.rasterized) {
            return &glyph;
        }
        entry.rasterized = true;

        if (hasFace) {
            int width = 0;
            int height = 0;
            int xoff = 0;
            int yoff = 0;
            unsigned char* bitmap = stbtt_GetCodepointBitmap(&face, 0.0f, metrics.scale,
                                                             static_cast<int>(codepoint), &width,
                                                             &height, &xoff, &yoff);
            if (bitmap != nullptr && width > 0 && height > 0) {
                glyph.bearingX = static_cast<float>(xoff);
                glyph.bearingY = static_cast<float>(yoff);
                glyph.width = static_cast<float>(width);
                glyph.height = static_cast<float>(height);
                const bool packed = packBitmap(width, height, bitmap, glyph);
                stbtt_FreeBitmap(bitmap, nullptr);
                if (!packed) {
                    glyph.width = 0.0f;
                    glyph.height = 0.0f;
                    entry.rasterized = false; // retry after the restart
                    return nullptr; // atlas full: caller falls back
                }
            }
            return &glyph;
        }

        // Synthetic 5x7 face: scale the pattern into coverage blocks.
        const char character = (codepoint >= kFontFirstChar && codepoint <= kFontLastChar)
                                   ? static_cast<char>(codepoint)
                                   : '?';
        const HudGlyph& rows = hudGlyphFor(character);
        bool anyInk = false;
        for (const auto& row : rows) {
            for (const char cell : row) {
                anyInk = anyInk || cell == '#';
            }
        }
        if (!anyInk) {
            return &glyph; // blank (space): advance only
        }
        const int block = std::max(1, static_cast<int>(metrics.scale));
        const int width = static_cast<int>(kHudGlyphWidth) * block;
        const int height = static_cast<int>(kHudGlyphHeight) * block;
        std::vector<uint8_t> bitmap(static_cast<size_t>(width) * static_cast<size_t>(height), 0);
        for (int gy = 0; gy < static_cast<int>(kHudGlyphHeight); ++gy) {
            for (int gx = 0; gx < static_cast<int>(kHudGlyphWidth); ++gx) {
                if (rows[static_cast<size_t>(gy)][static_cast<size_t>(gx)] != '#') {
                    continue;
                }
                for (int ty = 0; ty < block; ++ty) {
                    for (int tx = 0; tx < block; ++tx) {
                        bitmap[static_cast<size_t>(gy * block + ty) * static_cast<size_t>(width) +
                               static_cast<size_t>(gx * block + tx)] = 255;
                    }
                }
            }
        }
        glyph.bearingX = 0.0f;
        glyph.bearingY = -static_cast<float>(height); // pattern sits on the baseline
        glyph.width = static_cast<float>(width);
        glyph.height = static_cast<float>(height);
        if (!packBitmap(width, height, bitmap.data(), glyph)) {
            glyph.width = 0.0f;
            glyph.height = 0.0f;
            entry.rasterized = false; // retry after the restart
            return nullptr;
        }
        return &glyph;
    }
};

DynamicFont::DynamicFont() : m_impl(std::make_unique<Impl>()) {}

DynamicFont::DynamicFont(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

DynamicFont::~DynamicFont() = default;
DynamicFont::DynamicFont(DynamicFont&&) noexcept = default;
DynamicFont& DynamicFont::operator=(DynamicFont&&) noexcept = default;

DynamicFont DynamicFont::fromBytes(std::vector<uint8_t> bytes, std::string name, uint32_t pageSize,
                                   uint32_t pageCount) {
    auto impl = std::make_unique<Impl>();
    impl->name = std::move(name);
    impl->pageSize = std::max(16u, pageSize);
    impl->pageLimit = std::clamp(pageCount, 1u, kFontTexturePages);

    // stb_truetype does no bounds checking on untrusted data (its own
    // header warns as much), so reject anything that does not carry a
    // plausible sfnt header before the parser ever sees it: TrueType
    // (0x00010000 / 'true'), OpenType-CFF ('OTTO') or a collection
    // ('ttcf', whose first font stb can locate via its offset table).
    bool plausible = false;
    if (bytes.size() >= 12) {
        const uint32_t magic = (static_cast<uint32_t>(bytes[0]) << 24) |
                               (static_cast<uint32_t>(bytes[1]) << 16) |
                               (static_cast<uint32_t>(bytes[2]) << 8) |
                               static_cast<uint32_t>(bytes[3]);
        plausible = magic == 0x00010000u || magic == 0x74727565u /*'true'*/ ||
                    magic == 0x4F54544Fu /*'OTTO'*/ || magic == 0x74746366u /*'ttcf'*/;
    }
    if (plausible) {
        impl->bytes = std::move(bytes);
        if (stbtt_InitFont(&impl->face, impl->bytes.data(),
                           stbtt_GetFontOffsetForIndex(impl->bytes.data(), 0)) != 0) {
            impl->hasFace = true;
            stbtt_GetFontVMetrics(&impl->face, &impl->designAscent, &impl->designDescent,
                                  &impl->designLineGap);
        } else {
            impl->bytes.clear();
            impl->bytes.shrink_to_fit();
        }
    }
    return DynamicFont(std::move(impl));
}

const FontGlyph& DynamicFont::glyphFor(uint32_t codepoint, int pixelSize) {
    const int size = m_impl->clampSize(pixelSize);
    if (const FontGlyph* glyph = m_impl->tryGlyph(codepoint, size, true)) {
        return *glyph;
    }
    if (codepoint != static_cast<uint32_t>('?')) {
        if (const FontGlyph* glyph = m_impl->tryGlyph(static_cast<uint32_t>('?'), size, true)) {
            return *glyph;
        }
    }
    // Every real face advances a space; this resolves even for
    // faces whose ink is entirely missing. The last-resort glyph
    // (blank) exists only so a pathological face cannot crash the
    // typesetter.
    if (const FontGlyph* space = m_impl->tryGlyph(static_cast<uint32_t>(' '), size, false)) {
        return *space;
    }
    static const FontGlyph kLastResort{};
    return kLastResort;
}

float DynamicFont::advanceFor(uint32_t codepoint, int pixelSize) {
    const int size = m_impl->clampSize(pixelSize);
    if (const FontGlyph* glyph = m_impl->tryGlyph(codepoint, size, false)) {
        return glyph->advance;
    }
    if (codepoint != static_cast<uint32_t>('?')) {
        if (const FontGlyph* glyph = m_impl->tryGlyph(static_cast<uint32_t>('?'), size, false)) {
            return glyph->advance;
        }
    }
    const FontGlyph* space = m_impl->tryGlyph(static_cast<uint32_t>(' '), size, false);
    return space != nullptr ? space->advance : 0.0f;
}

float DynamicFont::kernFor(uint32_t first, uint32_t second, int pixelSize) {
    Impl& impl = *m_impl;
    if (!impl.hasFace || first == 0 || second == 0) {
        return 0.0f;
    }
    const int size = impl.clampSize(pixelSize);
    const uint64_t key = kernKey(first, second, size);
    const auto found = impl.kerns.find(key);
    if (found != impl.kerns.end()) {
        return found->second;
    }
    const float kern = static_cast<float>(stbtt_GetCodepointKernAdvance(
                           &impl.face, static_cast<int>(first), static_cast<int>(second))) *
                       impl.metricsFor(size).scale;
    impl.kerns.emplace(key, kern);
    return kern;
}

float DynamicFont::ascent(int pixelSize) { return m_impl->metricsFor(pixelSize).ascent; }
float DynamicFont::descent(int pixelSize) { return m_impl->metricsFor(pixelSize).descent; }
float DynamicFont::lineHeight(int pixelSize) { return m_impl->metricsFor(pixelSize).lineHeight; }

void DynamicFont::beginFrame() {
    Impl& impl = *m_impl;
    if (!impl.resetPending) {
        return;
    }
    // The atlas filled beyond repair: start over empty. Glyphs
    // re-rasterize on demand; sizes' metrics are size-derived, not
    // atlas-derived, and survive. Page revisions stay monotonic so
    // the GPU mirror re-uploads the fresh pages.
    impl.pages.clear();
    impl.glyphs.clear();
    impl.kerns.clear();
    impl.resetPending = false;
}

uint32_t DynamicFont::pageSize() const { return m_impl->pageSize; }
uint32_t DynamicFont::pageCount() const { return static_cast<uint32_t>(m_impl->pages.size()); }

const std::vector<uint8_t>& DynamicFont::pagePixels(uint32_t page) const {
    static const std::vector<uint8_t> kEmpty;
    return page < m_impl->pages.size() ? m_impl->pages[page].pixels : kEmpty;
}

uint64_t DynamicFont::pageRevision(uint32_t page) const {
    return page < m_impl->pages.size() ? m_impl->pages[page].revision : 0;
}

const std::string& DynamicFont::fontName() const { return m_impl->name; }
bool DynamicFont::trueType() const { return m_impl->hasFace; }
size_t DynamicFont::cachedGlyphCount() const { return m_impl->glyphs.size(); }
size_t DynamicFont::cachedSizeCount() const { return m_impl->sizes.size(); }

std::vector<std::string> uiFontCandidatePaths() {
    std::vector<std::string> paths;
    if (const char* overridePath = std::getenv("ENGINE_UI_FONT"); overridePath != nullptr && *overridePath != '\0') {
        paths.emplace_back(overridePath);
    }
#if defined(_WIN32)
    // Segoe UI is the Windows UI typeface; Arial is the universal
    // fallback on installs without it.
    paths.emplace_back("C:/Windows/Fonts/segoeui.ttf");
    paths.emplace_back("C:/Windows/Fonts/arial.ttf");
    paths.emplace_back("C:/Windows/Fonts/calibri.ttf");
#elif defined(__APPLE__)
    paths.emplace_back("/System/Library/Fonts/Helvetica.ttc");
    paths.emplace_back("/System/Library/Fonts/Supplemental/Arial.ttf");
#else
    paths.emplace_back("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    paths.emplace_back("/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf");
    paths.emplace_back("/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf");
    paths.emplace_back("/usr/share/fonts/TTF/DejaVuSans.ttf");
#endif
    // Bundled fallback: the build bakes the source-tree copy, and a
    // deployed editor finds it under its copied assets directory.
    if constexpr (sizeof(ENGINE_DEFAULT_FONT_PATH) > 1) {
        paths.emplace_back(ENGINE_DEFAULT_FONT_PATH);
    }
    paths.emplace_back("assets/fonts/DejaVuSans.ttf");
    paths.emplace_back("../assets/fonts/DejaVuSans.ttf");
    return paths;
}

DynamicFont createUiFont() {
    for (const std::string& candidate : uiFontCandidatePaths()) {
        std::vector<uint8_t> bytes = readWholeFile(candidate);
        if (bytes.empty()) {
            continue;
        }
        DynamicFont font = DynamicFont::fromBytes(std::move(bytes), candidate);
        if (font.trueType()) {
            return font;
        }
    }
    return DynamicFont{};
}

} // namespace engine
