#pragma once
#include "Core/Export.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace engine {

class Scene;

// ---------------------------------------------------------------------------
// GBQ1 binary scene container (Phase 30)
//
// Layout (all integers little-endian, floats as raw IEEE bits):
//
//   Header (32 bytes)
//     char[4]  magic "GBQ1"
//     u32      format version (kGbqVersion)
//     u32      flags (0)
//     u32      section count
//     u32      reserved (0)
//     u64      table-of-contents offset
//     u32      reserved (0)
//   Table of contents (sectionCount x 32 bytes, right after the
//   header): { char[4] type, u32 section version, u64 offset,
//   u64 size } — a fast-access index: tools can locate any
//   section in O(1) without scanning the file (inspectGbqFile).
//   Sections (8-byte aligned): fixed-layout records, no text
//   anywhere. Strings are u32-length-prefixed byte runs.
//
//   META scene settings + entity count      ENTY parent indices
//   TAGC tag strings                        TRNF transforms
//   CAMR cameras                            MESH meshes
//   SPIN spins                              RGBD rigid bodies
//   CLDR colliders                          GRVF gravity fields
//   JONT joints (entity links remapped)     THRS thrusters
//   PLGT point lights                       SLGT spot lights
//   NSCR native script names (instances are runtime state and
//        are recreated by the script system, never persisted)
//
// Entities are referenced by their index in ENTY only — EnTT
// handles recycle and are never persisted. Unknown sections are
// skipped (forward compatibility); a known section with an
// unexpected version, any truncation, or a bad magic rejects
// the whole load and leaves the target scene untouched.
// ---------------------------------------------------------------------------

inline constexpr char kGbqMagic[4] = {'G', 'B', 'Q', '1'};
inline constexpr uint32_t kGbqVersion = 1;

struct GbqSectionInfo {
    std::string type; // four-character section code, e.g. "TRNF"
    uint32_t version = 0;
    uint64_t offset = 0; // absolute file offset of the section data
    uint64_t size = 0;   // section size in bytes
};

struct GbqFileInfo {
    uint32_t version = 0;
    uint32_t entityCount = 0; // from META when present
    uint64_t fileSize = 0;
    std::vector<GbqSectionInfo> sections;
};

// Reads just the header + table of contents (no section data is
// interpreted). False on I/O errors, bad magic or truncation.
[[nodiscard]] ENGINE_SCENE_API bool inspectGbqFile(const std::filesystem::path& path, GbqFileInfo& outInfo);

// Serializes the whole scene into a .gbq container (the fast,
// bit-exact counterpart of Scene::saveToFile). False on I/O error.
[[nodiscard]] ENGINE_SCENE_API bool serializeScene(const Scene& scene, const std::filesystem::path& path);

// Loads a .gbq scene: validates everything first (the scene is
// left untouched on malformed input), then clears and rebuilds
// the registry, hierarchy and joint links.
ENGINE_SCENE_API bool deserializeScene(Scene& scene, const std::filesystem::path& path);

} // namespace engine
