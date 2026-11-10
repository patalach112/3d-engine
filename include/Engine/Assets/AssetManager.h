#pragma once
#include "Core/Export.h"

#include "Core/Concurrency/LockFreeRingBuffer.h"
#include "Geometry/Bounds.h"
#include "Geometry/Vertex.h"
#include "Graphics/PipelineState.h"

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine {

class JobSystem;
class VramBudget;

// Lifecycle of an asynchronously loaded asset (Phase 20). CPU-side
// parsing finishing on a worker does NOT advance Loading -> Loaded:
// only the main thread's processCompletedLoads() may publish data
// into the registry (the frame sync boundary where GPU resources
// would be allocated), so consumers never observe half-built assets.
enum class AssetState : uint8_t { Unloaded, Loading, Loaded, Failed };
enum class AssetKind : uint8_t { Mesh, Texture };

// Stable, dense asset handles. 0 is always the invalid id; real ids are
// 1-based and assigned in registration order, so a deterministic startup
// registration sequence yields identical ids on every run — these are
// the ids the render queue sorts by and descriptor indices derive from.
using MeshId = uint32_t;
using ShaderId = uint32_t;
using MaterialId = uint32_t;
using TextureId = uint32_t;
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

// CPU-side texture image (RGBA8). GPU upload is a main-thread
// reservation against the VRAM budget, exactly like meshes; the
// pixel payload is what a future sampler path will consume.
struct TextureAsset {
    TextureId id = kInvalidAssetId;
    std::string name;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels; // RGBA8, row-major, width*height*4 bytes
    uint64_t vramBytes = 0;
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

// Metallic-roughness PBR parameters (Phase 22). Texture slots name
// registered TextureAssets (empty = untextured; the packed push
// constants carry scalar factors, so untextured materials still
// shade fully). Maps follow the glTF packing: metallicRoughness
// stores roughness in G and metallic in B; occlusion uses R;
// normals are tangent-space RGB. Albedo texels are treated as sRGB
// and decoded in the shader.
struct MaterialPbr {
    float metallic = 0.0f;
    float roughness = 0.55f;
    float occlusionStrength = 1.0f;
    std::string albedoTexture;
    std::string metallicRoughnessTexture;
    std::string normalTexture;
    std::string occlusionTexture;
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
    MaterialPbr pbr{};
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
    // Phase 32: live material editing (the editor's Material
    // panel). Replaces the registered description in place — id
    // and dense descriptor slot preserved, material registry
    // version bumped — so the next frame's draw items shade with
    // the new PBR values. False when nothing is registered under
    // `name`. The name inside `desc` is ignored (no renames).
    bool updateMaterial(std::string_view name, const MaterialDesc& desc);

    [[nodiscard]] const MeshAsset* mesh(MeshId id) const;
    [[nodiscard]] const MeshAsset* mesh(std::string_view name) const;
    [[nodiscard]] const ShaderAsset* shader(ShaderId id) const;
    [[nodiscard]] const MaterialAsset* material(MaterialId id) const;
    [[nodiscard]] const MaterialAsset* material(std::string_view name) const;

    [[nodiscard]] size_t meshCount() const { return m_meshes.size(); }
    [[nodiscard]] size_t shaderCount() const { return m_shaders.size(); }
    [[nodiscard]] size_t materialCount() const { return m_materials.size(); }
    [[nodiscard]] size_t textureCount() const { return m_textures.size(); }
    // Dense texture table in id order (slot = id - 1): the renderer
    // mirrors this into the material texture array (Phase 22).
    [[nodiscard]] const std::vector<TextureAsset>& textures() const { return m_textures; }
    // Bumps on every texture register/replace, so GPU mirrors can
    // skip re-uploads when nothing changed.
    [[nodiscard]] uint64_t textureRegistryVersion() const { return m_textureRegistryVersion; }
    // Material-side twin: bumps on every material register or
    // updateMaterial() (Phase 32).
    [[nodiscard]] uint64_t materialRegistryVersion() const { return m_materialRegistryVersion; }
    [[nodiscard]] uint64_t reservedVramBytes() const { return m_reservedVramBytes; }

    // --- Textures (synchronous registration, Phase 20) -------------------
    // Idempotent by name. Returns kInvalidAssetId when the budgets
    // would be exceeded or the pixel payload mismatches w*h*4.
    TextureId registerTexture(std::string name, uint32_t width, uint32_t height,
                              std::span<const uint8_t> rgbaPixels);
    [[nodiscard]] const TextureAsset* texture(TextureId id) const;
    [[nodiscard]] const TextureAsset* texture(std::string_view name) const;

    // --- Async loading pipeline (Phase 20) --------------------------------
    // Declare a file-backed asset, then requestLoad() it: file I/O and
    // parsing (OBJ meshes, PPM textures) run on JobSystem workers
    // (inline on the caller when no JobSystem is set), completions
    // land in a lock-free queue, and processCompletedLoads() — the
    // main thread's frame sync boundary — publishes them into the
    // registry (budget-checked, ids stable across reloads). Until
    // then the asset reports Loading and meshOrFallback /
    // textureOrFallback serve the built-in error assets.
    //
    // Declaration, requestLoad, reloadAsset, state/error queries and
    // pollFileChanges are thread-safe. Registry getters (mesh,
    // texture, register*) stay main-thread APIs, as before.
    void setJobSystem(JobSystem* jobs) { m_jobs = jobs; }
    // Fired for Loading on the requesting thread, and for
    // Loaded/Failed on the thread calling processCompletedLoads.
    using AssetEventCallback = std::function<void(std::string_view name, AssetKind kind, AssetState state)>;
    void setAssetEventCallback(AssetEventCallback callback);

    bool declareMeshSource(const std::string& name, std::filesystem::path source);
    bool declareTextureSource(const std::string& name, std::filesystem::path source);
    // Starts loading a declared asset (Unloaded/Failed -> Loading).
    bool requestLoad(std::string_view name);
    // Re-loads a Loaded/Failed asset from its source file (hot
    // reload; the registry entry keeps its id, data is swapped at
    // the next processCompletedLoads).
    bool reloadAsset(std::string_view name);
    // Reloads every Loaded asset whose source file's mtime changed
    // since its load. Returns how many reloads were triggered.
    size_t pollFileChanges();

    [[nodiscard]] AssetState meshState(std::string_view name) const;
    [[nodiscard]] AssetState textureState(std::string_view name) const;
    [[nodiscard]] std::string assetError(std::string_view name) const;
    // Loads whose CPU work finished but which still wait for the
    // main-thread sync boundary.
    [[nodiscard]] size_t pendingUploadCount() const;
    // Publishes up to maxCount finished loads into the registry.
    // Returns how many were published.
    size_t processCompletedLoads(size_t maxCount = std::numeric_limits<size_t>::max());

    // Built-in error assets (magenta-tinted cube / checker texture),
    // and the accessors consumers should prefer: real data when the
    // registry has it, the fallback otherwise (unknown name, first
    // load still in flight, or load failed with nothing cached).
    [[nodiscard]] const MeshAsset& fallbackMesh() const { return m_fallbackMesh; }
    [[nodiscard]] const TextureAsset& fallbackTexture() const { return m_fallbackTexture; }
    [[nodiscard]] const MeshAsset* meshOrFallback(std::string_view name) const;
    [[nodiscard]] const TextureAsset* textureOrFallback(std::string_view name) const;

private:
    struct MeshLoadRecord {
        std::string name;
        std::filesystem::path source;
        std::atomic<AssetState> state{AssetState::Unloaded};
        std::mutex mutex; // guards the payload fields below
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        std::string error;
        std::filesystem::file_time_type writeTime{};
        MeshId meshId = kInvalidAssetId;
        bool cpuReady = false;
    };
    struct TextureLoadRecord {
        std::string name;
        std::filesystem::path source;
        std::atomic<AssetState> state{AssetState::Unloaded};
        std::mutex mutex; // guards the payload fields below
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint8_t> pixels;
        std::string error;
        std::filesystem::file_time_type writeTime{};
        TextureId textureId = kInvalidAssetId;
        bool cpuReady = false;
    };
    // Completion channel shared with worker jobs via shared_ptr, so
    // jobs finishing after the manager's destruction still land
    // safely (their completions are simply never processed).
    struct CompletionSink {
        concurrency::LockFreeRingBuffer<std::shared_ptr<MeshLoadRecord>> meshCompletions{256};
        concurrency::LockFreeRingBuffer<std::shared_ptr<TextureLoadRecord>> textureCompletions{256};
        std::mutex overflowMutex;
        std::vector<std::shared_ptr<MeshLoadRecord>> meshOverflow;
        std::vector<std::shared_ptr<TextureLoadRecord>> textureOverflow;
        std::atomic<size_t> pending{0};
    };

    void submitMeshLoad(const std::shared_ptr<MeshLoadRecord>& record);
    void submitTextureLoad(const std::shared_ptr<TextureLoadRecord>& record);
    void finishMeshLoad(const std::shared_ptr<MeshLoadRecord>& record);
    void finishTextureLoad(const std::shared_ptr<TextureLoadRecord>& record);
    void fireAssetEvent(std::string_view name, AssetKind kind, AssetState state);
    std::shared_ptr<MeshLoadRecord> findMeshRecord(std::string_view name) const;
    std::shared_ptr<TextureLoadRecord> findTextureRecord(std::string_view name) const;

    VramBudget& m_vramBudget;
    VramBudget& m_stagingBudget;
    uint64_t m_reservedVramBytes = 0;

    std::vector<MeshAsset> m_meshes;
    std::vector<ShaderAsset> m_shaders;
    std::vector<MaterialAsset> m_materials;
    std::vector<TextureAsset> m_textures;
    uint64_t m_textureRegistryVersion = 0;
    uint64_t m_materialRegistryVersion = 0;
    std::unordered_map<std::string, MeshId> m_meshIds;
    std::unordered_map<std::string, ShaderId> m_shaderIds;
    std::unordered_map<std::string, MaterialId> m_materialIds;
    std::unordered_map<std::string, TextureId> m_textureIds;

    JobSystem* m_jobs = nullptr;
    AssetEventCallback m_assetEventCallback;
    MeshAsset m_fallbackMesh;
    TextureAsset m_fallbackTexture;
    mutable std::mutex m_recordsMutex;
    std::unordered_map<std::string, std::shared_ptr<MeshLoadRecord>> m_meshRecords;
    std::unordered_map<std::string, std::shared_ptr<TextureLoadRecord>> m_textureRecords;
    std::shared_ptr<CompletionSink> m_sink = std::make_shared<CompletionSink>();
};

} // namespace engine
