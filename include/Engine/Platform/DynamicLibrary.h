#pragma once
#include "Core/Export.h"

#include <filesystem>
#include <string>

namespace engine {

// ---------------------------------------------------------------------------
// DynamicLibrary — RAII wrapper over the OS dynamic loader
// (LoadLibrary/dlopen). Owns the OS handle; symbols are resolved by
// (unmangled) name. Movable, not copyable: exactly one owner closes
// the library, which is what makes the game-module scratch-copy
// reload dance safe to reason about.
// ---------------------------------------------------------------------------
class ENGINE_PLATFORM_API DynamicLibrary {
public:
    DynamicLibrary() = default;
    ~DynamicLibrary();

    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;
    DynamicLibrary(DynamicLibrary&& other) noexcept;
    DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;

    // Loads the library at `path`. On failure returns false and
    // fills outError with the OS loader's message; the object stays
    // unloaded and reusable.
    bool load(const std::filesystem::path& path, std::string& outError);
    void unload();

    [[nodiscard]] bool isLoaded() const { return m_handle != nullptr; }
    [[nodiscard]] void* nativeHandle() const { return m_handle; }

    // Address of an exported symbol, or nullptr when absent (or when
    // nothing is loaded). Names are looked up verbatim — export with
    // extern "C" for stable resolution.
    [[nodiscard]] void* symbol(const char* name) const;

    template <typename Function>
    [[nodiscard]] Function symbolAs(const char* name) const {
        return reinterpret_cast<Function>(symbol(name));
    }

private:
    void* m_handle = nullptr; // HMODULE on Windows, dlopen handle elsewhere
};

} // namespace engine
