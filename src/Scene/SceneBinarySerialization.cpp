// Scene binary serialization (Phase 30): the GBQ1 container (see
// Scene/SceneBinarySerialization.h for the format). The writer
// emits fixed-layout little-endian records per component type;
// the loader stages the whole file into plain structs first, so a
// malformed file is rejected before the scene is touched, then
// clears and rebuilds the registry in one pass. Floats travel as
// raw IEEE bits, so a round trip is bit-for-bit.

#include "Scene/SceneBinarySerialization.h"

#include "Core/Logger.h"
#include "Scene/Scene.h"

#include <array>
#include <cstring>
#include <fstream>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine {
namespace {

constexpr uint32_t kNoEntity = 0xFFFFFFFFu;
constexpr size_t kHeaderSize = 32;
constexpr size_t kTocEntrySize = 32;
constexpr uint32_t kMaxEntities = 1u << 20;
constexpr uint32_t kMaxSections = 4096;

// ---------------------------------------------------------------------------
// Little-endian byte writer / bounds-checked reader
// ---------------------------------------------------------------------------

struct ByteWriter {
    std::vector<std::byte> data;

    void bytes(const void* source, size_t count) {
        const auto* begin = static_cast<const std::byte*>(source);
        data.insert(data.end(), begin, begin + count);
    }
    void u8(uint8_t value) { data.push_back(static_cast<std::byte>(value)); }
    void boolean(bool value) { u8(value ? 1u : 0u); }
    void u32(uint32_t value) {
        for (int i = 0; i < 4; ++i) {
            data.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFFu));
        }
    }
    void u64(uint64_t value) {
        for (int i = 0; i < 8; ++i) {
            data.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFFu));
        }
    }
    void f32(float value) {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, 4);
        u32(bits);
    }
    void vec2(const glm::vec2& value) {
        f32(value.x);
        f32(value.y);
    }
    void vec3(const glm::vec3& value) {
        f32(value.x);
        f32(value.y);
        f32(value.z);
    }
    void quat(const glm::quat& value) {
        f32(value.w);
        f32(value.x);
        f32(value.y);
        f32(value.z);
    }
    void mat3(const glm::mat3& value) {
        for (int column = 0; column < 3; ++column) {
            for (int row = 0; row < 3; ++row) {
                f32(value[column][row]);
            }
        }
    }
    void str(std::string_view value) {
        u32(static_cast<uint32_t>(value.size()));
        bytes(value.data(), value.size());
    }
    void align8() {
        while (data.size() % 8 != 0) {
            data.push_back(std::byte{0});
        }
    }
};

struct ByteReader {
    const std::byte* cursor = nullptr;
    size_t left = 0;

    [[nodiscard]] bool require(size_t count) const { return left >= count; }
    bool bytes(void* dest, size_t count) {
        if (!require(count)) {
            return false;
        }
        std::memcpy(dest, cursor, count);
        cursor += count;
        left -= count;
        return true;
    }
    bool u8(uint8_t& value) {
        if (!require(1)) {
            return false;
        }
        value = static_cast<uint8_t>(*cursor);
        cursor += 1;
        left -= 1;
        return true;
    }
    bool boolean(bool& value) {
        uint8_t raw = 0;
        if (!u8(raw)) {
            return false;
        }
        value = raw != 0;
        return true;
    }
    bool u32(uint32_t& value) {
        std::byte raw[4];
        if (!bytes(raw, 4)) {
            return false;
        }
        value = 0;
        for (int i = 0; i < 4; ++i) {
            value |= static_cast<uint32_t>(static_cast<uint8_t>(raw[i])) << (8 * i);
        }
        return true;
    }
    bool u64(uint64_t& value) {
        std::byte raw[8];
        if (!bytes(raw, 8)) {
            return false;
        }
        value = 0;
        for (int i = 0; i < 8; ++i) {
            value |= static_cast<uint64_t>(static_cast<uint8_t>(raw[i])) << (8 * i);
        }
        return true;
    }
    bool f32(float& value) {
        uint32_t bits = 0;
        if (!u32(bits)) {
            return false;
        }
        std::memcpy(&value, &bits, 4);
        return true;
    }
    bool vec2(glm::vec2& value) { return f32(value.x) && f32(value.y); }
    bool vec3(glm::vec3& value) { return f32(value.x) && f32(value.y) && f32(value.z); }
    bool quat(glm::quat& value) { return f32(value.w) && f32(value.x) && f32(value.y) && f32(value.z); }
    bool mat3(glm::mat3& value) {
        for (int column = 0; column < 3; ++column) {
            for (int row = 0; row < 3; ++row) {
                if (!f32(value[column][row])) {
                    return false;
                }
            }
        }
        return true;
    }
    bool str(std::string& value) {
        uint32_t length = 0;
        if (!u32(length) || !require(length)) {
            return false;
        }
        value.assign(reinterpret_cast<const char*>(cursor), length);
        cursor += length;
        left -= length;
        return true;
    }
};

