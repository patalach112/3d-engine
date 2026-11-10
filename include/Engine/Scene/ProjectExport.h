#pragma once
#include "Core/Export.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace engine {

class Scene;
class AssetManager;

// ---------------------------------------------------------------------------
// Standalone packaging (Phase 33)
//
// ExportProject() distills the editor scene into one deployable
// bundle file (magic "GBQB"): the scene as GBQ bytes plus every
// asset the scene references — mesh geometry, texture pixels,
// shader SPIR-V, material descriptions (JSON), and the names
// of native script classes the build must provide. Payloads are
// LZ77-compressed and CRC-32-checksummed; a JSON manifest inside
// the bundle indexes them (name/kind/sizes/offset/CRC), mirrored
// to a `<name>.manifest.json` sidecar plus a `game.ini` config
// for the standalone player.
//
// Layout (little-endian):
//   header 32B: magic GBQB | u32 version | u32 flags
//               u64 manifestOffset | u64 manifestSize
//               u32 entryCount | u32 reserved
//   payload blobs (8-byte aligned, LZ77 streams)
//   manifest (raw JSON text)
// ---------------------------------------------------------------------------

inline constexpr char kBundleMagic[4] = {'G', 'B', 'Q', 'B'};
inline constexpr uint32_t kBundleVersion = 1;

enum class BundleEntryKind : uint8_t { Scene = 0, Mesh = 1, Texture = 2, Shader = 3, Material = 4, Script = 5 };

[[nodiscard]] ENGINE_SCENE_API const char* bundleEntryKindName(BundleEntryKind kind);

struct BundleEntryInfo {
    BundleEntryKind kind = BundleEntryKind::Scene;
    std::string name;
    uint64_t rawSize = 0;    // payload size before compression
    uint64_t storedSize = 0; // bytes the payload occupies
    uint64_t dataOffset = 0; // absolute offset of the stored payload
    uint32_t crc32 = 0;      // CRC-32 of the raw payload
};

struct BundleInfo {
    uint32_t version = 0;
    uint64_t bundleFileSize = 0;
    std::vector<BundleEntryInfo> entries;

    [[nodiscard]] const BundleEntryInfo* find(std::string_view name) const {
        for (const BundleEntryInfo& entry : entries) {
            if (entry.name == name) {
                return &entry;
            }
        }
        return nullptr;
    }
};

struct ExportReport {
    bool ok = false;
    std::string error; // populated when ok == false
    std::filesystem::path bundlePath;
    std::vector<std::string> packed;  // "kind:name" per entry written
    std::vector<std::string> missing; // referenced names that were not registered
    uint64_t bundleBytes = 0;
};

// CRC-32 (ISO 3309, the zlib polynomial) of a byte range.
[[nodiscard]] ENGINE_SCENE_API uint32_t bundleCrc32(const void* data, size_t size);

// Exports the scene and everything it references into a GBQB
// bundle at bundlePath (parent directories are created), plus
// the manifest sidecar and game.ini next to it. Fails (nothing
// written) when any referenced asset is not registered.
ENGINE_SCENE_API ExportReport ExportProject(const Scene& scene, const AssetManager& assets,
                                            const std::filesystem::path& bundlePath);

// Reads only the header + manifest. False on bad magic/version,
// truncation, or a malformed manifest.
ENGINE_SCENE_API bool inspectBundle(const std::filesystem::path& bundlePath, BundleInfo& outInfo);

// Extracts one entry by name: decompresses and CRC-verifies.
// False on missing entry, truncation, or CRC mismatch.
ENGINE_SCENE_API bool extractBundleEntry(const std::filesystem::path& bundlePath, std::string_view name,
                                         std::vector<std::byte>& outBytes);

struct RuntimeLoadResult {
    bool ok = false;
    std::string error; // populated when ok == false
    std::vector<std::string> scripts; // native script classes the scene needs
    size_t entityCount = 0;
};

// The standalone runtime's boot step: registers every bundle
// asset into `assets`, then rebuilds `scene` from the packed
// scene entry. Used by game-player and by the tests.
ENGINE_SCENE_API RuntimeLoadResult loadBundleIntoRuntime(const std::filesystem::path& bundlePath,
                                                         AssetManager& assets, Scene& scene);

} // namespace engine
