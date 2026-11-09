#pragma once
#include "Core/Export.h"

#include "Geometry/Bounds.h"
#include "Geometry/Vertex.h"
#include "Graphics/PipelineState.h"

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine {

class VramBudget;

// Stable, dense asset handles. 0 is always the invalid id; real ids are
// 1-based and assigned in registration order, so a deterministic startup
// registration sequence yields identical ids on every run — these are
// the ids the render queue sorts by and descriptor indices derive from.
using MeshId = uint32_t;
using ShaderId = uint32_t;
using MaterialId = uint32_t;
inline constexpr uint32_t kInvalidAssetId = 0;

// Import/normalization bounds for one mesh, computed once from its raw
// (unscaled, file-space) vertices at registration time.
//
// min/max are the raw AABB corners; center and extents are the AABB
// center and *half* extents (so a box collider can consume extents
// directly as its halfExtents); size is the full extent per axis.
// autoScaleFactor normalizes the mesh's longest full dimension to 1.0
// (1 / max(size)), which makes models exported in millimetres, inches
// or metres spawn at the same baseline size: a spawner applies
// userScale * autoScaleFactor as the transform scale and the raw
// center/extents as the collider offset/half-extents, and the world
// hitbox then matches the rendered bounds exactly.
struct MeshBounds {
    glm::vec3 min{-0.5f};
    glm::vec3 max{0.5f};
    glm::vec3 center{0.0f};
    glm::vec3 extents{0.5f}; // half extents
    glm::vec3 size{1.0f};    // full extents (max - min)
    float autoScaleFactor = 1.0f;

    [[nodiscard]] AABB aabb() const { return AABB{min, max}; }
};

// Computes MeshBounds from vertex positions (unit-cube defaults for an
// empty span). Shared by programmatic registration and file import so
// every mesh in the registry carries consistent bounds.
[[nodiscard]] ENGINE_ASSETS_API MeshBounds computeMeshBounds(std::span<const Vertex> vertices);

struct MeshAsset {
    MeshId id = kInvalidAssetId;
    std::string name;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    MeshBounds bounds{};
    uint64_t vramBytes = 0; // device-local bytes this mesh will occupy
};

struct ShaderAsset {
    ShaderId id = kInvalidAssetId;
    std::string name;
    std::vector<uint32_t> code; // SPIR-V words
    uint64_t byteSize = 0;
};

// Per-material lighting parameters for the directional shader. They are
// packed into two vec4s in the per-draw push constants (see
// Graphics/RenderQueue.h), so upgrading a material's look costs no extra
// descriptor traffic: specular color + shininess, then specular strength,
// rim strength, rim power, and ambient strength.
struct MaterialLighting {
    glm::vec3 specularColor{0.35f};
    float specularStrength = 0.35f;
    float shininess = 32.0f;
    float rimStrength = 0.10f;
    float rimPower = 3.0f;
    float ambientStrength = 0.28f;
};

// Everything a material needs: shader pair, a base color and lighting
// parameters (the first entries of the future material descriptor array),
// and its pipeline state.
struct MaterialDesc {
    std::string name;
    std::string vertexShader = "cube.vert.spv";
    std::string fragmentShader = "cube.frag.spv";
    glm::vec4 baseColor{1.0f};
    MaterialLighting lighting{};
    PipelineStateDesc pipeline{};
};

// Built-in sandbox palette. Each category of scene object gets its own
// base color and lighting response so silhouettes never merge: a dark
// asphalt floor, warm metallic falling cubes, cool teal stacking cubes, a
// blue-violet environment, and a bright player accent. Defined in
// EngineAssets so the application and the tests share one source of truth.
[[nodiscard]] ENGINE_ASSETS_API MaterialDesc makeSandboxFloorMaterial();
[[nodiscard]] ENGINE_ASSETS_API MaterialDesc makeSandboxCubeMaterial();
[[nodiscard]] ENGINE_ASSETS_API MaterialDesc makeSandboxStackMaterial();
[[nodiscard]] ENGINE_ASSETS_API MaterialDesc makeSandboxEnvironmentMaterial();
[[nodiscard]] ENGINE_ASSETS_API MaterialDesc makeSandboxPlayerMaterial();

struct MaterialAsset {
    MaterialId id = kInvalidAssetId;
    MaterialDesc desc;
    // Dense descriptor-table index (id - 1): the slot this material will
    // occupy in a descriptor-indexing / bindless material array, and the
    // index DrawItems carry so shaders can fetch material data without
    // per-draw descriptor rebinding.
    uint32_t descriptorIndex = 0;
};

// ---------------------------------------------------------------------------
// AssetManager
//
// Lightweight, startup-oriented asset registry. Assets are registered
// once (dense vectors + name maps, capacities reserved up front to avoid
// heap fragmentation) and then referenced everywhere by stable integer
// ids — the ECS stores only names/ids, systems resolve ids to data.
//
// VRAM discipline: registering GPU-bound data reserves its device-local
// footprint against the shared VramBudget (GTX 970 fast-segment cap)
// after a transient staging-budget check for the upload. Registration
// that would over-commit VRAM fails cleanly (invalid id) instead of
// letting the driver evict resources later. Reservations are released
// when the manager is destroyed.
// ---------------------------------------------------------------------------
class ENGINE_ASSETS_API AssetManager {
public:
    struct Capacity {
        size_t meshes = 1024;
        size_t shaders = 256;
        size_t materials = 1024;
    };

    AssetManager(VramBudget& vramBudget, VramBudget& stagingBudget);
    AssetManager(VramBudget& vramBudget, VramBudget& stagingBudget, Capacity capacity);
    ~AssetManager();

    AssetManager(const AssetManager&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;

    // Registers geometry (idempotent by name). Returns kInvalidAssetId
    // when the VRAM or staging budget would be exceeded.
    MeshId registerMesh(std::string name, std::span<const Vertex> vertices, std::span<const uint32_t> indices);
    // Imports a Wavefront OBJ model (positions, UVs, normals; polygon
    // faces are fan-triangulated, negative relative indices are
    // resolved, missing normals are generated flat per face) and
    // registers it under `name`. The mesh keeps its raw file-space
    // vertices; normalization lives in MeshAsset::bounds (see
    // MeshBounds) and is applied by the spawner, never baked into the
    // geometry. Returns kInvalidAssetId on I/O or parse failure.
    MeshId loadObj(const std::string& name, const std::filesystem::path& path);
    ShaderId registerShader(std::string name, std::span<const uint32_t> spirvCode);
    // Loads SPIR-V from a file and registers it. Invalid id on I/O failure.
    ShaderId loadShader(const std::string& name, const std::filesystem::path& path);
    MaterialId registerMaterial(MaterialDesc desc);

    [[nodiscard]] const MeshAsset* mesh(MeshId id) const;
    [[nodiscard]] const MeshAsset* mesh(std::string_view name) const;
    [[nodiscard]] const ShaderAsset* shader(ShaderId id) const;
    [[nodiscard]] const MaterialAsset* material(MaterialId id) const;
    [[nodiscard]] const MaterialAsset* material(std::string_view name) const;

    [[nodiscard]] size_t meshCount() const { return m_meshes.size(); }
    [[nodiscard]] size_t shaderCount() const { return m_shaders.size(); }
    [[nodiscard]] size_t materialCount() const { return m_materials.size(); }
    [[nodiscard]] uint64_t reservedVramBytes() const { return m_reservedVramBytes; }

private:
    VramBudget& m_vramBudget;
    VramBudget& m_stagingBudget;
    uint64_t m_reservedVramBytes = 0;

    std::vector<MeshAsset> m_meshes;
    std::vector<ShaderAsset> m_shaders;
    std::vector<MaterialAsset> m_materials;
    std::unordered_map<std::string, MeshId> m_meshIds;
    std::unordered_map<std::string, ShaderId> m_shaderIds;
    std::unordered_map<std::string, MaterialId> m_materialIds;
};

} // namespace engine