std::optional<std::vector<std::byte>> readWholeFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return std::nullopt;
    }
    const std::streamoff size = in.tellg();
    if (size < 0) {
        return std::nullopt;
    }
    std::vector<std::byte> bytes(static_cast<size_t>(size));
    in.seekg(0);
    if (size > 0 && !in.read(reinterpret_cast<char*>(bytes.data()), size)) {
        return std::nullopt;
    }
    return bytes;
}

// ---------------------------------------------------------------------------
// Table of contents
// ---------------------------------------------------------------------------

struct TocEntry {
    std::array<char, 4> type{};
    uint32_t version = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
};

bool parseHeaderAndToc(const std::vector<std::byte>& bytes, uint32_t& outVersion, std::vector<TocEntry>& outToc) {
    if (bytes.size() < kHeaderSize) {
        return false;
    }
    if (std::memcmp(bytes.data(), kGbqMagic, 4) != 0) {
        return false;
    }
    ByteReader reader{bytes.data(), bytes.size()};
    reader.cursor += 4;
    reader.left -= 4;
    uint32_t flags = 0;
    uint32_t sectionCount = 0;
    uint32_t reserved = 0;
    uint64_t tocOffset = 0;
    uint32_t reserved2 = 0;
    if (!reader.u32(outVersion) || !reader.u32(flags) || !reader.u32(sectionCount) || !reader.u32(reserved) ||
        !reader.u64(tocOffset) || !reader.u32(reserved2)) {
        return false;
    }
    if (outVersion != kGbqVersion || sectionCount > kMaxSections) {
        return false;
    }
    if (tocOffset > bytes.size() || sectionCount * kTocEntrySize > bytes.size() - static_cast<size_t>(tocOffset)) {
        return false;
    }
    ByteReader toc{bytes.data() + tocOffset, sectionCount * kTocEntrySize};
    outToc.clear();
    for (uint32_t i = 0; i < sectionCount; ++i) {
        TocEntry entry;
        uint64_t entryReserved = 0;
        if (!toc.bytes(entry.type.data(), 4) || !toc.u32(entry.version) || !toc.u64(entry.offset) ||
            !toc.u64(entry.size) || !toc.u64(entryReserved)) {
            return false;
        }
        if (entry.offset > bytes.size() || entry.size > bytes.size() - static_cast<size_t>(entry.offset)) {
            return false; // section reaches past the end of the file
        }
        outToc.push_back(entry);
    }
    return true;
}

const TocEntry* findSection(const std::vector<TocEntry>& toc, const char* type) {
    for (const TocEntry& entry : toc) {
        if (std::memcmp(entry.type.data(), type, 4) == 0) {
            return entry.version == kGbqVersion ? &entry : nullptr;
        }
    }
    return nullptr;
}

ByteReader sectionReader(const std::vector<std::byte>& bytes, const TocEntry& entry) {
    return ByteReader{bytes.data() + entry.offset, static_cast<size_t>(entry.size)};
}

// ---------------------------------------------------------------------------
// Staged scene (validated, not yet applied)
// ---------------------------------------------------------------------------

struct StagedCamera {
    CameraConfig config;
    glm::vec3 position{0.0f};
    float yawDegrees = 0.0f;
    float pitchDegrees = 0.0f;
    bool isActive = true;
};

struct StagedJoint {
    JointComponent joint;
    int32_t other = -1;
};

struct StagedEntity {
    int32_t parent = -1;
    bool hasTag = false;
    std::string tag;
    std::optional<TransformComponent> transform;
    std::optional<StagedCamera> camera;
    std::optional<MeshComponent> mesh;
    std::optional<SpinComponent> spin;
    std::optional<RigidBodyComponent> rigidBody;
    std::optional<ColliderComponent> collider;
    std::optional<GravityFieldComponent> gravityField;
    std::optional<StagedJoint> joint;
    std::optional<ThrusterComponent> thruster;
    std::optional<PointLightComponent> pointLight;
    std::optional<SpotLightComponent> spotLight;
    std::optional<std::string> script;
};

struct StagedScene {
    HierarchyDestroyMode destroyMode = HierarchyDestroyMode::Orphan;
    std::vector<StagedEntity> entities;
};

template <typename Read>
bool readComponentSection(const std::vector<std::byte>& bytes, const std::vector<TocEntry>& toc, const char* type,
                          uint32_t entityCount, Read&& readOne) {
    const TocEntry* section = findSection(toc, type);
    if (section == nullptr) {
        return true; // section absent (or its version was vetoed earlier)
    }
    ByteReader reader = sectionReader(bytes, *section);
    uint32_t count = 0;
    if (!reader.u32(count)) {
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t index = 0;
        if (!reader.u32(index) || index >= entityCount) {
            return false;
        }
        if (!readOne(reader, index)) {
            return false;
        }
    }
    return true;
}

