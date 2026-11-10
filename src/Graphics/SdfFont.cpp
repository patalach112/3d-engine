#include "Graphics/SdfFont.h"

#include "Graphics/HudFont.h"

#include <algorithm>
#include <cmath>
#include <cstring>

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
        const size_t offset = bytes.size();
        bytes.resize(offset + sizeof(T));
        std::memcpy(bytes.data() + offset, &value, sizeof(T));
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

} // namespace

const SdfGlyph& SdfFontAtlas::glyphFor(char character) const {
    const auto code = static_cast<uint32_t>(static_cast<unsigned char>(character));
    const uint32_t fallback = static_cast<uint32_t>('?') - kSdfFirstChar;
    const uint32_t index =
        (code >= kSdfFirstChar && code <= kSdfLastChar) ? code - kSdfFirstChar : fallback;
    return glyphs[index];
}

SdfFontAtlas generateSdfFontAtlas() {
    SdfFontAtlas atlas;
    atlas.width = kAtlasColumns * kSdfCellWidth;
    atlas.height = ((kSdfGlyphCount + kAtlasColumns - 1) / kAtlasColumns) * kSdfCellHeight;
    atlas.pixels.assign(static_cast<size_t>(atlas.width) * atlas.height, 0);
    atlas.advance = static_cast<float>(kHudGlyphAdvance);
    atlas.lineHeight = static_cast<float>(kHudLineAdvance);
    atlas.cellWidth = static_cast<float>(kSdfCellWidth) / static_cast<float>(kSdfGlyphScale);
    atlas.cellHeight = static_cast<float>(kSdfCellHeight) / static_cast<float>(kSdfGlyphScale);
    atlas.cellInset = static_cast<float>(kGlyphTexelOffset) / static_cast<float>(kSdfGlyphScale);

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
    }
    return atlas;
}

std::vector<uint8_t> saveSdfFontAtlas(const SdfFontAtlas& atlas) {
    BlobWriter writer;
    writer.writeBytes(std::array<uint8_t, 4>{'E', 'S', 'D', 'F'});
    writer.write<uint32_t>(1); // format version
    writer.write(atlas.width);
    writer.write(atlas.height);
    writer.write(atlas.advance);
    writer.write(atlas.lineHeight);
    writer.write(atlas.cellWidth);
    writer.write(atlas.cellHeight);
    writer.write(atlas.cellInset);
    writer.write(kSdfGlyphCount);
    for (const SdfGlyph& glyph : atlas.glyphs) {
        writer.write(glyph.u0);
        writer.write(glyph.v0);
        writer.write(glyph.u1);
        writer.write(glyph.v1);
        writer.write(glyph.advance);
    }
    writer.writeBytes(atlas.pixels);
    return writer.bytes;
}

bool loadSdfFontAtlas(std::span<const uint8_t> blob, SdfFontAtlas& out) {
    BlobReader reader{blob, 0};
    std::array<uint8_t, 4> magic{};
    uint32_t version = 0;
    uint32_t glyphCount = 0;
    SdfFontAtlas atlas;
    if (!reader.read(magic) || magic != std::array<uint8_t, 4>{'E', 'S', 'D', 'F'} ||
        !reader.read(version) || version != 1 || !reader.read(atlas.width) || !reader.read(atlas.height) ||
        atlas.width == 0 || atlas.height == 0 || atlas.width > 16384 || atlas.height > 16384 ||
        !reader.read(atlas.advance) || !reader.read(atlas.lineHeight) || !reader.read(atlas.cellWidth) ||
        !reader.read(atlas.cellHeight) || !reader.read(atlas.cellInset) || !reader.read(glyphCount) ||
        glyphCount != kSdfGlyphCount) {
        return false;
    }
    for (SdfGlyph& glyph : atlas.glyphs) {
        if (!reader.read(glyph.u0) || !reader.read(glyph.v0) || !reader.read(glyph.u1) ||
            !reader.read(glyph.v1) || !reader.read(glyph.advance)) {
            return false;
        }
    }
    const size_t pixelCount = static_cast<size_t>(atlas.width) * atlas.height;
    if (reader.offset + pixelCount != blob.size()) {
        return false;
    }
    atlas.pixels.assign(blob.begin() + static_cast<std::ptrdiff_t>(reader.offset), blob.end());
    out = std::move(atlas);
    return true;
}

} // namespace engine
