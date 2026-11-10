#pragma once
#include "Core/Export.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "Graphics/UIContext.h"

namespace engine {

// ---------------------------------------------------------------------------
// UIAssetBrowser (Phase 29): docked Asset Browser panel over a
// project directory. Lists the current folder (folders first, then
// files, each name-sorted; hidden dotfiles skipped) with minimalist
// colored kind chips:
//
//   Mesh    .obj .gltf .glb          Texture .ppm .png .jpg .tga ...
//   Shader  .spv .vert .frag .glsl   Script  .cpp .h .lua .py ...
//   Module  .so .dll .dylib          Scene   .json .scene
//
// Clicking selects; double-clicking a folder navigates into it (a
// ".." row walks back up), double-clicking a file reports it as
// activated. Files are drag sources: the payload type comes from
// payloadTypeFor (empty = not draggable) and the payload data is
// the path relative to the browser root, so drop handlers resolve
// files with resolveWithinRoot (which refuses anything that would
// escape the root — "..", absolute paths, symlink tricks).
//
// The browser is presentation + navigation only; spawning and
// assignment happen in the host through UIContext drop targets.
// ---------------------------------------------------------------------------

enum class AssetEntryKind : uint8_t { Folder, Mesh, Texture, Shader, Script, Module, SceneFile, Unknown };

struct AssetEntry {
    std::string name;         // file name within its folder
    std::string relativePath; // from the browser root (generic separators)
    AssetEntryKind kind = AssetEntryKind::Unknown;
    bool isDirectory = false;
};

class ENGINE_SCENE_API UIAssetBrowser {
public:
    static constexpr float kRowHeight = 22.0f;

    struct Result {
        const AssetEntry* activated = nullptr; // double-clicked file (valid until the next build/refresh)
        bool navigated = false;                // the current folder changed this frame
    };

    // Points the browser at a project folder and scans it. A
    // missing root simply yields an empty browser (drawn as such).
    void setRootPath(const std::filesystem::path& root);
    [[nodiscard]] const std::filesystem::path& rootPath() const { return m_root; }
    // Current folder relative to the root ("" at the root itself).
    [[nodiscard]] const std::filesystem::path& currentDirectory() const { return m_current; }
    void refresh();
    [[nodiscard]] const std::vector<AssetEntry>& entries() const { return m_entries; }
    [[nodiscard]] const AssetEntry* selectedEntry() const;

    Result build(UIContext& ui, const UIRect& rect);

    // Extension-based recognition (case-insensitive; directories
    // are always Folder regardless of their name).
    [[nodiscard]] static AssetEntryKind classifyPath(const std::filesystem::path& path, bool isDirectory);
    // Drag payload type per kind ("" = the kind cannot be dragged).
    [[nodiscard]] static std::string_view payloadTypeFor(AssetEntryKind kind);
    // What the import pipeline can actually consume today: OBJ.
    [[nodiscard]] static bool isImportableMesh(const std::filesystem::path& path);
    // One folder's entries: folders first, then files, each sorted
    // by name; dotfiles skipped; {} when the folder is unreadable.
    [[nodiscard]] static std::vector<AssetEntry> scanDirectory(const std::filesystem::path& root,
                                                               const std::filesystem::path& relativeDir);
    // Resolves `relative` under `root`, refusing anything that
    // escapes it. On success outAbsolute is the canonical target.
    [[nodiscard]] static bool resolveWithinRoot(const std::filesystem::path& root,
                                                const std::filesystem::path& relative,
                                                std::filesystem::path& outAbsolute);

private:
    std::filesystem::path m_root;
    std::filesystem::path m_current; // relative to m_root
    std::vector<AssetEntry> m_entries;
    std::string m_selectedPath; // relativePath of the selection ("" = none)
    float m_scroll = 0.0f;      // pixels
    std::string m_lastClickedPath;
    double m_lastClickSeconds = -1.0; // steady-clock seconds (double-click)

    void navigateInto(const AssetEntry& folder);
    void navigateUp();
};

} // namespace engine
