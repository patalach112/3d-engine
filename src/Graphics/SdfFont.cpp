#include "Graphics/SdfFont.h"

#include "Graphics/HudFont.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

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

constexpr uint32_t kAtlasColumns = 16;
constexpr uint32_t kGlyphTexelOffset = 2; // glyph box inset inside a cell, texels

// Exact squared distance from (x, y) to the nearest texel in `targets`.
uint32_t nearestDistanceSquared(uint32_t x, uint32_t y, const std::vector<uint32_t>& targetsX,
                                const std::vector<uint32_t>& targetsY) {
    uint32_t best = 0xFFFFFFFFu;
    for (size_t i = 0; i < targetsX.size(); ++i) {
        const int dx = static_cast<int>(x) - static_cast<int>(targetsX[i]);
        const int dy = static_cast<int>(y) - static_cast<int>(targetsY[i]);
        const uint32_t dist = static_cast<uint32_t>(dx * dx + dy * dy);
        if (dist < best) {
            best = dist;
            if (best <= 1) {
                break; // cannot do better than an adjacent texel
            }
        }
    }
    return best;
}

struct BlobWriter {
    std::vector<uint8_t> bytes;
    template <typename T>
    void write(const T& value) {
        // insert() rather than resize()+memcpy(): GCC 12's
        // -Wstringop-overflow false-fires on the resize idiom at -O2+.
        const auto* first = reinterpret_cast<const uint8_t*>(&value);
        bytes.insert(bytes.end(), first, first + sizeof(T));
    }
    void writeBytes(std::span<const uint8_t> data) {
        bytes.insert(bytes.end(), data.begin(), data.end());
    }
};

struct BlobReader {
    std::span<const uint8_t> bytes;
    size_t offset = 0;
    template <typename T>
    bool read(T& value) {
        if (offset + sizeof(T) > bytes.size()) {
            return false;
        }
        std::memcpy(&value, bytes.data() + offset, sizeof(T));
        offset += sizeof(T);
        return true;
    }
};

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

const SdfGlyph& SdfFontAtlas::glyphFor(char character) const {
    const auto code = static_cast<uint32_t>(static_cast<unsigned char>(character));
    const uint32_t fallback = static_cast<uint32_t>('?') - kSdfFirstChar;
    const uint32_t index =
        (code >= kSdfFirstChar && code <= kSdfLastChar) ? code - kSdfFirstChar : fallback;
    return glyphs[index];
}

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

