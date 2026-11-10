#include "Scene/UIAssetBrowser.h"

#include <algorithm>
#include <cctype>
#include <chrono>

namespace engine {
namespace {

double nowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string loweredExtension(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}

glm::vec4 chipColor(AssetEntryKind kind) {
    switch (kind) {
        case AssetEntryKind::Folder: return glm::vec4{0.85f, 0.65f, 0.25f, 1.0f};
        case AssetEntryKind::Mesh: return glm::vec4{0.45f, 0.75f, 1.0f, 1.0f};
        case AssetEntryKind::Texture: return glm::vec4{0.55f, 0.90f, 0.50f, 1.0f};
        case AssetEntryKind::Shader: return glm::vec4{0.80f, 0.55f, 1.0f, 1.0f};
        case AssetEntryKind::Script: return glm::vec4{1.00f, 0.60f, 0.35f, 1.0f};
        case AssetEntryKind::Module: return glm::vec4{0.95f, 0.45f, 0.45f, 1.0f};
        case AssetEntryKind::SceneFile: return glm::vec4{0.40f, 0.85f, 0.85f, 1.0f};
        case AssetEntryKind::Unknown: break;
    }
    return glm::vec4{0.50f, 0.52f, 0.58f, 1.0f};
}

const char* chipLetter(AssetEntryKind kind) {
    switch (kind) {
        case AssetEntryKind::Folder: return "D";
        case AssetEntryKind::Mesh: return "M";
        case AssetEntryKind::Texture: return "T";
        case AssetEntryKind::Shader: return "S";
        case AssetEntryKind::Script: return "C";
        case AssetEntryKind::Module: return "N";
        case AssetEntryKind::SceneFile: return "V";
        case AssetEntryKind::Unknown: break;
    }
    return "?";
}

} // namespace

AssetEntryKind UIAssetBrowser::classifyPath(const std::filesystem::path& path, bool isDirectory) {
    if (isDirectory) {
        return AssetEntryKind::Folder;
    }
    const std::string ext = loweredExtension(path);
    if (ext == ".obj" || ext == ".gltf" || ext == ".glb") {
        return AssetEntryKind::Mesh;
    }
    if (ext == ".ppm" || ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp") {
        return AssetEntryKind::Texture;
    }
    if (ext == ".spv" || ext == ".vert" || ext == ".frag" || ext == ".geom" || ext == ".glsl") {
        return AssetEntryKind::Shader;
    }
    if (ext == ".cpp" || ext == ".cc" || ext == ".h" || ext == ".hpp" || ext == ".lua" || ext == ".py") {
        return AssetEntryKind::Script;
    }
    if (ext == ".so" || ext == ".dll" || ext == ".dylib") {
        return AssetEntryKind::Module;
    }
    if (ext == ".json" || ext == ".scene") {
        return AssetEntryKind::SceneFile;
    }
    return AssetEntryKind::Unknown;
}

std::string_view UIAssetBrowser::payloadTypeFor(AssetEntryKind kind) {
    switch (kind) {
        case AssetEntryKind::Mesh: return "AssetMesh";
        case AssetEntryKind::Texture: return "AssetTexture";
        case AssetEntryKind::Shader: return "AssetShader";
        case AssetEntryKind::Script: return "AssetScript";
        case AssetEntryKind::Folder:
        case AssetEntryKind::Module:
        case AssetEntryKind::SceneFile:
        case AssetEntryKind::Unknown: break;
    }
    return "";
}

bool UIAssetBrowser::isImportableMesh(const std::filesystem::path& path) {
    return loweredExtension(path) == ".obj";
}

std::vector<AssetEntry> UIAssetBrowser::scanDirectory(const std::filesystem::path& root,
                                                      const std::filesystem::path& relativeDir) {
    std::vector<AssetEntry> entries;
    const std::filesystem::path dir = relativeDir.empty() ? root : root / relativeDir;
    std::error_code ec;
    const std::filesystem::directory_iterator iterator(dir, ec);
    if (ec) {
        return entries;
    }
    for (const std::filesystem::directory_entry& item : iterator) {
        const std::string name = item.path().filename().string();
        if (name.empty() || name.front() == '.') {
            continue; // hidden files are not project assets
        }
        const bool isDir = item.is_directory(ec);
        if (ec) {
            continue;
        }
        AssetEntry entry;
        entry.name = name;
        entry.relativePath = (relativeDir / item.path().filename()).generic_string();
        entry.isDirectory = isDir;
        entry.kind = classifyPath(item.path(), isDir);
        entries.push_back(std::move(entry));
    }
    std::sort(entries.begin(), entries.end(), [](const AssetEntry& a, const AssetEntry& b) {
        if (a.isDirectory != b.isDirectory) {
            return a.isDirectory > b.isDirectory;
        }
        return a.name < b.name;
    });
    return entries;
}

bool UIAssetBrowser::resolveWithinRoot(const std::filesystem::path& root, const std::filesystem::path& relative,
                                       std::filesystem::path& outAbsolute) {
    std::error_code ec;
    const std::filesystem::path canonRoot = std::filesystem::weakly_canonical(root, ec);
    if (ec) {
        return false;
    }
    const std::filesystem::path candidate = std::filesystem::weakly_canonical(canonRoot / relative, ec);
    if (ec) {
        return false;
    }
    const std::string rootStr = canonRoot.generic_string();
    const std::string candidateStr = candidate.generic_string();
    if (candidateStr == rootStr) {
        outAbsolute = candidate;
        return true;
    }
    std::string prefix = rootStr;
    if (prefix.empty() || prefix.back() != '/') {
        prefix += '/';
    }
    if (candidateStr.starts_with(prefix)) {
        outAbsolute = candidate;
        return true;
    }
    return false;
}

void UIAssetBrowser::setRootPath(const std::filesystem::path& root) {
    m_root = root;
    m_current.clear();
    m_selectedPath.clear();
    m_scroll = 0.0f;
    refresh();
}

void UIAssetBrowser::refresh() {
    m_entries = scanDirectory(m_root, m_current);
}

void UIAssetBrowser::navigateInto(const AssetEntry& folder) {
    m_current = std::filesystem::path(folder.relativePath);
    m_selectedPath.clear();
    m_scroll = 0.0f;
    refresh();
}

void UIAssetBrowser::navigateUp() {
    if (m_current.empty()) {
        return;
    }
    m_current = m_current.parent_path();
    m_selectedPath.clear();
    m_scroll = 0.0f;
    refresh();
}

const AssetEntry* UIAssetBrowser::selectedEntry() const {
    if (m_selectedPath.empty() || m_selectedPath == "..") {
        return nullptr;
    }
    for (const AssetEntry& entry : m_entries) {
        if (entry.relativePath == m_selectedPath) {
            return &entry;
        }
    }
    return nullptr;
}

UIAssetBrowser::Result UIAssetBrowser::build(UIContext& ui, const UIRect& rect) {
    Result result;
    UIBatch* batch = ui.batch();
    if (batch == nullptr) {
        return result;
    }
    if (selectedEntry() == nullptr && m_selectedPath != "..") {
        m_selectedPath.clear();
    }

    ui.beginPanel("Asset Browser", rect);
    const std::string header =
        m_current.empty() ? std::string{"Assets"} : "Assets / " + m_current.generic_string();
    ui.label(header.c_str());

    const bool hasParent = !m_current.empty();
    const size_t rowCount = m_entries.size() + (hasParent ? 1u : 0u);
    const float rowsTop = rect.y + UIContext::kPanelPadding +
                          (UIContext::kRowHeight + UIContext::kRowSpacing); // below the header row
    const float viewBottom = rect.y + rect.height - UIContext::kPanelPadding;
    const float viewHeight = std::max(0.0f, viewBottom - rowsTop);
    const float contentHeight = static_cast<float>(rowCount) * kRowHeight;
    if (ui.hoveredRect(rect)) {
        m_scroll -= ui.scrollDelta() * (3.0f * kRowHeight);
    }
    m_scroll = std::clamp(m_scroll, 0.0f, std::max(0.0f, contentHeight - viewHeight));

    if (rowCount == 0) {
        batch->drawText(m_root.empty() ? "No asset root set" : "Folder is empty",
                        rect.x + UIContext::kPanelPadding + 2.0f, rowsTop + 2.0f, UIContext::kTextScale,
                        glm::vec4{0.55f, 0.60f, 0.70f, 1.0f});
    }

    const double now = nowSeconds();
    for (size_t row = 0; row < rowCount; ++row) {
        const bool parentRow = hasParent && row == 0;
        const AssetEntry* entry = parentRow ? nullptr : &m_entries[row - (hasParent ? 1u : 0u)];
        const AssetEntryKind kind = parentRow ? AssetEntryKind::Folder : entry->kind;
        const std::string key = parentRow ? std::string{".."} : entry->relativePath;
        const UIRect rowRect{rect.x + 4.0f, rowsTop + static_cast<float>(row) * kRowHeight - m_scroll,
                             rect.width - 8.0f, kRowHeight};
        if (rowRect.y + kRowHeight < rowsTop || rowRect.y > viewBottom) {
            continue; // fully scrolled out of the panel
        }
        const bool isSelected = m_selectedPath == key;
        if (isSelected) {
            batch->drawRoundedRect(rowRect.x, rowRect.y, rowRect.width, rowRect.height,
                                   UIRectStyle{4.0f, 0.0f, glm::vec4{0.0f}}, glm::vec4{0.16f, 0.38f, 0.68f, 0.55f});
        } else if (ui.hoveredRect(rowRect)) {
            batch->drawRoundedRect(rowRect.x, rowRect.y, rowRect.width, rowRect.height,
                                   UIRectStyle{4.0f, 0.0f, glm::vec4{0.0f}}, glm::vec4{1.0f, 1.0f, 1.0f, 0.08f});
        }
        // Kind chip + name (".." walks up; folders carry a trailing /).
        batch->drawRoundedRect(rowRect.x + 2.0f, rowRect.y + 3.0f, 16.0f, 16.0f,
                               UIRectStyle{3.0f, 0.0f, glm::vec4{0.0f}}, chipColor(kind));
        batch->drawText(chipLetter(kind), rowRect.x + 7.0f, rowRect.y + 5.0f, 1.3f,
                        glm::vec4{0.05f, 0.06f, 0.08f, 1.0f});
        const std::string display = parentRow ? std::string{".."} : entry->name + (entry->isDirectory ? "/" : "");
        batch->drawText(display.c_str(), rowRect.x + 24.0f, rowRect.y + 5.0f, UIContext::kTextScale,
                        isSelected ? glm::vec4{1.0f} : glm::vec4{0.85f, 0.90f, 0.97f, 1.0f});

        if (ui.pressedRect(rowRect)) {
            const bool doubleClick = m_lastClickedPath == key && now - m_lastClickSeconds < 0.35;
            m_selectedPath = key;
            if (doubleClick) {
                m_lastClickedPath.clear();
                m_lastClickSeconds = -1.0;
                if (parentRow) {
                    navigateUp();
                    result.navigated = true;
                    break; // the entry list was just replaced
                }
                if (entry->isDirectory) {
                    navigateInto(*entry);
                    result.navigated = true;
                    break; // the entry list was just replaced
                }
                result.activated = entry;
            } else {
                m_lastClickedPath = key;
                m_lastClickSeconds = now;
            }
        }
        if (!parentRow && entry != nullptr && !entry->isDirectory) {
            if (const std::string_view payloadType = payloadTypeFor(entry->kind); !payloadType.empty()) {
                ui.beginDragSource(rowRect, payloadType, entry->relativePath, entry->name);
            }
        }
    }
    ui.endPanel();
    return result;
}

} // namespace engine
