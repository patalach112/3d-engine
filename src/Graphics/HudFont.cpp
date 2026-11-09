#include "Graphics/HudFont.h"

#include <cctype>
#include <iomanip>
#include <sstream>
#include <utility>

namespace engine {
namespace {

const HudGlyph kGlyphSpace{{".....", ".....", ".....", ".....", ".....", ".....", "....."}};
const HudGlyph kGlyphUnknown{{".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#.."}};

const HudGlyph& glyphUppercase(char character) {
    static const HudGlyph kA{{".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}};
    static const HudGlyph kB{{"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."}};
    static const HudGlyph kC{{".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."}};
    static const HudGlyph kD{{"####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."}};
    static const HudGlyph kE{{"#####", "#....", "#....", "####.", "#....", "#....", "#####"}};
    static const HudGlyph kF{{"#####", "#....", "#....", "####.", "#....", "#....", "#...."}};
    static const HudGlyph kG{{".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".###."}};
    static const HudGlyph kH{{"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}};
    static const HudGlyph kI{{"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "#####"}};
    static const HudGlyph kJ{{"..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."}};
    static const HudGlyph kK{{"#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"}};
    static const HudGlyph kL{{"#....", "#....", "#....", "#....", "#....", "#....", "#####"}};
    static const HudGlyph kM{{"#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"}};
    static const HudGlyph kN{{"#...#", "#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#"}};
    static const HudGlyph kO{{".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}};
    static const HudGlyph kP{{"####.", "#...#", "#...#", "####.", "#....", "#....", "#...."}};
    static const HudGlyph kQ{{".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"}};
    static const HudGlyph kR{{"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"}};
    static const HudGlyph kS{{".####", "#....", "#....", ".###.", "....#", "....#", "####."}};
    static const HudGlyph kT{{"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."}};
    static const HudGlyph kU{{"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}};
    static const HudGlyph kV{{"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."}};
    static const HudGlyph kW{{"#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#"}};
    static const HudGlyph kX{{"#...#", ".#.#.", "..#..", "..#..", "..#..", ".#.#.", "#...#"}};
    static const HudGlyph kY{{"#...#", ".#.#.", "..#..", "..#..", "..#..", "..#..", "..#.."}};
    static const HudGlyph kZ{{"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"}};
    static const HudGlyph k0{{".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."}};
    static const HudGlyph k1{{"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", "#####"}};
    static const HudGlyph k2{{".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"}};
    static const HudGlyph k3{{"#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###."}};
    static const HudGlyph k4{{"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."}};
    static const HudGlyph k5{{"#####", "#....", "####.", "....#", "....#", "#...#", ".###."}};
    static const HudGlyph k6{{".###.", "#....", "#....", "####.", "#...#", "#...#", ".###."}};
    static const HudGlyph k7{{"#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."}};
    static const HudGlyph k8{{".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."}};
    static const HudGlyph k9{{".###.", "#...#", "#...#", ".####", "....#", "....#", ".###."}};
    static const HudGlyph kDot{{".....", ".....", ".....", ".....", ".....", ".##..", ".##.."}};
    static const HudGlyph kColon{{".....", ".##..", ".##..", ".....", ".##..", ".##..", "....."}};
    static const HudGlyph kSlash{{"....#", "....#", "...#.", "..#..", ".#...", "#....", "#...."}};
    static const HudGlyph kPercent{{"##..#", "##.#.", "....#", "...#.", "..#..", ".#.##", "#..##"}};
    static const HudGlyph kMinus{{".....", ".....", ".....", "#####", ".....", ".....", "....."}};
    static const HudGlyph kPlus{{".....", "..#..", "..#..", "#####", "..#..", "..#..", "....."}};
    static const HudGlyph kOpenParen{{"...##", "..#..", ".#...", ".#...", ".#...", "..#..", "...##"}};
    static const HudGlyph kCloseParen{{"##...", "..#..", "...#.", "...#.", "...#.", "..#..", "##..."}};

    switch (character) {
    case 'A': return kA;
    case 'B': return kB;
    case 'C': return kC;
    case 'D': return kD;
    case 'E': return kE;
    case 'F': return kF;
    case 'G': return kG;
    case 'H': return kH;
    case 'I': return kI;
    case 'J': return kJ;
    case 'K': return kK;
    case 'L': return kL;
    case 'M': return kM;
    case 'N': return kN;
    case 'O': return kO;
    case 'P': return kP;
    case 'Q': return kQ;
    case 'R': return kR;
    case 'S': return kS;
    case 'T': return kT;
    case 'U': return kU;
    case 'V': return kV;
    case 'W': return kW;
    case 'X': return kX;
    case 'Y': return kY;
    case 'Z': return kZ;
    case '0': return k0;
    case '1': return k1;
    case '2': return k2;
    case '3': return k3;
    case '4': return k4;
    case '5': return k5;
    case '6': return k6;
    case '7': return k7;
    case '8': return k8;
    case '9': return k9;
    case '.': return kDot;
    case ':': return kColon;
    case '/': return kSlash;
    case '%': return kPercent;
    case '-': return kMinus;
    case '+': return kPlus;
    case '(': return kOpenParen;
    case ')': return kCloseParen;
    case ' ': return kGlyphSpace;
    default: return kGlyphUnknown;
    }
}

} // namespace

const HudGlyph& hudGlyphFor(char character) {
    const auto upper = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    return glyphUppercase(upper);
}

std::vector<HudVertex> buildHudVertices(std::string_view text, float xPixels, float yPixels, float pixelScale,
                                        uint32_t viewportWidth, uint32_t viewportHeight, HudColor color) {
    std::vector<HudVertex> vertices;
    if (text.empty() || viewportWidth == 0 || viewportHeight == 0 || pixelScale <= 0.0f) {
        return vertices;
    }

    const auto toNdcX = [viewportWidth](float pixelX) {
        return (pixelX / static_cast<float>(viewportWidth)) * 2.0f - 1.0f;
    };
    // Vulkan framebuffer rows run top-down: pixel row 0 is NDC y = -1.
    const auto toNdcY = [viewportHeight](float pixelY) {
        return (pixelY / static_cast<float>(viewportHeight)) * 2.0f - 1.0f;
    };
    const auto pushQuad = [&vertices, &toNdcX, &toNdcY, pixelScale, color](float pixelX, float pixelY) {
        const float x0 = toNdcX(pixelX);
        const float x1 = toNdcX(pixelX + pixelScale);
        const float y0 = toNdcY(pixelY);
        const float y1 = toNdcY(pixelY + pixelScale);
        const HudVertex topLeft{x0, y0, color.r, color.g, color.b, color.a};
        const HudVertex topRight{x1, y0, color.r, color.g, color.b, color.a};
        const HudVertex bottomLeft{x0, y1, color.r, color.g, color.b, color.a};
        const HudVertex bottomRight{x1, y1, color.r, color.g, color.b, color.a};
        vertices.insert(vertices.end(), {topLeft, bottomLeft, topRight, topRight, bottomLeft, bottomRight});
    };

    float cursorX = xPixels;
    float cursorY = yPixels;
    for (const char character : text) {
        if (character == '\n') {
            cursorX = xPixels;
            cursorY += static_cast<float>(kHudLineAdvance) * pixelScale;
            continue;
        }
        const HudGlyph& glyph = hudGlyphFor(character);
        for (uint32_t row = 0; row < kHudGlyphHeight; ++row) {
            const std::string_view rowBits = glyph[row];
            for (uint32_t column = 0; column < kHudGlyphWidth && column < rowBits.size(); ++column) {
                if (rowBits[column] == '#') {
                    pushQuad(cursorX + static_cast<float>(column) * pixelScale,
                             cursorY + static_cast<float>(row) * pixelScale);
                }
            }
        }
        cursorX += static_cast<float>(kHudGlyphAdvance) * pixelScale;
    }
    return vertices;
}

std::string formatHudText(double framesPerSecond, double frameMilliseconds, uint32_t workerThreads,
                          uint64_t vramUsedMiB, uint64_t vramBudgetMiB, uint32_t drawnCount,
                          uint32_t culledCount) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(1) << "FPS " << framesPerSecond << " FRAME " << std::setprecision(2)
           << frameMilliseconds << " MS\n"
           << "WORKERS " << workerThreads << "+1 VRAM " << vramUsedMiB << '/' << vramBudgetMiB << " MIB\n"
           << "DRAWN " << drawnCount << " CULLED " << culledCount;
    return stream.str();
}

} // namespace engine
