#pragma once

#include <entt/entt.hpp>

#include <cstddef>

namespace engine {

// ---------------------------------------------------------------------------
// Dense storage access helpers.
//
// EnTT packs components into dense, paged arrays: pages of
// component_traits<T>::page_size elements (1024 by default), where each
// page is a single contiguous allocation and elements inside a page are
// ordinary array entries. Iterating dense indices therefore walks memory
// almost perfectly sequentially — one page-table hop per 1024 elements.
//
// denseElementAt() resolves a dense index to a component pointer through
// the storage's page table (raw()). Page sizes are powers of two, so the
// division/modulo compile down to a shift and a mask.
// ---------------------------------------------------------------------------
template <typename Storage>
[[nodiscard]] auto* denseElementAt(Storage& storage, size_t index) {
    using Element = typename Storage::element_type;
    constexpr size_t kPageSize = entt::component_traits<Element>::page_size;
    static_assert(kPageSize > 0, "dense access requires paged (non-empty) component storage");
    return storage.raw()[index / kPageSize] + (index % kPageSize);
}

} // namespace engine