SdfFontAtlas generateTrueTypeFontAtlas(std::span<const uint8_t> fontBytes, float pixelHeight) {
    SdfFontAtlas atlas;
    if (fontBytes.size() < 12 || pixelHeight <= 4.0f) {
        return atlas;
    }
    // stb_truetype does no bounds checking on untrusted data (its own
    // header warns as much), so reject anything that does not carry a
    // plausible sfnt header before the parser ever sees it: TrueType
    // (0x00010000 / 'true'), OpenType-CFF ('OTTO') or a collection
    // ('ttcf', whose first font stb can locate via its offset table).
    const uint32_t magic = (static_cast<uint32_t>(fontBytes[0]) << 24) |
                           (static_cast<uint32_t>(fontBytes[1]) << 16) |
                           (static_cast<uint32_t>(fontBytes[2]) << 8) |
                           static_cast<uint32_t>(fontBytes[3]);
    if (magic != 0x00010000u && magic != 0x74727565u /*'true'*/ && magic != 0x4F54544Fu /*'OTTO'*/ &&
        magic != 0x74746366u /*'ttcf'*/) {
        return atlas;
    }
    stbtt_fontinfo font;
    if (stbtt_InitFont(&font, fontBytes.data(),
                       stbtt_GetFontOffsetForIndex(fontBytes.data(), 0)) == 0) {
        return atlas;
    }

    // Bake large, report small: glyphs are rasterized at `pixelHeight`
    // for a crisp distance field, then every metric is divided into
    // font units (em = kUiFontUnitsPerEm) so drawText scale semantics
    // stay independent of the bake resolution.
    const float scale = stbtt_ScaleForPixelHeight(&font, pixelHeight);
    const float unitsPerPixel = kUiFontUnitsPerEm / pixelHeight;
    int ascentPx = 0;
    int descentPx = 0;
    int lineGapPx = 0;
    stbtt_GetFontVMetrics(&font, &ascentPx, &descentPx, &lineGapPx);
    atlas.ascent = static_cast<float>(ascentPx) * scale * unitsPerPixel;
    atlas.descent = static_cast<float>(descentPx) * scale * unitsPerPixel;
    atlas.lineHeight = static_cast<float>(ascentPx - descentPx + lineGapPx) * scale * unitsPerPixel;
    atlas.trueType = true;

    // Rasterize every printable ASCII glyph's SDF, then shelf-pack
    // the bitmaps into one page. Blank glyphs (space) only advance.
    struct BakedGlyph {
        std::vector<uint8_t> bitmap;
        int width = 0;
        int height = 0;
    };
    std::array<BakedGlyph, kSdfGlyphCount> baked;
    constexpr int kPadding = 6;               // SDF halo, bake pixels
    constexpr unsigned char kOnEdge = 128;    // shader thresholds at 0.5
    uint32_t shelfWidth = 0;
    for (uint32_t index = 0; index < kSdfGlyphCount; ++index) {
        const int codepoint = static_cast<int>(kSdfFirstChar + index);
        const int glyphIndex = stbtt_FindGlyphIndex(&font, codepoint);
        int advanceWidth = 0;
        int leftSideBearing = 0;
        stbtt_GetGlyphHMetrics(&font, glyphIndex, &advanceWidth, &leftSideBearing);
        SdfGlyph& glyph = atlas.glyphs[index];
        glyph.advance = static_cast<float>(advanceWidth) * scale * unitsPerPixel;

        int width = 0;
        int height = 0;
        int xoff = 0;
        int yoff = 0;
        unsigned char* sdf = stbtt_GetGlyphSDF(&font, scale, glyphIndex, kPadding, kOnEdge,
                                               static_cast<float>(kOnEdge) / static_cast<float>(kPadding),
                                               &width, &height, &xoff, &yoff);
        if (sdf != nullptr && width > 0 && height > 0) {
            BakedGlyph& entry = baked[index];
            entry.width = width;
            entry.height = height;
            entry.bitmap.assign(sdf, sdf + static_cast<size_t>(width) * static_cast<size_t>(height));
            glyph.bearingX = static_cast<float>(xoff) * unitsPerPixel;
            glyph.bearingY = static_cast<float>(yoff) * unitsPerPixel;
            glyph.width = static_cast<float>(width) * unitsPerPixel;
            glyph.height = static_cast<float>(height) * unitsPerPixel;
            shelfWidth += static_cast<uint32_t>(width) + 2;
        }
        if (sdf != nullptr) {
            stbtt_FreeSDF(sdf, nullptr);
        }
    }
    atlas.advance = atlas.glyphs[static_cast<size_t>('M' - kSdfFirstChar)].advance;

    // Pairwise kerning (font units) for every ASCII pair the font
    // kerns; most pairs stay exactly 0.
    for (uint32_t first = 0; first < kSdfGlyphCount; ++first) {
        for (uint32_t second = 0; second < kSdfGlyphCount; ++second) {
            const int kern = stbtt_GetCodepointKernAdvance(
                &font, static_cast<int>(kSdfFirstChar + first), static_cast<int>(kSdfFirstChar + second));
            if (kern != 0) {
                atlas.kerning[first][second] = static_cast<float>(kern) * scale * unitsPerPixel;
            }
        }
    }

    // Shelf-pack: rows of glyphs left-to-right, page grows downwards.
    atlas.width = 1024;
    uint32_t penX = 1;
    uint32_t penY = 1;
    uint32_t shelfHeight = 0;
    for (uint32_t index = 0; index < kSdfGlyphCount; ++index) {
        const BakedGlyph& entry = baked[index];
        if (entry.width <= 0 || entry.height <= 0) {
            continue;
        }
        if (penX + static_cast<uint32_t>(entry.width) + 1 > atlas.width) {
            penX = 1;
            penY += shelfHeight + 2;
            shelfHeight = 0;
        }
        SdfGlyph& glyph = atlas.glyphs[index];
        glyph.u0 = static_cast<float>(penX);
        glyph.v0 = static_cast<float>(penY);
        glyph.u1 = static_cast<float>(penX + static_cast<uint32_t>(entry.width));
        glyph.v1 = static_cast<float>(penY + static_cast<uint32_t>(entry.height));
        penX += static_cast<uint32_t>(entry.width) + 2;
        shelfHeight = std::max(shelfHeight, static_cast<uint32_t>(entry.height));
    }
    atlas.height = penY + shelfHeight + 1;
    // Normalize UVs now the final page size is known, and copy pixels.
    atlas.pixels.assign(static_cast<size_t>(atlas.width) * atlas.height, 0);
    penX = 1;
    penY = 1;
    shelfHeight = 0;
    for (uint32_t index = 0; index < kSdfGlyphCount; ++index) {
        const BakedGlyph& entry = baked[index];
        if (entry.width <= 0 || entry.height <= 0) {
            continue;
        }
        if (penX + static_cast<uint32_t>(entry.width) + 1 > atlas.width) {
            penX = 1;
            penY += shelfHeight + 2;
            shelfHeight = 0;
        }
        for (int row = 0; row < entry.height; ++row) {
            std::memcpy(&atlas.pixels[(penY + static_cast<uint32_t>(row)) * atlas.width + penX],
                        &entry.bitmap[static_cast<size_t>(row) * static_cast<size_t>(entry.width)],
                        static_cast<size_t>(entry.width));
        }
        SdfGlyph& glyph = atlas.glyphs[index];
        glyph.u0 /= static_cast<float>(atlas.width);
        glyph.v0 /= static_cast<float>(atlas.height);
        glyph.u1 /= static_cast<float>(atlas.width);
        glyph.v1 /= static_cast<float>(atlas.height);
        penX += static_cast<uint32_t>(entry.width) + 2;
        shelfHeight = std::max(shelfHeight, static_cast<uint32_t>(entry.height));
    }
    (void)shelfWidth;
    return atlas;
}

