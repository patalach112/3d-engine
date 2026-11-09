#include "Assets/AssetManager.h"

#include "Core/Logger.h"
#include "Graphics/VramBudget.h"

#include <algorithm>
#include <cstring>
#include <glm/common.hpp>
#include <fstream>

namespace engine {
namespace {

template <typename Asset>
const Asset* findById(const std::vector<Asset>& assets, uint32_t id) {
    // Ids are dense and 1-based: id - 1 is the storage index.
    if (id == kInvalidAssetId || id > assets.size()) {
        return nullptr;
    }
    return &assets[id - 1];
}

} // namespace

AssetManager::AssetManager(VramBudget& vramBudget, VramBudget& stagingBudget)
    : AssetManager(vramBudget, stagingBudget, Capacity{}) {}

AssetManager::AssetManager(VramBudget& vramBudget, VramBudget& stagingBudget, Capacity capacity)
    : m_vramBudget(vramBudget), m_stagingBudget(stagingBudget) {
    m_meshes.reserve(capacity.meshes);
    m_shaders.reserve(capacity.shaders);
    m_materials.reserve(capacity.materials);
    m_meshIds.reserve(capacity.meshes);
    m_shaderIds.reserve(capacity.shaders);
    m_materialIds.reserve(capacity.materials);
}

AssetManager::~AssetManager() {
    if (m_reservedVramBytes > 0) {
        m_vramBudget.release(m_reservedVramBytes);
    }
}

MeshId AssetManager::registerMesh(std::string name, std::span<const Vertex> vertices,
                                  std::span<const uint32_t> indices) {
    if (const auto existing = m_meshIds.find(name); existing != m_meshIds.end()) {
        return existing->second;
    }

    const uint64_t vramBytes = static_cast<uint64_t>(vertices.size() * sizeof(Vertex)) +
                               static_cast<uint64_t>(indices.size() * sizeof(uint32_t));

    // The upload passes through a staging buffer: check the transient
    // staging budget first (reserved and immediately released), then
    // reserve the persistent device-local footprint.
    if (!m_stagingBudget.tryReserve(vramBytes)) {
        ENGINE_LOG_WARNING("AssetManager: staging budget exceeded registering mesh '" << name << "'");
        return kInvalidAssetId;
    }
    m_stagingBudget.release(vramBytes);
    if (!m_vramBudget.tryReserve(vramBytes)) {
        ENGINE_LOG_WARNING("AssetManager: VRAM budget exceeded registering mesh '" << name << "' ("
                                                                                          << vramBytes
                                                                                          << " bytes)");
        return kInvalidAssetId;
    }
    m_reservedVramBytes += vramBytes;

    MeshAsset asset;
    asset.id = static_cast<MeshId>(m_meshes.size()) + 1;
    asset.name = name;
    asset.vertices.assign(vertices.begin(), vertices.end());
    asset.indices.assign(indices.begin(), indices.end());
    asset.vramBytes = vramBytes;
    if (!vertices.empty()) {
        glm::vec3 min = vertices.front().position;
        glm::vec3 max = min;
        for (const Vertex& vertex : vertices) {
            min = glm::min(min, vertex.position);
            max = glm::max(max, vertex.position);
        }
        asset.bounds = AABB{min, max};
    }

    m_meshIds.emplace(asset.name, asset.id);
    m_meshes.push_back(std::move(asset));
    return m_meshes.back().id;
}

ShaderId AssetManager::registerShader(std::string name, std::span<const uint32_t> spirvCode) {
    if (const auto existing = m_shaderIds.find(name); existing != m_shaderIds.end()) {
        return existing->second;
    }
    const uint64_t byteSize = static_cast<uint64_t>(spirvCode.size() * sizeof(uint32_t));
    if (!m_vramBudget.tryReserve(byteSize)) {
        ENGINE_LOG_WARNING("AssetManager: VRAM budget exceeded registering shader '" << name << "'");
        return kInvalidAssetId;
    }
    m_reservedVramBytes += byteSize;

    ShaderAsset asset;
    asset.id = static_cast<ShaderId>(m_shaders.size()) + 1;
    asset.name = name;
    asset.code.assign(spirvCode.begin(), spirvCode.end());
    asset.byteSize = byteSize;
    m_shaderIds.emplace(asset.name, asset.id);
    m_shaders.push_back(std::move(asset));
    return m_shaders.back().id;
}

ShaderId AssetManager::loadShader(const std::string& name, const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        ENGINE_LOG_WARNING("AssetManager: cannot open shader file '" << path.string() << "'");
        return kInvalidAssetId;
    }
    const std::streamsize byteCount = file.tellg();
    if (byteCount <= 0) {
        ENGINE_LOG_WARNING("AssetManager: shader file '" << path.string() << "' is empty");
        return kInvalidAssetId;
    }
    std::vector<char> bytes(static_cast<size_t>(byteCount));
    file.seekg(0);
    if (!file.read(bytes.data(), byteCount)) {
        ENGINE_LOG_WARNING("AssetManager: failed reading shader file '" << path.string() << "'");
        return kInvalidAssetId;
    }
    std::vector<uint32_t> words((bytes.size() + sizeof(uint32_t) - 1) / sizeof(uint32_t), 0);
    std::memcpy(words.data(), bytes.data(), bytes.size());
    return registerShader(name, words);
}

MaterialId AssetManager::registerMaterial(MaterialDesc desc) {
    if (const auto existing = m_materialIds.find(desc.name); existing != m_materialIds.end()) {
        return existing->second;
    }
    MaterialAsset asset;
    asset.id = static_cast<MaterialId>(m_materials.size()) + 1;
    asset.descriptorIndex = asset.id - 1; // dense bindless-style slot
    asset.desc = std::move(desc);
    m_materialIds.emplace(asset.desc.name, asset.id);
    m_materials.push_back(std::move(asset));
    return m_materials.back().id;
}

const MeshAsset* AssetManager::mesh(MeshId id) const {
    return findById(m_meshes, id);
}

const MeshAsset* AssetManager::mesh(std::string_view name) const {
    const auto it = m_meshIds.find(std::string(name));
    return it != m_meshIds.end() ? &m_meshes[it->second - 1] : nullptr;
}

const ShaderAsset* AssetManager::shader(ShaderId id) const {
    return findById(m_shaders, id);
}

const MaterialAsset* AssetManager::material(MaterialId id) const {
    return findById(m_materials, id);
}

const MaterialAsset* AssetManager::material(std::string_view name) const {
    const auto it = m_materialIds.find(std::string(name));
    return it != m_materialIds.end() ? &m_materials[it->second - 1] : nullptr;
}

} // namespace engine
