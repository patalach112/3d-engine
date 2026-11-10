# stb (vendored)

`stb_truetype.h` v1.26 by Sean Barrett / RAD Game Tools (public domain /
MIT dual license, see the header). Vendored so the TrueType UI font
rasterizer builds identically on Windows, Linux and macOS without a
package manager or network fetch at configure time. Only
`stb_truetype.h` is used (SDF glyph baking for the editor UI font).
