#include "Assets/AssetManager.h"

#include "Core/Logger.h"
#include "Graphics/VramBudget.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <glm/common.hpp>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <tuple>

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

MeshBounds computeMeshBounds(std::span<const Vertex> vertices) {
    MeshBounds bounds; // unit-cube defaults; also the empty-mesh result
    if (vertices.empty()) {
        return bounds;
    }
    glm::vec3 min = vertices.front().position;
    glm::vec3 max = min;
    for (const Vertex& vertex : vertices) {
        min = glm::min(min, vertex.position);
        max = glm::max(max, vertex.position);
    }
    bounds.min = min;
    bounds.max = max;
    bounds.center = (min + max) * 0.5f;
    bounds.size = max - min;
    bounds.extents = bounds.size * 0.5f;
    const float maxExtent = std::max({bounds.size.x, bounds.size.y, bounds.size.z});
    // A degenerate (zero-size) mesh keeps factor 1 rather than inf/nan.
    bounds.autoScaleFactor = maxExtent > 0.0f ? 1.0f / maxExtent : 1.0f;
    return bounds;
}

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
    asset.bounds = computeMeshBounds(vertices);

    m_meshIds.emplace(asset.name, asset.id);
    m_meshes.push_back(std::move(asset));
    return m_meshes.back().id;
}