SdfFontAtlas generateSdfFontAtlas() {
    for (const std::string& candidate : uiFontCandidatePaths()) {
        const std::vector<uint8_t> bytes = readWholeFile(candidate);
        if (bytes.empty()) {
            continue;
        }
        SdfFontAtlas atlas = generateTrueTypeFontAtlas(bytes);
        if (!atlas.empty()) {
            atlas.fontName = candidate;
            return atlas;
        }
    }
    return generateBitmapFallbackAtlas();
}

SdfFontAtlas generateBitmapFallbackAtlas() {
    SdfFontAtlas atlas;
    atlas.width = kAtlasColumns * kSdfCellWidth;
    atlas.height = ((kSdfGlyphCount + kAtlasColumns - 1) / kAtlasColumns) * kSdfCellHeight;
    atlas.pixels.assign(static_cast<size_t>(atlas.width) * atlas.height, 0);
    atlas.advance = static_cast<float>(kHudGlyphAdvance);
    atlas.lineHeight = static_cast<float>(kHudLineAdvance);
    atlas.ascent = static_cast<float>(kHudGlyphHeight);
    atlas.descent = 0.0f;
    atlas.cellWidth = static_cast<float>(kSdfCellWidth) / static_cast<float>(kSdfGlyphScale);
    atlas.cellHeight = static_cast<float>(kSdfCellHeight) / static_cast<float>(kSdfGlyphScale);
    atlas.cellInset = static_cast<float>(kGlyphTexelOffset) / static_cast<float>(kSdfGlyphScale);
    atlas.trueType = false;

    std::vector<uint8_t> mask(kSdfCellWidth * kSdfCellHeight);
    std::vector<uint32_t> insideX;
    std::vector<uint32_t> insideY;
    std::vector<uint32_t> outsideX;
    std::vector<uint32_t> outsideY;

    for (uint32_t index = 0; index < kSdfGlyphCount; ++index) {
        const char character = static_cast<char>(kSdfFirstChar + index);
        const HudGlyph& rows = hudGlyphFor(character);
        std::fill(mask.begin(), mask.end(), 0);
        // Rasterize the 5x7 bitmap into the cell at kSdfGlyphScale.
        for (uint32_t gy = 0; gy < kHudGlyphHeight; ++gy) {
            for (uint32_t gx = 0; gx < kHudGlyphWidth; ++gx) {
                if (rows[gy][gx] != '#') {
                    continue;
                }
                for (uint32_t ty = 0; ty < kSdfGlyphScale; ++ty) {
                    for (uint32_t tx = 0; tx < kSdfGlyphScale; ++tx) {
                        const uint32_t x = kGlyphTexelOffset + gx * kSdfGlyphScale + tx;
                        const uint32_t y = kGlyphTexelOffset + gy * kSdfGlyphScale + ty;
                        mask[y * kSdfCellWidth + x] = 1;
                    }
                }
            }
        }
        insideX.clear();
        insideY.clear();
        outsideX.clear();
        outsideY.clear();
        for (uint32_t y = 0; y < kSdfCellHeight; ++y) {
            for (uint32_t x = 0; x < kSdfCellWidth; ++x) {
                if (mask[y * kSdfCellWidth + x] != 0) {
                    insideX.push_back(x);
                    insideY.push_back(y);
                } else {
                    outsideX.push_back(x);
                    outsideY.push_back(y);
                }
            }
        }

        const uint32_t cellX = (index % kAtlasColumns) * kSdfCellWidth;
        const uint32_t cellY = (index / kAtlasColumns) * kSdfCellHeight;
        for (uint32_t y = 0; y < kSdfCellHeight; ++y) {
            for (uint32_t x = 0; x < kSdfCellWidth; ++x) {
                const bool inside = mask[y * kSdfCellWidth + x] != 0;
                // An empty opposite set (blank glyph) reads as fully outside.
                float signedDistance = -static_cast<float>(kSdfSpread);
                if (inside && !outsideX.empty()) {
                    signedDistance = std::sqrt(
                        static_cast<float>(nearestDistanceSquared(x, y, outsideX, outsideY)));
                } else if (!inside && !insideX.empty()) {
                    signedDistance = -std::sqrt(
                        static_cast<float>(nearestDistanceSquared(x, y, insideX, insideY)));
                }
                const float value =
                    std::clamp(0.5f + 0.5f * signedDistance / static_cast<float>(kSdfSpread), 0.0f, 1.0f);
                atlas.pixels[(cellY + y) * atlas.width + (cellX + x)] =
                    static_cast<uint8_t>(std::lround(value * 255.0f));
            }
        }

        SdfGlyph& glyph = atlas.glyphs[index];
        glyph.u0 = static_cast<float>(cellX) / static_cast<float>(atlas.width);
        glyph.v0 = static_cast<float>(cellY) / static_cast<float>(atlas.height);
        glyph.u1 = static_cast<float>(cellX + kSdfCellWidth) / static_cast<float>(atlas.width);
        glyph.v1 = static_cast<float>(cellY + kSdfCellHeight) / static_cast<float>(atlas.height);
        glyph.advance = atlas.advance;
        // Baseline-layout equivalents of the old cell blit: the quad
        // sat at (pen - inset, lineTop - inset) with lineTop one
        // glyph-box above the baseline.
        glyph.bearingX = -atlas.cellInset;
        glyph.bearingY = -(atlas.ascent + atlas.cellInset);
        glyph.width = atlas.cellWidth;
        glyph.height = atlas.cellHeight;
    }
    return atlas;
}