// A known section present with the wrong version must reject the
// load (findSection hides it, which this check turns into a veto).
bool sectionVersionsAcceptable(const std::vector<TocEntry>& toc) {
    static constexpr const char* kKnown[] = {"META", "ENTY", "TAGC", "TRNF", "CAMR", "MESH", "SPIN", "RGBD",
                                             "CLDR", "GRVF", "JONT", "THRS", "PLGT", "SLGT", "NSCR"};
    for (const TocEntry& entry : toc) {
        for (const char* known : kKnown) {
            if (std::memcmp(entry.type.data(), known, 4) == 0 && entry.version != kGbqVersion) {
                return false;
            }
        }
    }
    return true;
}

bool parseGbq(const std::vector<std::byte>& bytes, StagedScene& staged) {
    uint32_t version = 0;
    std::vector<TocEntry> toc;
    if (!parseHeaderAndToc(bytes, version, toc) || !sectionVersionsAcceptable(toc)) {
        return false;
    }

    const TocEntry* enty = findSection(toc, "ENTY");
    if (enty == nullptr) {
        return false;
    }
    uint32_t entityTotal = 0;
    {
        ByteReader reader = sectionReader(bytes, *enty);
        if (!reader.u32(entityTotal) || entityTotal > kMaxEntities) {
            return false;
        }
        const uint32_t count = entityTotal;
        staged.entities.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t parent = 0;
            if (!reader.u32(parent)) {
                return false;
            }
            if (parent != kNoEntity && parent >= count) {
                return false;
            }
            staged.entities[i].parent = parent == kNoEntity ? -1 : static_cast<int32_t>(parent);
        }
    }
    const uint32_t entityCount = static_cast<uint32_t>(staged.entities.size());

    if (const TocEntry* meta = findSection(toc, "META"); meta != nullptr) {
        ByteReader reader = sectionReader(bytes, *meta);
        uint32_t mode = 0;
        uint32_t metaCount = 0;
        if (!reader.u32(mode) || !reader.u32(metaCount) || mode > 1 || metaCount != entityTotal) {
            return false;
        }
        staged.destroyMode = mode == 1 ? HierarchyDestroyMode::Cascade : HierarchyDestroyMode::Orphan;
    }

    if (!readComponentSection(bytes, toc, "TAGC", entityCount,
                              [&staged](ByteReader& reader, uint32_t index) {
                                  StagedEntity& entity = staged.entities[index];
                                  entity.hasTag = true;
                                  return reader.str(entity.tag);
                              })) {
        return false;
    }
    if (!readComponentSection(bytes, toc, "TRNF", entityCount,
                              [&staged](ByteReader& reader, uint32_t index) {
                                  TransformComponent value;
                                  bool ok = reader.vec3(value.position) && reader.vec3(value.rotationDegrees) &&
                                            reader.vec3(value.scale) && reader.quat(value.orientation) &&
                                            reader.boolean(value.useQuaternionOrientation);
                                  if (ok) {
                                      staged.entities[index].transform = value;
                                  }
                                  return ok;
                              })) {
        return false;
    }
    if (!readComponentSection(bytes, toc, "CAMR", entityCount,
                              [&staged](ByteReader& reader, uint32_t index) {
                                  StagedCamera camera;
                                  CameraConfig& config = camera.config;
                                  bool ok = reader.boolean(camera.isActive) && reader.vec3(config.startPosition) &&
                                            reader.f32(config.startYawDegrees) && reader.f32(config.startPitchDegrees) &&
                                            reader.f32(config.movementSpeed) && reader.f32(config.boostMultiplier) &&
                                            reader.f32(config.mouseSensitivity) && reader.boolean(config.invertY) &&
                                            reader.f32(config.lookSmoothingSeconds) &&
                                            reader.f32(config.movementSmoothingSeconds) &&
                                            reader.f32(config.pitchLimitDegrees) &&
                                            reader.f32(config.fieldOfViewDegrees) && reader.f32(config.nearPlane) &&
                                            reader.f32(config.farPlane) && reader.vec3(camera.position) &&
                                            reader.f32(camera.yawDegrees) && reader.f32(camera.pitchDegrees);
                                  if (ok) {
                                      staged.entities[index].camera = camera;
                                  }
                                  return ok;
                              })) {
        return false;
    }
    if (!readComponentSection(bytes, toc, "MESH", entityCount,
                              [&staged](ByteReader& reader, uint32_t index) {
                                  MeshComponent value;
                                  bool ok = reader.str(value.assetId) && reader.str(value.materialName) &&
                                            reader.boolean(value.visible) && reader.vec3(value.localBounds.min) &&
                                            reader.vec3(value.localBounds.max);
                                  if (ok) {
                                      staged.entities[index].mesh = value;
                                  }
                                  return ok;
                              })) {
        return false;
    }
    if (!readComponentSection(bytes, toc, "SPIN", entityCount,
                              [&staged](ByteReader& reader, uint32_t index) {
                                  SpinComponent value;
                                  const bool ok = reader.vec3(value.degreesPerSecond);
                                  if (ok) {
                                      staged.entities[index].spin = value;
                                  }
                                  return ok;
                              })) {
        return false;
    }
    if (!readComponentSection(bytes, toc, "RGBD", entityCount,
                              [&staged](ByteReader& reader, uint32_t index) {
                                  RigidBodyComponent value{1.0f};
                                  uint8_t flags = 0;
                                  bool ok = reader.vec3(value.velocity) && reader.vec3(value.angularVelocity) &&
                                            reader.vec3(value.angularMomentum) && reader.quat(value.orientation) &&
                                            reader.mat3(value.inertiaTensor) &&
                                            reader.mat3(value.inverseInertiaTensor) && reader.f32(value.mass) &&
                                            reader.f32(value.inverseMass) && reader.f32(value.restitution) &&
                                            reader.f32(value.friction) && reader.vec2(value.anisotropicFriction) &&
                                            reader.f32(value.gravityScale) && reader.f32(value.sleepTimer) &&
                                            reader.u8(flags);
                                  if (ok) {
                                      value.isStatic = (flags & 0x01u) != 0;
                                      value.useGravity = (flags & 0x02u) != 0;
                                      value.autoInertia = (flags & 0x04u) != 0;
                                      value.orientationInitialized = (flags & 0x08u) != 0;
                                      value.sleeping = (flags & 0x10u) != 0;
                                      value.allowSleep = (flags & 0x20u) != 0;
                                      staged.entities[index].rigidBody = value;
                                  }
                                  return ok;
                              })) {
        return false;
    }
    if (!readComponentSection(bytes, toc, "CLDR", entityCount,
                              [&staged](ByteReader& reader, uint32_t index) {
                                  ColliderComponent value;
                                  uint8_t type = 0;
                                  uint8_t hullCount = 0;
                                  bool ok = reader.u8(type) && reader.vec3(value.halfExtents) &&
                                            reader.vec3(value.centerOffset) && reader.f32(value.radius) &&
                                            reader.f32(value.capsuleHalfHeight) && reader.vec3(value.planeNormal) &&
                                            reader.f32(value.planeOffset) && reader.boolean(value.isTrigger) &&
                                            reader.u32(value.layer) && reader.u32(value.mask) && reader.u8(hullCount);
                                  if (ok && type > static_cast<uint8_t>(ColliderComponent::Type::ConvexHull)) {
                                      ok = false;
                                  }
                                  if (ok && hullCount > 8) {
                                      ok = false;
                                  }
                                  for (uint8_t i = 0; ok && i < hullCount; ++i) {
                                      ok = reader.vec3(value.hullPoints[i]);
                                  }
                                  if (ok) {
                                      value.type = static_cast<ColliderComponent::Type>(type);
                                      value.hullPointCount = hullCount;
                                      staged.entities[index].collider = value;
                                  }
                                  return ok;
                              })) {
        return false;
    }
    if (!readComponentSection(bytes, toc, "GRVF", entityCount,
                              [&staged](ByteReader& reader, uint32_t index) {
                                  GravityFieldComponent value;
                                  const bool ok = reader.f32(value.gravitationalParameter) && reader.f32(value.radius) &&
                                                  reader.boolean(value.enabled);
                                  if (ok) {
                                      staged.entities[index].gravityField = value;
                                  }
                                  return ok;
                              })) {
        return false;
    }
    if (!readComponentSection(bytes, toc, "JONT", entityCount,
                              [&staged, entityCount](ByteReader& reader, uint32_t index) {
                                  StagedJoint stagedJoint;
                                  JointComponent& value = stagedJoint.joint;
                                  uint8_t type = 0;
                                  uint32_t other = 0;
                                  bool ok = reader.u8(type) && reader.u32(other) && reader.vec3(value.anchorA) &&
                                            reader.vec3(value.anchorB) && reader.vec3(value.hingeAxis) &&
                                            reader.f32(value.restLength) && reader.f32(value.minDistance) &&
                                            reader.f32(value.maxDistance) && reader.f32(value.minAngleDegrees) &&
                                            reader.f32(value.maxAngleDegrees) && reader.boolean(value.enableLimits) &&
                                            reader.f32(value.compliance) && reader.f32(value.stiffness) &&
                                            reader.f32(value.damping) && reader.boolean(value.enabled);
                                  if (ok && type > static_cast<uint8_t>(JointType::SpringDamper)) {
                                      ok = false;
                                  }
                                  if (ok && other != kNoEntity && other >= entityCount) {
                                      ok = false;
                                  }
                                  if (ok) {
                                      value.type = static_cast<JointType>(type);
                                      stagedJoint.other = other == kNoEntity ? -1 : static_cast<int32_t>(other);
                                      staged.entities[index].joint = stagedJoint;
                                  }
                                  return ok;
                              })) {
        return false;
    }
    if (!readComponentSection(bytes, toc, "THRS", entityCount,
                              [&staged](ByteReader& reader, uint32_t index) {
                                  ThrusterComponent value;
                                  const bool ok = reader.vec3(value.force) && reader.vec3(value.torque) &&
                                                  reader.vec3(value.localPosition) && reader.f32(value.throttle) &&
                                                  reader.boolean(value.enabled);
                                  if (ok) {
                                      staged.entities[index].thruster = value;
                                  }
                                  return ok;
                              })) {
        return false;
    }
    if (!readComponentSection(bytes, toc, "PLGT", entityCount,
                              [&staged](ByteReader& reader, uint32_t index) {
                                  PointLightComponent value;
                                  const bool ok = reader.vec3(value.color) && reader.f32(value.intensity) &&
                                                  reader.f32(value.range) && reader.boolean(value.enabled);
                                  if (ok) {
                                      staged.entities[index].pointLight = value;
                                  }
                                  return ok;
                              })) {
        return false;
    }
    if (!readComponentSection(bytes, toc, "SLGT", entityCount,
                              [&staged](ByteReader& reader, uint32_t index) {
                                  SpotLightComponent value;
                                  const bool ok = reader.vec3(value.color) && reader.f32(value.intensity) &&
                                                  reader.f32(value.range) && reader.f32(value.innerConeRadians) &&
                                                  reader.f32(value.outerConeRadians) && reader.boolean(value.enabled);
                                  if (ok) {
                                      staged.entities[index].spotLight = value;
                                  }
                                  return ok;
                              })) {
        return false;
    }
    if (!readComponentSection(bytes, toc, "NSCR", entityCount,
                              [&staged](ByteReader& reader, uint32_t index) {
                                  std::string name;
                                  const bool ok = reader.str(name);
                                  if (ok) {
                                      staged.entities[index].script = std::move(name);
                                  }
                                  return ok;
                              })) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