namespace {

// --- Wavefront OBJ parsing -------------------------------------------------
//
// Minimal, strict importer for static triangle geometry: `v` positions,
// `vt` UVs and `vn` normals are collected, `f` faces (v, v/vt, v//vn and
// v/vt/vn forms, positive or negative relative indices) are
// fan-triangulated into an indexed mesh with duplicate attributes
// shared. Faces without usable normals get a flat Newell normal, so
// normal-less exports still shade correctly. Anything malformed
// (bad numbers, out-of-range indices, no faces) fails the whole import
// instead of registering a half-parsed mesh.

struct ObjCorner {
    uint32_t position = 0;
    int32_t texCoord = -1; // -1: face supplied no UV for this corner
    int32_t normal = -1;   // -1: face supplied no normal for this corner
};

bool parseObjFloat(std::string_view token, float& out) {
    if (token.empty()) {
        return false;
    }
    float value = 0.0f;
    const char* const end = token.data() + token.size();
    const auto result = std::from_chars(token.data(), end, value);
    if (result.ec != std::errc() || result.ptr != end || !std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

bool parseObjInt(std::string_view token, int& out) {
    if (token.empty()) {
        return false;
    }
    int value = 0;
    const char* const end = token.data() + token.size();
    const auto result = std::from_chars(token.data(), end, value);
    if (result.ec != std::errc() || result.ptr != end) {
        return false;
    }
    out = value;
    return true;
}

// OBJ indices are 1-based; negative indices count back from the most
// recently defined element (-1 is the last one).
bool resolveObjIndex(std::string_view token, size_t elementCount, uint32_t& out) {
    int raw = 0;
    if (!parseObjInt(token, raw) || raw == 0 || elementCount == 0) {
        return false;
    }
    const int64_t resolved = raw > 0 ? static_cast<int64_t>(raw) - 1
                                     : static_cast<int64_t>(elementCount) + static_cast<int64_t>(raw);
    if (resolved < 0 || resolved >= static_cast<int64_t>(elementCount)) {
        return false;
    }
    out = static_cast<uint32_t>(resolved);
    return true;
}

bool parseObjCorner(const std::string& token, size_t positionCount, size_t texCoordCount, size_t normalCount,
                    ObjCorner& out) {
    // Split "v", "v/vt", "v//vn" or "v/vt/vn" on '/'.
    std::array<std::string_view, 3> parts{};
    size_t partCount = 0;
    size_t begin = 0;
    const std::string_view view{token};
    while (begin <= view.size() && partCount < parts.size()) {
        const size_t slash = view.find('/', begin);
        if (slash == std::string_view::npos) {
            parts[partCount++] = view.substr(begin);
            begin = view.size() + 1;
        } else {
            parts[partCount++] = view.substr(begin, slash - begin);
            begin = slash + 1;
        }
    }
    if (partCount == 0 || begin != view.size() + 1) {
        return false;
    }
    if (!resolveObjIndex(parts[0], positionCount, out.position)) {
        return false;
    }
    if (partCount >= 2 && !parts[1].empty()) {
        uint32_t texCoord = 0;
        if (!resolveObjIndex(parts[1], texCoordCount, texCoord)) {
            return false;
        }
        out.texCoord = static_cast<int32_t>(texCoord);
    }
    if (partCount >= 3 && !parts[2].empty()) {
        uint32_t normal = 0;
        if (!resolveObjIndex(parts[2], normalCount, normal)) {
            return false;
        }
        out.normal = static_cast<int32_t>(normal);
    }
    return true;
}

bool parseObjStream(std::istream& stream, std::vector<Vertex>& outVertices, std::vector<uint32_t>& outIndices) {
    std::vector<glm::vec3> positions;
    std::vector<glm::vec2> texCoords;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec3> generatedNormals; // flat face normals, interned

    // Indexed-mesh deduplication on the full attribute tuple; generated
    // normals are keyed past the file-supplied ones so a flat-shaded
    // corner never merges with a smooth one sharing position/UV.
    using VertexKey = std::tuple<uint32_t, uint32_t, uint32_t>;
    constexpr uint32_t kMissing = std::numeric_limits<uint32_t>::max();
    std::map<VertexKey, uint32_t> vertexIds;

    const auto emitCorner = [&](const ObjCorner& corner, const glm::vec3& faceNormal, bool useFaceNormal,
                                bool& ok) -> uint32_t {
        glm::vec3 normal = faceNormal;
        uint32_t normalKey = kMissing;
        if (useFaceNormal) {
            uint32_t generated = static_cast<uint32_t>(generatedNormals.size());
            for (uint32_t i = 0; i < generatedNormals.size(); ++i) {
                if (generatedNormals[i] == faceNormal) {
                    generated = i;
                    break;
                }
            }
            if (generated == generatedNormals.size()) {
                generatedNormals.push_back(faceNormal);
            }
            // Key generated normals from the top of the range so they
            // can never collide with file-supplied normal indices.
            normalKey = kMissing - 1u - generated;
        } else {
            const glm::vec3& supplied = normals[static_cast<size_t>(corner.normal)];
            const float length = glm::length(supplied);
            if (length <= 1e-12f) {
                ok = false;
                return 0;
            }
            normal = supplied / length;
            normalKey = static_cast<uint32_t>(corner.normal);
        }
        const VertexKey key{corner.position,
                            corner.texCoord >= 0 ? static_cast<uint32_t>(corner.texCoord) : kMissing, normalKey};
        if (const auto existing = vertexIds.find(key); existing != vertexIds.end()) {
            return existing->second;
        }
        if (outVertices.size() >= std::numeric_limits<uint32_t>::max()) {
            ok = false;
            return 0;
        }
        Vertex vertex;
        vertex.position = positions[corner.position];
        vertex.normal = normal;
        vertex.color = glm::vec3{1.0f};
        vertex.texCoord = corner.texCoord >= 0 ? texCoords[static_cast<size_t>(corner.texCoord)] : glm::vec2{0.0f};
        const uint32_t id = static_cast<uint32_t>(outVertices.size());
        outVertices.push_back(vertex);
        vertexIds.emplace(key, id);
        return id;
    };

    std::string line;
    while (std::getline(stream, line)) {
        if (const size_t comment = line.find('#'); comment != std::string::npos) {
            line.erase(comment);
        }
        std::istringstream tokens{line};
        std::string record;
        if (!(tokens >> record)) {
            continue;
        }
        if (record == "v" || record == "vn") {
            std::array<std::string, 3> fields;
            if (!(tokens >> fields[0] >> fields[1] >> fields[2])) {
                return false;
            }
            glm::vec3 value;
            if (!parseObjFloat(fields[0], value.x) || !parseObjFloat(fields[1], value.y) ||
                !parseObjFloat(fields[2], value.z)) {
                return false;
            }
            (record == "v" ? positions : normals).push_back(value);
        } else if (record == "vt") {
            std::array<std::string, 2> fields;
            if (!(tokens >> fields[0] >> fields[1])) {
                return false;
            }
            glm::vec2 value;
            if (!parseObjFloat(fields[0], value.x) || !parseObjFloat(fields[1], value.y)) {
                return false;
            }
            texCoords.push_back(value);
        } else if (record == "f") {
            std::vector<ObjCorner> corners;
            std::string token;
            while (tokens >> token) {
                ObjCorner corner;
                if (!parseObjCorner(token, positions.size(), texCoords.size(), normals.size(), corner)) {
                    return false;
                }
                corners.push_back(corner);
            }
            if (corners.size() < 3) {
                return false;
            }
            const bool useFaceNormal =
                std::any_of(corners.begin(), corners.end(), [](const ObjCorner& c) { return c.normal < 0; });
            glm::vec3 faceNormal{0.0f};
            if (useFaceNormal) {
                // Newell's method: robust face normal even for slightly
                // non-planar polygons.
                for (size_t i = 0; i < corners.size(); ++i) {
                    const glm::vec3& current = positions[corners[i].position];
                    const glm::vec3& next = positions[corners[(i + 1) % corners.size()].position];
                    faceNormal.x += (current.y - next.y) * (current.z + next.z);
                    faceNormal.y += (current.z - next.z) * (current.x + next.x);
                    faceNormal.z += (current.x - next.x) * (current.y + next.y);
                }
                const float length = glm::length(faceNormal);
                if (length <= 1e-12f) {
                    continue; // degenerate polygon: nothing to triangulate
                }
                faceNormal /= length;
            }
            // Fan triangulation: (0, i, i+1) preserves the file winding.
            for (size_t i = 1; i + 1 < corners.size(); ++i) {
                const std::array<const ObjCorner*, 3> triangle{&corners[0], &corners[i], &corners[i + 1]};
                const glm::vec3& p0 = positions[triangle[0]->position];
                const glm::vec3& p1 = positions[triangle[1]->position];
                const glm::vec3& p2 = positions[triangle[2]->position];
                if (glm::length(glm::cross(p1 - p0, p2 - p0)) <= 1e-12f) {
                    continue; // zero-area sliver carries no surface
                }
                bool ok = true;
                std::array<uint32_t, 3> ids{};
                for (size_t c = 0; c < triangle.size(); ++c) {
                    ids[c] = emitCorner(*triangle[c], faceNormal, useFaceNormal, ok);
                }
                if (!ok) {
                    return false;
                }
                outIndices.insert(outIndices.end(), ids.begin(), ids.end());
            }
        }
        // All other records (o/g/s/usemtl/mtllib/l/p/...) are ignored:
        // material/texture binding stays with the engine's registry.
    }
    return !positions.empty() && !outIndices.empty();
}

} // namespace

MeshId AssetManager::loadObj(const std::string& name, const std::filesystem::path& path) {
    if (const auto existing = m_meshIds.find(name); existing != m_meshIds.end()) {
        return existing->second;
    }
    std::ifstream file(path);
    if (!file) {
        ENGINE_LOG_WARNING("AssetManager: cannot open OBJ file '" << path.string() << "'");
        return kInvalidAssetId;
    }
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    if (!parseObjStream(file, vertices, indices)) {
        ENGINE_LOG_WARNING("AssetManager: failed to parse OBJ file '" << path.string() << "'");
        return kInvalidAssetId;
    }
    const MeshId id = registerMesh(name, vertices, indices);
    if (id != kInvalidAssetId) {
        const MeshAsset& asset = m_meshes[id - 1];
        ENGINE_LOG_INFO("AssetManager: imported OBJ '" << path.string() << "' as mesh '" << name << "' ("
                                                       << asset.vertices.size() << " vertices, "
                                                       << asset.indices.size() / 3 << " triangles, auto-scale "
                                                       << asset.bounds.autoScaleFactor << ")");
    }
    return id;
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

namespace {

MaterialDesc sandboxMaterial(const char* name, const glm::vec4& baseColor, const MaterialLighting& lighting) {
    MaterialDesc desc;
    desc.name = name;
    desc.baseColor = baseColor;
    desc.lighting = lighting;
    return desc;
}

} // namespace

MaterialDesc makeSandboxFloorMaterial() {
    // Dark asphalt/slate: matte, dim specular, almost no rim, so the floor
    // recedes and everything resting on it reads against it.
    MaterialLighting lighting;
    lighting.specularColor = glm::vec3{0.32f, 0.38f, 0.46f};
    lighting.specularStrength = 0.08f;
    lighting.shininess = 10.0f;
    lighting.rimStrength = 0.035f;
    lighting.rimPower = 4.0f;
    lighting.ambientStrength = 0.34f;
    return sandboxMaterial("floor", glm::vec4{0.10f, 0.115f, 0.135f, 1.0f}, lighting);
}

MaterialDesc makeSandboxCubeMaterial() {
    // Warm metallic copper for the falling/dynamic cubes: strong warm
    // specular highlight, moderate rim.
    MaterialLighting lighting;
    lighting.specularColor = glm::vec3{1.0f, 0.70f, 0.40f};
    lighting.specularStrength = 0.62f;
    lighting.shininess = 56.0f;
    lighting.rimStrength = 0.16f;
    lighting.rimPower = 3.0f;
    lighting.ambientStrength = 0.24f;
    return sandboxMaterial("cube", glm::vec4{0.84f, 0.43f, 0.18f, 1.0f}, lighting);
}

MaterialDesc makeSandboxStackMaterial() {
    // Cool teal for the stacking stress cubes: separates the resting stack
    // from the warm falling cubes by hue as well as by shading.
    MaterialLighting lighting;
    lighting.specularColor = glm::vec3{0.55f, 0.95f, 1.0f};
    lighting.specularStrength = 0.42f;
    lighting.shininess = 38.0f;
    lighting.rimStrength = 0.22f;
    lighting.rimPower = 3.5f;
    lighting.ambientStrength = 0.27f;
    return sandboxMaterial("stackCube", glm::vec4{0.08f, 0.52f, 0.58f, 1.0f}, lighting);
}

MaterialDesc makeSandboxEnvironmentMaterial() {
    // Blue-violet environment (planet, CCD wall): mid-tone, cool specular.
    MaterialLighting lighting;
    lighting.specularColor = glm::vec3{0.62f, 0.74f, 1.0f};
    lighting.specularStrength = 0.38f;
    lighting.shininess = 28.0f;
    lighting.rimStrength = 0.18f;
    lighting.rimPower = 3.0f;
    lighting.ambientStrength = 0.28f;
    return sandboxMaterial("environment", glm::vec4{0.27f, 0.33f, 0.58f, 1.0f}, lighting);
}

MaterialDesc makeSandboxPlayerMaterial() {
    // Bright mint player accent: the strongest rim response in the palette,
    // so player-adjacent craft stay readable against every other category.
    MaterialLighting lighting;
    lighting.specularColor = glm::vec3{0.72f, 1.0f, 0.88f};
    lighting.specularStrength = 0.50f;
    lighting.shininess = 44.0f;
    lighting.rimStrength = 0.30f;
    lighting.rimPower = 2.6f;
    lighting.ambientStrength = 0.26f;
    return sandboxMaterial("player", glm::vec4{0.16f, 0.76f, 0.46f, 1.0f}, lighting);
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
