// Standalone packaging (Phase 33): the GBQB bundle writer/reader
// and the runtime boot loader. See Scene/ProjectExport.h for the
// format. Payloads compress with a small flag-byte LZSS (12-bit
// window, 3..18 byte matches — no external zlib dependency) and
// carry CRC-32 checksums so a corrupted bundle fails loudly at
// extract time instead of crashing the player.

#include "Scene/ProjectExport.h"

#include "Assets/AssetManager.h"
#include "Core/Json.h"
#include "Core/Logger.h"
#include "Geometry/Vertex.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>

namespace engine {
namespace {

constexpr size_t kHeaderSize = 32;

// ---------------------------------------------------------------------------
// Little-endian field helpers
// ---------------------------------------------------------------------------

void putU32(std::vector<std::byte>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFFu));
    }
}

void patchU32(std::vector<std::byte>& data, size_t offset, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        data[offset + i] = static_cast<std::byte>((value >> (8 * i)) & 0xFFu);
    }
}

void patchU64(std::vector<std::byte>& data, size_t offset, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        data[offset + i] = static_cast<std::byte>((value >> (8 * i)) & 0xFFu);
    }
}

uint32_t getU32(const std::byte* data) {
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<uint32_t>(static_cast<uint8_t>(data[i])) << (8 * i);
    }
    return value;
}

uint64_t getU64(const std::byte* data) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(static_cast<uint8_t>(data[i])) << (8 * i);
    }
    return value;
}