struct SectionData {
    std::array<char, 4> type{};
    ByteWriter body;
};

void addSection(std::vector<SectionData>& sections, const char* type, ByteWriter&& body) {
    SectionData section;
    std::memcpy(section.type.data(), type, 4);
    section.body = std::move(body);
    sections.push_back(std::move(section));
}

} // namespace

bool Scene::saveToGbqFile(const std::string& filepath) const {
    // Gather every entity that carries anything (same reach as the
    // JSON writer), in a stable order for byte-identical re-saves.
    std::vector<entt::entity> entities;
    std::unordered_map<uint32_t, uint32_t> indexOf;
    const auto collect = [&](auto view) {
        for (const entt::entity entity : view) {
            if (indexOf.emplace(entt::to_integral(entity), static_cast<uint32_t>(entities.size())).second) {
                entities.push_back(entity);
            }
        }
    };
    collect(m_registry.view<TagComponent>());
    collect(m_registry.view<TransformComponent>());
    collect(m_registry.view<CameraComponent>());
    collect(m_registry.view<MeshComponent>());
    collect(m_registry.view<SpinComponent>());
    collect(m_registry.view<PointLightComponent>());
    collect(m_registry.view<SpotLightComponent>());
    collect(m_registry.view<NativeScriptComponent>());
    collect(m_registry.view<RigidBodyComponent>());
    collect(m_registry.view<ColliderComponent>());
    collect(m_registry.view<GravityFieldComponent>());
    collect(m_registry.view<JointComponent>());
    collect(m_registry.view<ThrusterComponent>());
    collect(m_registry.view<ParentComponent>());
    collect(m_registry.view<ChildrenComponent>());

    std::vector<SectionData> sections;
    {
        ByteWriter meta;
        meta.u32(m_hierarchyDestroyMode == HierarchyDestroyMode::Cascade ? 1u : 0u);
        meta.u32(static_cast<uint32_t>(entities.size()));
        addSection(sections, "META", std::move(meta));
    }
    {
        ByteWriter enty;
        enty.u32(static_cast<uint32_t>(entities.size()));
        for (const entt::entity entity : entities) {
            uint32_t parent = kNoEntity;
            if (const ParentComponent* parentComponent = m_registry.try_get<ParentComponent>(entity);
                parentComponent != nullptr && parentComponent->parent != entt::null &&
                m_registry.valid(parentComponent->parent)) {
                if (const auto found = indexOf.find(entt::to_integral(parentComponent->parent));
                    found != indexOf.end()) {
                    parent = found->second;
                }
            }
            enty.u32(parent);
        }
        addSection(sections, "ENTY", std::move(enty));
    }

    const auto writeSection = [&](const char* type, auto&& writeRecord) {
        ByteWriter records;
        uint32_t count = 0;
        for (const entt::entity entity : entities) {
            if (writeRecord(records, entity, indexOf.at(entt::to_integral(entity)))) {
                ++count;
            }
        }
        if (count == 0) {
            return;
        }
        ByteWriter body;
        body.u32(count);
        body.bytes(records.data.data(), records.data.size());
        addSection(sections, type, std::move(body));
    };

    writeSection("TAGC", [&](ByteWriter& out, entt::entity entity, uint32_t index) {
        const TagComponent* value = m_registry.try_get<TagComponent>(entity);
        if (value == nullptr) {
            return false;
        }
        out.u32(index);
        out.str(value->tag);
        return true;
    });
    writeSection("TRNF", [&](ByteWriter& out, entt::entity entity, uint32_t index) {
        const TransformComponent* value = m_registry.try_get<TransformComponent>(entity);
        if (value == nullptr) {
            return false;
        }
        out.u32(index);
        out.vec3(value->position);
        out.vec3(value->rotationDegrees);
        out.vec3(value->scale);
        out.quat(value->orientation);
        out.boolean(value->useQuaternionOrientation);
        return true;
    });
    writeSection("CAMR", [&](ByteWriter& out, entt::entity entity, uint32_t index) {
        const CameraComponent* component = m_registry.try_get<CameraComponent>(entity);
        if (component == nullptr) {
            return false;
        }
        const CameraConfig& config = component->camera.config();
        out.u32(index);
        out.boolean(component->isActive);
        out.vec3(config.startPosition);
        out.f32(config.startYawDegrees);
        out.f32(config.startPitchDegrees);
        out.f32(config.movementSpeed);
        out.f32(config.boostMultiplier);
        out.f32(config.mouseSensitivity);
        out.boolean(config.invertY);
        out.f32(config.lookSmoothingSeconds);
        out.f32(config.movementSmoothingSeconds);
        out.f32(config.pitchLimitDegrees);
        out.f32(config.fieldOfViewDegrees);
        out.f32(config.nearPlane);
        out.f32(config.farPlane);
        out.vec3(component->camera.position());
        out.f32(component->camera.yawDegrees());
        out.f32(component->camera.pitchDegrees());
        return true;
    });
    writeSection("MESH", [&](ByteWriter& out, entt::entity entity, uint32_t index) {
        const MeshComponent* value = m_registry.try_get<MeshComponent>(entity);
        if (value == nullptr) {
            return false;
        }
        out.u32(index);
        out.str(value->assetId);
        out.str(value->materialName);
        out.boolean(value->visible);
        out.vec3(value->localBounds.min);
        out.vec3(value->localBounds.max);
        return true;
    });
    writeSection("SPIN", [&](ByteWriter& out, entt::entity entity, uint32_t index) {
        const SpinComponent* value = m_registry.try_get<SpinComponent>(entity);
        if (value == nullptr) {
            return false;
        }
        out.u32(index);
        out.vec3(value->degreesPerSecond);
        return true;
    });
    writeSection("RGBD", [&](ByteWriter& out, entt::entity entity, uint32_t index) {
        const RigidBodyComponent* value = m_registry.try_get<RigidBodyComponent>(entity);
        if (value == nullptr) {
            return false;
        }
        out.u32(index);
        out.vec3(value->velocity);
        out.vec3(value->angularVelocity);
        out.vec3(value->angularMomentum);
        out.quat(value->orientation);
        out.mat3(value->inertiaTensor);
        out.mat3(value->inverseInertiaTensor);
        out.f32(value->mass);
        out.f32(value->inverseMass);
        out.f32(value->restitution);
        out.f32(value->friction);
        out.vec2(value->anisotropicFriction);
        out.f32(value->gravityScale);
        out.f32(value->sleepTimer);
        const uint8_t flags = static_cast<uint8_t>((value->isStatic ? 0x01u : 0u) | (value->useGravity ? 0x02u : 0u) |
                                                   (value->autoInertia ? 0x04u : 0u) |
                                                   (value->orientationInitialized ? 0x08u : 0u) |
                                                   (value->sleeping ? 0x10u : 0u) | (value->allowSleep ? 0x20u : 0u));
        out.u8(flags);
        return true;
    });
    writeSection("CLDR", [&](ByteWriter& out, entt::entity entity, uint32_t index) {
        const ColliderComponent* value = m_registry.try_get<ColliderComponent>(entity);
        if (value == nullptr) {
            return false;
        }
        out.u32(index);
        out.u8(static_cast<uint8_t>(value->type));
        out.vec3(value->halfExtents);
        out.vec3(value->centerOffset);
        out.f32(value->radius);
        out.f32(value->capsuleHalfHeight);
        out.vec3(value->planeNormal);
        out.f32(value->planeOffset);
        out.boolean(value->isTrigger);
        out.u32(value->layer);
        out.u32(value->mask);
        const uint8_t hullCount = static_cast<uint8_t>(std::min<size_t>(value->hullPointCount, 8));
        out.u8(hullCount);
        for (uint8_t i = 0; i < hullCount; ++i) {
            out.vec3(value->hullPoints[i]);
        }
        return true;
    });
    writeSection("GRVF", [&](ByteWriter& out, entt::entity entity, uint32_t index) {
        const GravityFieldComponent* value = m_registry.try_get<GravityFieldComponent>(entity);
        if (value == nullptr) {
            return false;
        }
        out.u32(index);
        out.f32(value->gravitationalParameter);
        out.f32(value->radius);
        out.boolean(value->enabled);
        return true;
    });
    writeSection("JONT", [&](ByteWriter& out, entt::entity entity, uint32_t index) {
        const JointComponent* value = m_registry.try_get<JointComponent>(entity);
        if (value == nullptr) {
            return false;
        }
        uint32_t other = kNoEntity;
        if (value->other != entt::null && m_registry.valid(value->other)) {
            if (const auto found = indexOf.find(entt::to_integral(value->other)); found != indexOf.end()) {
                other = found->second;
            }
        }
        out.u32(index);
        out.u8(static_cast<uint8_t>(value->type));
        out.u32(other);
        out.vec3(value->anchorA);
        out.vec3(value->anchorB);
        out.vec3(value->hingeAxis);
        out.f32(value->restLength);
        out.f32(value->minDistance);
        out.f32(value->maxDistance);
        out.f32(value->minAngleDegrees);
        out.f32(value->maxAngleDegrees);
        out.boolean(value->enableLimits);
        out.f32(value->compliance);
        out.f32(value->stiffness);
        out.f32(value->damping);
        out.boolean(value->enabled);
        return true;
    });
    writeSection("THRS", [&](ByteWriter& out, entt::entity entity, uint32_t index) {
        const ThrusterComponent* value = m_registry.try_get<ThrusterComponent>(entity);
        if (value == nullptr) {
            return false;
        }
        out.u32(index);
        out.vec3(value->force);
        out.vec3(value->torque);
        out.vec3(value->localPosition);
        out.f32(value->throttle);
        out.boolean(value->enabled);
        return true;
    });
    writeSection("PLGT", [&](ByteWriter& out, entt::entity entity, uint32_t index) {
        const PointLightComponent* value = m_registry.try_get<PointLightComponent>(entity);
        if (value == nullptr) {
            return false;
        }
        out.u32(index);
        out.vec3(value->color);
        out.f32(value->intensity);
        out.f32(value->range);
        out.boolean(value->enabled);
        return true;
    });
    writeSection("SLGT", [&](ByteWriter& out, entt::entity entity, uint32_t index) {
        const SpotLightComponent* value = m_registry.try_get<SpotLightComponent>(entity);
        if (value == nullptr) {
            return false;
        }
        out.u32(index);
        out.vec3(value->color);
        out.f32(value->intensity);
        out.f32(value->range);
        out.f32(value->innerConeRadians);
        out.f32(value->outerConeRadians);
        out.boolean(value->enabled);
        return true;
    });
    writeSection("NSCR", [&](ByteWriter& out, entt::entity entity, uint32_t index) {
        const NativeScriptComponent* value = m_registry.try_get<NativeScriptComponent>(entity);
        if (value == nullptr) {
            return false;
        }
        out.u32(index);
        out.str(value->scriptName);
        return true;
    });

    // Assemble: header, TOC, then 8-byte-aligned section payloads.
    ByteWriter file;
    file.bytes(kGbqMagic, 4);
    file.u32(kGbqVersion);
    file.u32(0); // flags
    file.u32(static_cast<uint32_t>(sections.size()));
    file.u32(0); // reserved
    file.u64(kHeaderSize); // TOC immediately follows the header
    file.u32(0);           // reserved
    const size_t tocBytes = sections.size() * kTocEntrySize;
    size_t offset = kHeaderSize + tocBytes;
    std::vector<uint64_t> offsets;
    for (const SectionData& section : sections) {
        offset = (offset + 7u) & ~static_cast<size_t>(7u);
        offsets.push_back(offset);
        offset += section.body.data.size();
    }
    for (size_t i = 0; i < sections.size(); ++i) {
        file.bytes(sections[i].type.data(), 4);
        file.u32(kGbqVersion);
        file.u64(offsets[i]);
        file.u64(sections[i].body.data.size());
        file.u64(0); // reserved
    }
    for (size_t i = 0; i < sections.size(); ++i) {
        while (file.data.size() < offsets[i]) {
            file.data.push_back(std::byte{0});
        }
        file.bytes(sections[i].body.data.data(), sections[i].body.data.size());
    }

    std::ofstream out(filepath, std::ios::binary | std::ios::trunc);
    if (!out) {
        ENGINE_LOG_WARNING("Scene GBQ: cannot open '" << filepath << "' for writing");
        return false;
    }
    out.write(reinterpret_cast<const char*>(file.data.data()), static_cast<std::streamsize>(file.data.size()));
    if (!out) {
        ENGINE_LOG_WARNING("Scene GBQ: failed while writing '" << filepath << "'");
        return false;
    }
    ENGINE_LOG_DEBUG("Scene GBQ: saved " << entities.size() << " entities to '" << filepath << "'");
    return true;
}

