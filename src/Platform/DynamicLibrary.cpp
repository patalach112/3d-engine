#include "Platform/DynamicLibrary.h"

#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

namespace engine {

DynamicLibrary::~DynamicLibrary() { unload(); }

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept : m_handle(other.m_handle) {
    other.m_handle = nullptr;
}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept {
    if (this != &other) {
        unload();
        m_handle = other.m_handle;
        other.m_handle = nullptr;
    }
    return *this;
}

bool DynamicLibrary::load(const std::filesystem::path& path, std::string& outError) {
    unload();
#if defined(_WIN32)
    m_handle = reinterpret_cast<void*>(LoadLibraryW(path.wstring().c_str()));
    if (m_handle == nullptr) {
        outError = "LoadLibrary failed for '" + path.string() + "' (error " +
                   std::to_string(GetLastError()) + ")";
        return false;
    }
#else
    dlerror(); // clear any stale error
    m_handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (m_handle == nullptr) {
        const char* message = dlerror();
        outError = "dlopen failed for '" + path.string() + "': " + (message != nullptr ? message : "unknown error");
        return false;
    }
#endif
    return true;
}

void DynamicLibrary::unload() {
    if (m_handle == nullptr) {
        return;
    }
#if defined(_WIN32)
    FreeLibrary(reinterpret_cast<HMODULE>(m_handle));
#else
    dlclose(m_handle);
#endif
    m_handle = nullptr;
}

void* DynamicLibrary::symbol(const char* name) const {
    if (m_handle == nullptr || name == nullptr) {
        return nullptr;
    }
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(m_handle), name));
#else
    return dlsym(m_handle, name);
#endif
}

} // namespace engine
