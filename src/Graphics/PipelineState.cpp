#include "Graphics/PipelineState.h"

namespace engine {
namespace {

void mix(uint64_t& hash, uint64_t value) {
    // FNV-1a over the value's bytes, low byte first.
    for (int byte = 0; byte < 8; ++byte) {
        hash ^= (value >> (byte * 8)) & 0xFFu;
        hash *= 1099511628211ull;
    }
}

} // namespace

uint64_t hashPipelineState(const PipelineStateDesc& state) {
    uint64_t hash = 14695981039346656037ull; // FNV offset basis
    mix(hash, static_cast<uint64_t>(state.topology));
    mix(hash, static_cast<uint64_t>(state.polygonMode));
    mix(hash, static_cast<uint64_t>(state.cullMode));
    mix(hash, state.depthTestEnable ? 1u : 0u);
    mix(hash, state.depthWriteEnable ? 1u : 0u);
    mix(hash, static_cast<uint64_t>(state.depthCompare));
    mix(hash, state.blendEnable ? 1u : 0u);
    return hash;
}

uint32_t PipelineStateCache::intern(const PipelineStateDesc& state) {
    const uint64_t key = hashPipelineState(state);
    const auto existing = m_idsByHash.find(key);
    if (existing != m_idsByHash.end()) {
        return existing->second;
    }
    const uint32_t id = static_cast<uint32_t>(m_idsByHash.size()) + 1;
    m_idsByHash.emplace(key, id);
    ++m_creationCount;
    return id;
}

uint32_t PipelineStateCache::find(const PipelineStateDesc& state) const {
    const auto existing = m_idsByHash.find(hashPipelineState(state));
    return existing != m_idsByHash.end() ? existing->second : 0;
}

} // namespace engine