bool Scene::loadFromGbqFile(const std::string& filepath) {
    const std::optional<std::vector<std::byte>> bytes = readWholeFile(filepath);
    if (!bytes.has_value()) {
        ENGINE_LOG_WARNING("Scene GBQ: cannot open '" << filepath << "' for reading");
        return false;
    }
    StagedScene staged;
    if (!parseGbq(*bytes, staged)) {
        ENGINE_LOG_WARNING("Scene GBQ: '" << filepath << "' is not a valid GBQ1 scene (scene left untouched)");
        return false;
    }

    // Validated: clear wholesale (silently, like loadFromString)
    // and rebuild in file order.
    m_registry.clear();
    m_entityCount = 0;
    m_hierarchyDestroyMode = staged.destroyMode;

    std::vector<entt::entity> handles;
    handles.reserve(staged.entities.size());
    for (const StagedEntity& stagedEntity : staged.entities) {
        const entt::entity handle = m_registry.create();
        ++m_entityCount;
        handles.push_back(handle);
        if (stagedEntity.hasTag) {
            m_registry.emplace<TagComponent>(handle, TagComponent{stagedEntity.tag});
        }
        if (stagedEntity.transform.has_value()) {
            m_registry.emplace<TransformComponent>(handle, *stagedEntity.transform);
        }
        if (stagedEntity.camera.has_value()) {
            CameraComponent& camera = m_registry.emplace<CameraComponent>(handle, stagedEntity.camera->config);
            camera.camera.setPosition(stagedEntity.camera->position);
            camera.camera.setOrientation(stagedEntity.camera->yawDegrees, stagedEntity.camera->pitchDegrees);
            camera.isActive = stagedEntity.camera->isActive;
        }
        if (stagedEntity.mesh.has_value()) {
            m_registry.emplace<MeshComponent>(handle, *stagedEntity.mesh);
        }
        if (stagedEntity.spin.has_value()) {
            m_registry.emplace<SpinComponent>(handle, *stagedEntity.spin);
        }
        if (stagedEntity.rigidBody.has_value()) {
            m_registry.emplace<RigidBodyComponent>(handle, *stagedEntity.rigidBody);
        }
        if (stagedEntity.collider.has_value()) {
            m_registry.emplace<ColliderComponent>(handle, *stagedEntity.collider);
        }
        if (stagedEntity.gravityField.has_value()) {
            m_registry.emplace<GravityFieldComponent>(handle, *stagedEntity.gravityField);
        }
        if (stagedEntity.joint.has_value()) {
            JointComponent joint = stagedEntity.joint->joint;
            joint.other = entt::null; // patched below, once all handles exist
            m_registry.emplace<JointComponent>(handle, joint);
        }
        if (stagedEntity.thruster.has_value()) {
            m_registry.emplace<ThrusterComponent>(handle, *stagedEntity.thruster);
        }
        if (stagedEntity.pointLight.has_value()) {
            m_registry.emplace<PointLightComponent>(handle, *stagedEntity.pointLight);
        }
        if (stagedEntity.spotLight.has_value()) {
            m_registry.emplace<SpotLightComponent>(handle, *stagedEntity.spotLight);
        }
        if (stagedEntity.script.has_value()) {
            m_registry.emplace<NativeScriptComponent>(handle, *stagedEntity.script);
        }
    }
    // Joint links point at entities that may be created later in
    // the file order: remap them now that every handle exists.
    for (size_t i = 0; i < staged.entities.size(); ++i) {
        if (staged.entities[i].joint.has_value() && staged.entities[i].joint->other >= 0) {
            m_registry.get<JointComponent>(handles[i]).other =
                handles[static_cast<size_t>(staged.entities[i].joint->other)];
        }
    }
    for (size_t i = 0; i < staged.entities.size(); ++i) {
        const int32_t parent = staged.entities[i].parent;
        if (parent >= 0) {
            setParent(Entity{handles[i], this}, Entity{handles[static_cast<size_t>(parent)], this});
        }
    }
    ENGINE_LOG_INFO("Scene GBQ: loaded " << handles.size() << " entities from '" << filepath << "'");
    return true;
}