std::optional<std::vector<std::byte>> readFile(const std::filesystem::path& path) {
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

bool writeFile(const std::filesystem::path& path, const std::byte* data, size_t size) {
    std::error_code ec;
    if (const std::filesystem::path parent = path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(out);
}

// ---------------------------------------------------------------------------
// LZSS compression (flag byte per 8 tokens; bit 1 = literal, bit 0 =
// 2-byte match: 12-bit offset back (+1), 4-bit length (+3)).
// ---------------------------------------------------------------------------

std::vector<std::byte> lzCompress(const std::byte* data, size_t size) {
    constexpr int kHashBits = 15;
    constexpr size_t kHashSize = size_t{1} << kHashBits;
    constexpr size_t kWindow = 4096;
    constexpr size_t kMaxMatch = 18;

    std::vector<std::byte> out;
    out.reserve(size / 2 + 16);
    std::vector<int32_t> head(kHashSize, -1);
    std::vector<int32_t> prev(size == 0 ? 1 : size, -1);

    const auto hashAt = [data](size_t i) {
        const uint32_t v = (static_cast<uint32_t>(static_cast<uint8_t>(data[i])) << 16) |
                           (static_cast<uint32_t>(static_cast<uint8_t>(data[i + 1])) << 8) |
                           static_cast<uint32_t>(static_cast<uint8_t>(data[i + 2]));
        return (v * 2654435761u >> (32 - kHashBits)) & (kHashSize - 1);
    };
    const auto insertAt = [&](size_t i) {
        if (i + 2 < size) {
            const size_t h = hashAt(i);
            prev[i] = head[h];
            head[h] = static_cast<int32_t>(i);
        }
    };

    size_t i = 0;
    while (i < size) {
        const size_t flagPos = out.size();
        out.push_back(std::byte{0});
        uint8_t flags = 0;
        for (int bit = 0; bit < 8 && i < size; ++bit) {
            size_t bestLength = 0;
            size_t bestOffset = 0;
            if (size - i >= 3) {
                int32_t candidate = head[hashAt(i)];
                for (int tries = 0; candidate >= 0 && tries < 32; ++tries) {
                    const size_t offset = i - static_cast<size_t>(candidate);
                    if (offset > kWindow) {
                        break;
                    }
                    size_t length = 0;
                    while (length < kMaxMatch && i + length < size && data[candidate + length] == data[i + length]) {
                        ++length;
                    }
                    if (length >= 3 && length > bestLength) {
                        bestLength = length;
                        bestOffset = offset;
                        if (length == kMaxMatch) {
                            break;
                        }
                    }
                    candidate = prev[static_cast<size_t>(candidate)];
                }
            }
            if (bestLength >= 3) {
                out.push_back(static_cast<std::byte>((bestOffset - 1) >> 4));
                out.push_back(static_cast<std::byte>(((bestOffset - 1) & 0xFu) << 4 | (bestLength - 3)));
                insertAt(i);
                i += bestLength;
            } else {
                flags = static_cast<uint8_t>(flags | (1u << bit));
                out.push_back(data[i]);
                insertAt(i);
                i += 1;
            }
        }
        out[flagPos] = static_cast<std::byte>(flags);
    }
    return out;
}

bool lzDecompress(const std::byte* data, size_t size, size_t rawSize, std::vector<std::byte>& out) {
    out.clear();
    out.reserve(rawSize);
    size_t i = 0;
    while (out.size() < rawSize) {
        if (i >= size) {
            return false;
        }
        const uint8_t flags = static_cast<uint8_t>(data[i++]);
        for (int bit = 0; bit < 8 && out.size() < rawSize; ++bit) {
            if ((flags & (1u << bit)) != 0) {
                if (i >= size) {
                    return false;
                }
                out.push_back(data[i++]);
            } else {
                if (i + 1 >= size) {
                    return false;
                }
                const size_t offset = ((static_cast<size_t>(static_cast<uint8_t>(data[i])) << 4) |
                                       (static_cast<uint8_t>(data[i + 1]) >> 4)) +
                                      1;
                const size_t length = (static_cast<uint8_t>(data[i + 1]) & 0xFu) + 3;
                i += 2;
                if (offset > out.size()) {
                    return false;
                }
                const size_t source = out.size() - offset;
                for (size_t k = 0; k < length; ++k) {
                    out.push_back(out[source + k]);
                }
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// MaterialDesc <-> JSON
// ---------------------------------------------------------------------------

json::Value vecToJson(const float* values, size_t count) {
    json::Value array = json::Value::array();
    for (size_t i = 0; i < count; ++i) {
        array.push(json::Value::number(values[i]));
    }
    return array;
}

void vecFromJson(const json::Value& value, float* out, size_t count) {
    if (value.type() != json::Value::Type::Array) {
        return;
    }
    const std::vector<json::Value>& items = value.items();
    for (size_t i = 0; i < count && i < items.size(); ++i) {
        out[i] = static_cast<float>(items[i].asNumber(out[i]));
    }
}

json::Value materialToJson(const MaterialDesc& desc) {
    json::Value root = json::Value::object();
    root.set("name", json::Value::string(desc.name));
    root.set("vertexShader", json::Value::string(desc.vertexShader));
    root.set("fragmentShader", json::Value::string(desc.fragmentShader));
    root.set("baseColor", vecToJson(&desc.baseColor.x, 4));

    json::Value lighting = json::Value::object();
    lighting.set("specularColor", vecToJson(&desc.lighting.specularColor.x, 3));
    lighting.set("specularStrength", json::Value::number(desc.lighting.specularStrength));
    lighting.set("shininess", json::Value::number(desc.lighting.shininess));
    lighting.set("rimStrength", json::Value::number(desc.lighting.rimStrength));
    lighting.set("rimPower", json::Value::number(desc.lighting.rimPower));
    lighting.set("ambientStrength", json::Value::number(desc.lighting.ambientStrength));
    root.set("lighting", lighting);

    json::Value pbr = json::Value::object();
    pbr.set("metallic", json::Value::number(desc.pbr.metallic));
    pbr.set("roughness", json::Value::number(desc.pbr.roughness));
    pbr.set("occlusionStrength", json::Value::number(desc.pbr.occlusionStrength));
    pbr.set("albedoTexture", json::Value::string(desc.pbr.albedoTexture));
    pbr.set("metallicRoughnessTexture", json::Value::string(desc.pbr.metallicRoughnessTexture));
    pbr.set("normalTexture", json::Value::string(desc.pbr.normalTexture));
    pbr.set("occlusionTexture", json::Value::string(desc.pbr.occlusionTexture));
    root.set("pbr", pbr);

    json::Value pipeline = json::Value::object();
    pipeline.set("topology", json::Value::number(static_cast<double>(desc.pipeline.topology)));
    pipeline.set("polygonMode", json::Value::number(static_cast<double>(desc.pipeline.polygonMode)));
    pipeline.set("cullMode", json::Value::number(static_cast<double>(desc.pipeline.cullMode)));
    pipeline.set("depthTestEnable", json::Value::boolean(desc.pipeline.depthTestEnable));
    pipeline.set("depthWriteEnable", json::Value::boolean(desc.pipeline.depthWriteEnable));
    pipeline.set("depthCompare", json::Value::number(static_cast<double>(desc.pipeline.depthCompare)));
    pipeline.set("blendEnable", json::Value::boolean(desc.pipeline.blendEnable));
    root.set("pipeline", pipeline);
    return root;
}

bool materialFromJson(const json::Value& root, MaterialDesc& outDesc) {
    if (root.type() != json::Value::Type::Object) {
        return false;
    }
    MaterialDesc desc;
    const auto stringField = [&root](const char* key, std::string& target) {
        if (const json::Value* value = root.find(key); value != nullptr) {
            target = value->asString();
        }
    };
    const auto numberIn = [](const json::Value& object, const char* key, float& target) {
        if (const json::Value* value = object.find(key); value != nullptr) {
            target = static_cast<float>(value->asNumber(target));
        }
    };
    const auto enumIn = [](const json::Value& object, const char* key, auto& target) {
        if (const json::Value* value = object.find(key); value != nullptr) {
            target = static_cast<std::decay_t<decltype(target)>>(static_cast<int>(value->asNumber()));
        }
    };
    stringField("name", desc.name);
    stringField("vertexShader", desc.vertexShader);
    stringField("fragmentShader", desc.fragmentShader);
    if (const json::Value* color = root.find("baseColor"); color != nullptr) {
        vecFromJson(*color, &desc.baseColor.x, 4);
    }
    if (const json::Value* lighting = root.find("lighting"); lighting != nullptr) {
        if (const json::Value* spec = lighting->find("specularColor"); spec != nullptr) {
            vecFromJson(*spec, &desc.lighting.specularColor.x, 3);
        }
        numberIn(*lighting, "specularStrength", desc.lighting.specularStrength);
        numberIn(*lighting, "shininess", desc.lighting.shininess);
        numberIn(*lighting, "rimStrength", desc.lighting.rimStrength);
        numberIn(*lighting, "rimPower", desc.lighting.rimPower);
        numberIn(*lighting, "ambientStrength", desc.lighting.ambientStrength);
    }
    if (const json::Value* pbr = root.find("pbr"); pbr != nullptr) {
        numberIn(*pbr, "metallic", desc.pbr.metallic);
        numberIn(*pbr, "roughness", desc.pbr.roughness);
        numberIn(*pbr, "occlusionStrength", desc.pbr.occlusionStrength);
        const auto pbrString = [&pbr](const char* key, std::string& target) {
            if (const json::Value* value = pbr->find(key); value != nullptr) {
                target = value->asString();
            }
        };
        pbrString("albedoTexture", desc.pbr.albedoTexture);
        pbrString("metallicRoughnessTexture", desc.pbr.metallicRoughnessTexture);
        pbrString("normalTexture", desc.pbr.normalTexture);
        pbrString("occlusionTexture", desc.pbr.occlusionTexture);
    }
    if (const json::Value* pipeline = root.find("pipeline"); pipeline != nullptr) {
        enumIn(*pipeline, "topology", desc.pipeline.topology);
        enumIn(*pipeline, "polygonMode", desc.pipeline.polygonMode);
        enumIn(*pipeline, "cullMode", desc.pipeline.cullMode);
        if (const json::Value* value = pipeline->find("depthTestEnable"); value != nullptr) {
            desc.pipeline.depthTestEnable = value->asBool(desc.pipeline.depthTestEnable);
        }
        if (const json::Value* value = pipeline->find("depthWriteEnable"); value != nullptr) {
            desc.pipeline.depthWriteEnable = value->asBool(desc.pipeline.depthWriteEnable);
        }
        enumIn(*pipeline, "depthCompare", desc.pipeline.depthCompare);
        if (const json::Value* value = pipeline->find("blendEnable"); value != nullptr) {
            desc.pipeline.blendEnable = value->asBool(desc.pipeline.blendEnable);
        }
    }
    if (desc.name.empty()) {
        return false;
    }
    outDesc = std::move(desc);
    return true;
}

// ---------------------------------------------------------------------------
// Bundle reading core (header + manifest shared by inspect/extract)
// ---------------------------------------------------------------------------

struct ParsedBundle {
    std::vector<std::byte> fileBytes;
    BundleInfo info;
};

bool parseBundleFile(const std::filesystem::path& bundlePath, ParsedBundle& out) {
    std::optional<std::vector<std::byte>> bytes = readFile(bundlePath);
    if (!bytes.has_value() || bytes->size() < kHeaderSize) {
        return false;
    }
    const std::byte* data = bytes->data();
    if (std::memcmp(data, kBundleMagic, 4) != 0) {
        return false;
    }
    const uint32_t version = getU32(data + 4);
    if (version != kBundleVersion) {
        return false;
    }
    const uint64_t manifestOffset = getU64(data + 12);
    const uint64_t manifestSize = getU64(data + 20);
    if (manifestOffset > bytes->size() || manifestSize > bytes->size() - static_cast<size_t>(manifestOffset)) {
        return false;
    }
    const std::string manifestText(reinterpret_cast<const char*>(data + manifestOffset),
                                   static_cast<size_t>(manifestSize));
    json::Value root;
    if (!json::parse(manifestText, root) || root.type() != json::Value::Type::Object) {
        return false;
    }
    const json::Value* entries = root.find("entries");
    if (entries == nullptr || entries->type() != json::Value::Type::Array) {
        return false;
    }
    out.fileBytes = std::move(*bytes);
    out.info = BundleInfo{};
    out.info.version = version;
    out.info.bundleFileSize = out.fileBytes.size();
    for (const json::Value& item : entries->items()) {
        if (item.type() != json::Value::Type::Object) {
            return false;
        }
        BundleEntryInfo entry;
        const auto numberOf = [&item](const char* key) {
            const json::Value* value = item.find(key);
            return value != nullptr ? value->asNumber(0.0) : 0.0;
        };
        entry.kind = static_cast<BundleEntryKind>(static_cast<int>(numberOf("kind")));
        if (const json::Value* name = item.find("name"); name != nullptr) {
            entry.name = name->asString();
        }
        entry.rawSize = static_cast<uint64_t>(numberOf("rawSize"));
        entry.storedSize = static_cast<uint64_t>(numberOf("storedSize"));
        entry.dataOffset = static_cast<uint64_t>(numberOf("dataOffset"));
        entry.crc32 = static_cast<uint32_t>(numberOf("crc"));
        if (entry.dataOffset > out.fileBytes.size() ||
            entry.storedSize > out.fileBytes.size() - static_cast<size_t>(entry.dataOffset)) {
            return false; // payload reaches past the file
        }
        out.info.entries.push_back(std::move(entry));
    }
    return true;
}

bool extractFromParsed(const ParsedBundle& bundle, const BundleEntryInfo& entry, std::vector<std::byte>& outBytes) {
    const std::byte* stored = bundle.fileBytes.data() + entry.dataOffset;
    if (!lzDecompress(stored, static_cast<size_t>(entry.storedSize), static_cast<size_t>(entry.rawSize), outBytes)) {
        return false;
    }
    return bundleCrc32(outBytes.data(), outBytes.size()) == entry.crc32;
}

const BundleEntryInfo* findEntry(const BundleInfo& info, BundleEntryKind kind, std::string_view name) {
    for (const BundleEntryInfo& entry : info.entries) {
        if (entry.kind == kind && entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace

const char* bundleEntryKindName(BundleEntryKind kind) {
    switch (kind) {
    case BundleEntryKind::Scene: return "scene";
    case BundleEntryKind::Mesh: return "mesh";
    case BundleEntryKind::Texture: return "texture";
    case BundleEntryKind::Shader: return "shader";
    case BundleEntryKind::Material: return "material";
    case BundleEntryKind::Script: return "script";
    }
    return "unknown";
}

uint32_t bundleCrc32(const void* data, size_t size) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> generated{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t crc = i;
            for (int bit = 0; bit < 8; ++bit) {
                crc = (crc & 1u) != 0 ? 0xEDB88320u ^ (crc >> 1) : crc >> 1;
            }
            generated[i] = crc;
        }
        return generated;
    }();
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ bytes[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

ExportReport ExportProject(const Scene& scene, const AssetManager& assets,
                           const std::filesystem::path& bundlePath) {
    ExportReport report;
    report.bundlePath = bundlePath;

    // --- Collect references deterministically ---------------------
    std::set<std::string> meshNames;
    std::set<std::string> materialNames;
    std::set<std::string> scriptNames;
    const auto& registry = scene.registry();
    for (const entt::entity entity : registry.view<MeshComponent>()) {
        const MeshComponent& mesh = registry.get<MeshComponent>(entity);
        meshNames.insert(mesh.assetId);
        materialNames.insert(mesh.materialName);
    }
    for (const entt::entity entity : registry.view<NativeScriptComponent>()) {
        scriptNames.insert(registry.get<NativeScriptComponent>(entity).scriptName);
    }

    struct Pending {
        BundleEntryKind kind;
        std::string name;
        std::vector<std::byte> raw;
    };
    std::vector<Pending> pending;
    const auto fail = [&report](std::string message) {
        report.ok = false;
        report.error = std::move(message);
        return report;
    };

    // Scene entry (always first; entry name "scene").
    pending.push_back(Pending{BundleEntryKind::Scene, "scene", scene.saveToGbqBytes()});

    for (const std::string& name : meshNames) {
        const MeshAsset* mesh = assets.mesh(name);
        if (mesh == nullptr) {
            report.missing.push_back(name);
            continue;
        }
        std::vector<std::byte> raw;
        putU32(raw, static_cast<uint32_t>(mesh->vertices.size()));
        putU32(raw, static_cast<uint32_t>(mesh->indices.size()));
        const size_t vertexBytes = mesh->vertices.size() * sizeof(Vertex);
        const size_t indexBytes = mesh->indices.size() * sizeof(uint32_t);
        raw.resize(raw.size() + vertexBytes + indexBytes);
        std::memcpy(raw.data() + 8, mesh->vertices.data(), vertexBytes);
        std::memcpy(raw.data() + 8 + vertexBytes, mesh->indices.data(), indexBytes);
        pending.push_back(Pending{BundleEntryKind::Mesh, name, std::move(raw)});
    }

    std::set<std::string> textureNames;
    std::set<std::string> shaderNames;
    for (const std::string& name : materialNames) {
        const MaterialAsset* material = assets.material(name);
        if (material == nullptr) {
            report.missing.push_back(name);
            continue;
        }
        const MaterialDesc& desc = material->desc;
        shaderNames.insert(desc.vertexShader);
        shaderNames.insert(desc.fragmentShader);
        for (const std::string* textureName :
             {&desc.pbr.albedoTexture, &desc.pbr.metallicRoughnessTexture, &desc.pbr.normalTexture,
              &desc.pbr.occlusionTexture}) {
            if (!textureName->empty()) {
                textureNames.insert(*textureName);
            }
        }
        const std::string json = json::stringify(materialToJson(desc));
        pending.push_back(Pending{BundleEntryKind::Material, name,
                                  std::vector<std::byte>(reinterpret_cast<const std::byte*>(json.data()),
                                                         reinterpret_cast<const std::byte*>(json.data() + json.size()))});
    }
    for (const std::string& name : textureNames) {
        const TextureAsset* texture = assets.texture(name);
        if (texture == nullptr) {
            report.missing.push_back(name);
            continue;
        }
        std::vector<std::byte> raw;
        putU32(raw, texture->width);
        putU32(raw, texture->height);
        raw.resize(8 + texture->pixels.size());
        std::memcpy(raw.data() + 8, texture->pixels.data(), texture->pixels.size());
        pending.push_back(Pending{BundleEntryKind::Texture, name, std::move(raw)});
    }
    for (const std::string& name : shaderNames) {
        const ShaderAsset* shader = assets.shader(name);
        if (shader == nullptr) {
            report.missing.push_back(name);
            continue;
        }
        const size_t byteCount = shader->code.size() * sizeof(uint32_t);
        std::vector<std::byte> raw(byteCount);
        std::memcpy(raw.data(), shader->code.data(), byteCount);
        pending.push_back(Pending{BundleEntryKind::Shader, name, std::move(raw)});
    }
    for (const std::string& name : scriptNames) {
        // Native scripts are compiled code: the bundle records the
        // class names the runtime binary must provide (empty payload).
        pending.push_back(Pending{BundleEntryKind::Script, name, {}});
    }

    if (!report.missing.empty()) {
        return fail("unregistered referenced assets (see report.missing)");
    }

    // --- Assemble -------------------------------------------------
    std::vector<std::byte> file(kHeaderSize, std::byte{0});
    std::memcpy(file.data(), kBundleMagic, 4);
    patchU32(file, 4, kBundleVersion);

    json::Value manifest = json::Value::object();
    manifest.set("format", json::Value::string("gbqb"));
    manifest.set("version", json::Value::number(kBundleVersion));
    json::Value entriesJson = json::Value::array();
    uint64_t offset = kHeaderSize;
    for (const Pending& item : pending) {
        const std::vector<std::byte> stored = lzCompress(item.raw.data(), item.raw.size());
        offset = (offset + 7u) & ~uint64_t{7u};
        while (file.size() < offset) {
            file.push_back(std::byte{0});
        }
        const uint64_t dataOffset = file.size();
        file.insert(file.end(), stored.begin(), stored.end());
        offset = file.size();

        json::Value entryJson = json::Value::object();
        entryJson.set("kind", json::Value::number(static_cast<double>(item.kind)));
        entryJson.set("kindName", json::Value::string(bundleEntryKindName(item.kind)));
        entryJson.set("name", json::Value::string(item.name));
        entryJson.set("rawSize", json::Value::number(static_cast<double>(item.raw.size())));
        entryJson.set("storedSize", json::Value::number(static_cast<double>(stored.size())));
        entryJson.set("dataOffset", json::Value::number(static_cast<double>(dataOffset)));
        entryJson.set("crc", json::Value::number(static_cast<double>(bundleCrc32(item.raw.data(), item.raw.size()))));
        entriesJson.push(entryJson);
        report.packed.push_back(std::string(bundleEntryKindName(item.kind)) + ":" + item.name);
    }
    manifest.set("entries", entriesJson);
    const std::string manifestText = json::stringify(manifest);

    offset = (offset + 7u) & ~uint64_t{7u};
    while (file.size() < offset) {
        file.push_back(std::byte{0});
    }
    patchU64(file, 12, offset); // manifestOffset
    patchU64(file, 20, manifestText.size());
    patchU32(file, 28, static_cast<uint32_t>(pending.size()));
    const auto* manifestBegin = reinterpret_cast<const std::byte*>(manifestText.data());
    file.insert(file.end(), manifestBegin, manifestBegin + manifestText.size());

    if (!writeFile(bundlePath, file.data(), file.size())) {
        return fail("cannot write bundle file");
    }
    report.bundleBytes = file.size();

    // Sidecar manifest + standalone config (deterministic text).
    const std::filesystem::path dir = bundlePath.parent_path();
    const std::string stem = bundlePath.stem().string();
    const std::string sidecar = json::stringify(manifest);
    writeFile(dir / (stem + ".manifest.json"), reinterpret_cast<const std::byte*>(sidecar.data()), sidecar.size());
    const std::string configText =
        "; Standalone runtime configuration (generated by Export Game, Phase 33)\n"
        "; Engine settings use the standard engine.ini sections; [Game] tells\n"
        "; the game-player which bundle to boot.\n"
        "[Game]\n"
        "bundle = " +
        bundlePath.filename().string() +
        "\n"
        "\n"
        "[Window]\n"
        "width = 1280\n"
        "height = 720\n"
        "title = Exported Game\n";
    writeFile(dir / "game.ini", reinterpret_cast<const std::byte*>(configText.data()), configText.size());

    report.ok = true;
    ENGINE_LOG_INFO("Export: wrote '" << bundlePath.string() << "' (" << file.size() << " bytes, "
                                      << pending.size() << " entries)");
    return report;
}

bool inspectBundle(const std::filesystem::path& bundlePath, BundleInfo& outInfo) {
    ParsedBundle parsed;
    if (!parseBundleFile(bundlePath, parsed)) {
        return false;
    }
    outInfo = std::move(parsed.info);
    return true;
}

bool extractBundleEntry(const std::filesystem::path& bundlePath, std::string_view name,
                        std::vector<std::byte>& outBytes) {
    ParsedBundle parsed;
    if (!parseBundleFile(bundlePath, parsed)) {
        return false;
    }
    const BundleEntryInfo* entry = parsed.info.find(name);
    if (entry == nullptr) {
        return false;
    }
    return extractFromParsed(parsed, *entry, outBytes);
}

RuntimeLoadResult loadBundleIntoRuntime(const std::filesystem::path& bundlePath, AssetManager& assets, Scene& scene) {
    RuntimeLoadResult result;
    ParsedBundle parsed;
    if (!parseBundleFile(bundlePath, parsed)) {
        result.error = "not a valid GBQB bundle";
        return result;
    }

    std::vector<std::byte> raw;
    const auto extractKind = [&parsed, &raw](const BundleEntryInfo& entry) {
        return extractFromParsed(parsed, entry, raw);
    };

    for (const BundleEntryInfo& entry : parsed.info.entries) {
        switch (entry.kind) {
        case BundleEntryKind::Texture: {
            if (!extractKind(entry) || raw.size() < 8) {
                result.error = "corrupt texture entry: " + entry.name;
                return result;
            }
            const uint32_t width = getU32(raw.data());
            const uint32_t height = getU32(raw.data() + 4);
            const size_t pixelBytes = raw.size() - 8;
            if (pixelBytes != static_cast<size_t>(width) * height * 4) {
                result.error = "texture entry size mismatch: " + entry.name;
                return result;
            }
            const auto* pixels = reinterpret_cast<const uint8_t*>(raw.data() + 8);
            if (assets.registerTexture(entry.name, width, height, std::span<const uint8_t>{pixels, pixelBytes}) ==
                kInvalidAssetId) {
                result.error = "texture registration failed: " + entry.name;
                return result;
            }
            break;
        }
        case BundleEntryKind::Mesh: {
            if (!extractKind(entry) || raw.size() < 8) {
                result.error = "corrupt mesh entry: " + entry.name;
                return result;
            }
            const uint32_t vertexCount = getU32(raw.data());
            const uint32_t indexCount = getU32(raw.data() + 4);
            const size_t expected = 8 + vertexCount * sizeof(Vertex) + indexCount * sizeof(uint32_t);
            if (raw.size() != expected) {
                result.error = "mesh entry size mismatch: " + entry.name;
                return result;
            }
            const Vertex* vertices = reinterpret_cast<const Vertex*>(raw.data() + 8);
            const uint32_t* indices = reinterpret_cast<const uint32_t*>(raw.data() + 8 + vertexCount * sizeof(Vertex));
            if (assets.registerMesh(entry.name, std::span<const Vertex>{vertices, vertexCount},
                                    std::span<const uint32_t>{indices, indexCount}) == kInvalidAssetId) {
                result.error = "mesh registration failed: " + entry.name;
                return result;
            }
            break;
        }
        case BundleEntryKind::Shader: {
            if (!extractKind(entry) || raw.size() % sizeof(uint32_t) != 0) {
                result.error = "corrupt shader entry: " + entry.name;
                return result;
            }
            const uint32_t* words = reinterpret_cast<const uint32_t*>(raw.data());
            if (assets.registerShader(entry.name,
                                      std::span<const uint32_t>{words, raw.size() / sizeof(uint32_t)}) ==
                kInvalidAssetId) {
                result.error = "shader registration failed: " + entry.name;
                return result;
            }
            break;
        }
        case BundleEntryKind::Material: {
            if (!extractKind(entry)) {
                result.error = "corrupt material entry: " + entry.name;
                return result;
            }
            const std::string text(reinterpret_cast<const char*>(raw.data()), raw.size());
            json::Value root;
            MaterialDesc desc;
            if (!json::parse(text, root) || !materialFromJson(root, desc)) {
                result.error = "malformed material entry: " + entry.name;
                return result;
            }
            assets.registerMaterial(std::move(desc));
            break;
        }
        case BundleEntryKind::Script:
            result.scripts.push_back(entry.name);
            break;
        case BundleEntryKind::Scene:
            break; // handled after all assets exist
        }
    }

    const BundleEntryInfo* sceneEntry = findEntry(parsed.info, BundleEntryKind::Scene, "scene");
    if (sceneEntry == nullptr) {
        result.error = "bundle has no scene entry";
        return result;
    }
    if (!extractKind(*sceneEntry) || !scene.loadFromGbqBytes(raw)) {
        result.error = "scene entry is corrupt or not a GBQ scene";
        return result;
    }
    result.entityCount = scene.entityCount();
    result.ok = true;
    return result;
}

} // namespace engine