std::vector<uint8_t> saveSdfFontAtlas(const SdfFontAtlas& atlas) {
    BlobWriter writer;
    writer.writeBytes(std::array<uint8_t, 4>{'E', 'S', 'D', 'F'});
    writer.write<uint32_t>(2); // format version (v2: metrics + kerning)
    writer.write(atlas.width);
    writer.write(atlas.height);
    writer.write(atlas.advance);
    writer.write(atlas.lineHeight);
    writer.write(atlas.cellWidth);
    writer.write(atlas.cellHeight);
    writer.write(atlas.cellInset);
    writer.write(atlas.ascent);
    writer.write(atlas.descent);
    writer.write<uint32_t>(atlas.trueType ? 1u : 0u);
    writer.write(kSdfGlyphCount);
    for (const SdfGlyph& glyph : atlas.glyphs) {
        writer.write(glyph.u0);
        writer.write(glyph.v0);
        writer.write(glyph.u1);
        writer.write(glyph.v1);
        writer.write(glyph.advance);
        writer.write(glyph.bearingX);
        writer.write(glyph.bearingY);
        writer.write(glyph.width);
        writer.write(glyph.height);
    }
    for (const auto& row : atlas.kerning) {
        for (const float kern : row) {
            writer.write(kern);
        }
    }
    const uint32_t nameSize = static_cast<uint32_t>(atlas.fontName.size());
    writer.write(nameSize);
    writer.writeBytes(std::span<const uint8_t>{
        reinterpret_cast<const uint8_t*>(atlas.fontName.data()), atlas.fontName.size()});
    writer.writeBytes(atlas.pixels);
    return writer.bytes;
}

