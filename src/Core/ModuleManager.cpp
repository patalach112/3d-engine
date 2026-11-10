#include "Core/ModuleManager.h"

#include "Core/Logger.h"

#include <algorithm>
#include <exception>
#include <atomic>
#include <system_error>
#include <utility>

namespace engine {

ModuleManager::~ModuleManager() { unload(); }

bool ModuleManager::isEngineBrandedModuleName(const std::filesystem::path& path) {
    const std::string extension = path.extension().string();
    if (extension != ".so" && extension != ".dll" && extension != ".dylib") {
        return false;
    }
    const std::string stem = path.stem().string();
    constexpr std::string_view prefix = "EngineRuntimeCore_";
    if (!stem.starts_with(prefix) || stem.size() == prefix.size()) {
        return false;
    }
    return std::all_of(stem.begin() + static_cast<std::ptrdiff_t>(prefix.size()), stem.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

bool ModuleManager::fail(std::string message) {
    m_lastError = std::move(message);
    ENGINE_LOG_WARNING("ModuleManager: " << m_lastError);
    return false;
}

void ModuleManager::retireCurrent() {
    if (m_module != nullptr) {
        try {
            m_module->onUnload();
        } catch (const std::exception& error) {
            ENGINE_LOG_WARNING("ModuleManager: module onUnload threw: " << error.what());
        } catch (...) {
            ENGINE_LOG_WARNING("ModuleManager: module onUnload threw an unknown exception");
        }
        if (m_destroy != nullptr) {
            m_destroy(m_module);
        }
        m_module = nullptr;
        m_destroy = nullptr;
    }
    m_library.unload();
    if (!m_scratchPath.empty()) {
        std::error_code ignored;
        std::filesystem::remove(m_scratchPath, ignored); // best effort; a new scratch name dodges locks
        m_scratchPath.clear();
    }
    m_moduleName.clear();
    m_moduleAbiVersion = 0;
}

bool ModuleManager::load(const std::filesystem::path& path, EngineContext& context) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        return fail("module file not found: '" + path.string() + "'");
    }
    const std::filesystem::file_time_type writeTime = std::filesystem::last_write_time(path, error);
    if (error) {
        return fail("cannot stat module file '" + path.string() + "': " + error.message());
    }

    // Engine-branded naming policy (Phase 24): game modules ship as
    // EngineRuntimeCore_<signature>; anything else is not ours.
    if (!isEngineBrandedModuleName(path)) {
        return fail("module rejected: '" + path.filename().string() +
                    "' does not follow the engine-branded naming convention EngineRuntimeCore_<signature>");
    }

    // Scratch copy: the OS loader only ever sees this file, so the
    // original stays writable by the toolchain. The process-wide
    // counter makes every generation's name unique — a still-loaded
    // previous scratch can never block the copy (Windows file
    // locking), and two managers can never collide on one path.
    // The counter is process-wide, not per-manager: two managers
    // must never pick the same scratch path, or one would truncate
    // the file the other has memory-mapped (and crash the loader).
    static std::atomic<uint64_t> s_scratchCounter{0};
    const uint64_t scratchIndex = s_scratchCounter.fetch_add(1, std::memory_order_relaxed) + 1;
    const std::filesystem::path scratch = path.parent_path() /
                                          (path.stem().string() + "_scratch" + std::to_string(scratchIndex) +
                                           path.extension().string());
    std::filesystem::copy_file(path, scratch, std::filesystem::copy_options::overwrite_existing, error);
    if (error) {
        return fail("cannot copy module to scratch file '" + scratch.string() + "': " + error.message());
    }

    const auto discardScratch = [&scratch] {
        std::error_code ignored;
        std::filesystem::remove(scratch, ignored);
    };

    DynamicLibrary candidate;
    std::string loadError;
    if (!candidate.load(scratch, loadError)) {
        discardScratch();
        return fail(loadError);
    }
    // Branded handshake (Phase 24): the binary must prove it was
    // built as an engine module — correct ABI token, exact ABI
    // version — before any factory inside it is allowed to run.
    const GetEngineModuleInfoFn getInfo = candidate.symbolAs<GetEngineModuleInfoFn>(kGetEngineModuleInfoSymbol);
    if (getInfo == nullptr) {
        discardScratch();
        return fail("module '" + path.string() + "' has no engine handshake (GetEngineModuleInfo missing)");
    }
    const EngineModuleInfo* info = nullptr;
    try {
        info = getInfo();
    } catch (...) {
        info = nullptr;
    }
    if (info == nullptr || info->structSize < sizeof(EngineModuleInfo)) {
        discardScratch();
        return fail("module '" + path.string() + "' returned an invalid handshake descriptor");
    }
    if (info->magic != kEngineModuleMagic) {
        discardScratch();
        return fail("module '" + path.string() + "' failed the engine ABI token check");
    }
    if (info->abiVersion != kEngineModuleAbiVersion) {
        discardScratch();
        return fail("module '" + path.string() + "' ABI version mismatch (module " +
                    std::to_string(info->abiVersion) + ", engine expects " +
                    std::to_string(kEngineModuleAbiVersion) + ")");
    }

    const CreateGameModuleFn create = candidate.symbolAs<CreateGameModuleFn>(kCreateGameModuleSymbol);
    const DestroyGameModuleFn destroy = candidate.symbolAs<DestroyGameModuleFn>(kDestroyGameModuleSymbol);
    if (create == nullptr || destroy == nullptr) {
        discardScratch();
        return fail("module '" + path.string() + "' does not export CreateGameModule/DestroyGameModule");
    }

    IGameModule* fresh = nullptr;
    try {
        fresh = create();
        if (fresh != nullptr) {
            fresh->onLoad(context);
        }
    } catch (const std::exception& exception) {
        if (fresh != nullptr) {
            destroy(fresh);
        }
        discardScratch();
        return fail("module initialization threw: " + std::string(exception.what()));
    } catch (...) {
        if (fresh != nullptr) {
            destroy(fresh);
        }
        discardScratch();
        return fail("module initialization threw an unknown exception");
    }
    if (fresh == nullptr) {
        discardScratch();
        return fail("CreateGameModule returned null for '" + path.string() + "'");
    }

    // The new generation is alive: only now retire the old one, so a
    // failed reload never leaves the game without logic.
    retireCurrent();
    m_library = std::move(candidate);
    m_module = fresh;
    m_destroy = destroy;
    m_sourcePath = path;
    m_scratchPath = scratch;
    m_sourceWriteTime = writeTime;
    ++m_generation;
    const char* name = nullptr;
    try {
        name = m_module->moduleName();
    } catch (...) {
        name = nullptr;
    }
    m_moduleName = name != nullptr ? name : "GameModule";
    m_moduleAbiVersion = info->abiVersion;
    m_lastError.clear();
    ENGINE_LOG_INFO("ModuleManager: loaded game module '" << m_moduleName << "' (generation " << m_generation
                                                          << ") from " << path.string());
    return true;
}

bool ModuleManager::reload(EngineContext& context) {
    if (m_sourcePath.empty()) {
        return fail("no module has been loaded yet");
    }
    return load(m_sourcePath, context);
}

bool ModuleManager::pollForChanges(EngineContext& context) {
    if (m_module == nullptr || m_sourcePath.empty()) {
        return false;
    }
    std::error_code error;
    const std::filesystem::file_time_type writeTime = std::filesystem::last_write_time(m_sourcePath, error);
    if (error || writeTime <= m_sourceWriteTime) {
        return false;
    }
    return reload(context);
}

void ModuleManager::unload() {
    retireCurrent();
    m_sourcePath.clear();
}

void ModuleManager::update(EngineContext& context, float deltaSeconds) {
    if (m_module == nullptr) {
        return;
    }
    try {
        m_module->onUpdate(context, deltaSeconds);
    } catch (const std::exception& error) {
        fail("module onUpdate threw (" + std::string(error.what()) + "); module retired");
        retireCurrent();
    } catch (...) {
        fail("module onUpdate threw an unknown exception; module retired");
        retireCurrent();
    }
}

void ModuleManager::fixedUpdate(EngineContext& context, float fixedDeltaSeconds) {
    if (m_module == nullptr) {
        return;
    }
    try {
        m_module->onFixedUpdate(context, fixedDeltaSeconds);
    } catch (const std::exception& error) {
        fail("module onFixedUpdate threw (" + std::string(error.what()) + "); module retired");
        retireCurrent();
    } catch (...) {
        fail("module onFixedUpdate threw an unknown exception; module retired");
        retireCurrent();
    }
}

} // namespace engine