bool inspectGbqFile(const std::filesystem::path& path, GbqFileInfo& outInfo) {
    const std::optional<std::vector<std::byte>> bytes = readWholeFile(path);
    if (!bytes.has_value()) {
        return false;
    }
    uint32_t version = 0;
    std::vector<TocEntry> toc;
    if (!parseHeaderAndToc(*bytes, version, toc)) {
        return false;
    }
    outInfo = GbqFileInfo{};
    outInfo.version = version;
    outInfo.fileSize = bytes->size();
    for (const TocEntry& entry : toc) {
        GbqSectionInfo info;
        info.type.assign(entry.type.data(), 4);
        info.version = entry.version;
        info.offset = entry.offset;
        info.size = entry.size;
        outInfo.sections.push_back(std::move(info));
    }
    if (const TocEntry* meta = findSection(toc, "META"); meta != nullptr && meta->size >= 8) {
        ByteReader reader = sectionReader(*bytes, *meta);
        uint32_t mode = 0;
        (void)reader.u32(mode);
        (void)reader.u32(outInfo.entityCount);
    }
    return true;
}

bool serializeScene(const Scene& scene, const std::filesystem::path& path) {
    return scene.saveToGbqFile(path.string());
}

bool deserializeScene(Scene& scene, const std::filesystem::path& path) {
    return scene.loadFromGbqFile(path.string());
}

} // namespace engine