bool loadSdfFontAtlas(std::span<const uint8_t> blob, SdfFontAtlas& out) {
    BlobReader reader{blob, 0};
    std::array<uint8_t, 4> magic{};
    uint32_t version = 0;
    uint32_t glyphCount = 0;
    uint32_t trueType = 0;
    uint32_t nameSize = 0;
    SdfFontAtlas atlas;
    if (!reader.read(magic) || magic != std::array<uint8_t, 4>{'E', 'S', 'D', 'F'} ||
        !reader.read(version) || version != 2 || !reader.read(atlas.width) || !reader.read(atlas.height) ||
        atlas.width == 0 || atlas.height == 0 || atlas.width > 16384 || atlas.height > 16384 ||
        !reader.read(atlas.advance) || !reader.read(atlas.lineHeight) || !reader.read(atlas.cellWidth) ||
        !reader.read(atlas.cellHeight) || !reader.read(atlas.cellInset) || !reader.read(atlas.ascent) ||
        !reader.read(atlas.descent) || !reader.read(trueType) || !reader.read(glyphCount) ||
        glyphCount != kSdfGlyphCount) {
        return false;
    }
    atlas.trueType = trueType != 0;
    for (SdfGlyph& glyph : atlas.glyphs) {
        if (!reader.read(glyph.u0) || !reader.read(glyph.v0) || !reader.read(glyph.u1) ||
            !reader.read(glyph.v1) || !reader.read(glyph.advance) || !reader.read(glyph.bearingX) ||
            !reader.read(glyph.bearingY) || !reader.read(glyph.width) || !reader.read(glyph.height)) {
            return false;
        }
    }
    for (auto& row : atlas.kerning) {
        for (float& kern : row) {
            if (!reader.read(kern)) {
                return false;
            }
        }
    }
    if (!reader.read(nameSize) || nameSize > 4096 || reader.offset + nameSize > blob.size()) {
        return false;
    }
    atlas.fontName.assign(reinterpret_cast<const char*>(blob.data() + reader.offset), nameSize);
    reader.offset += nameSize;
    const size_t pixelCount = static_cast<size_t>(atlas.width) * atlas.height;
    if (reader.offset + pixelCount != blob.size()) {
        return false;
    }
    atlas.pixels.assign(blob.begin() + static_cast<std::ptrdiff_t>(reader.offset), blob.end());
    out = std::move(atlas);
    return true;
}

} // namespace engine
