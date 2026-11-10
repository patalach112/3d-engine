#pragma once
#include "Core/Export.h"

#include "Core/GameModule.h"
#include "Platform/DynamicLibrary.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace engine {

// ---------------------------------------------------------------------------
// ModuleManager — loads, hot-reloads and drives one native game
// module (Phase 23).
//
// File locking is solved by scratch copies: the compiler's output
// file is copied to a uniquely named scratch sibling (e.g.
// game_logic_scratch1.so) and only the scratch file is handed to the
// OS loader, so the toolchain can rebuild the original at any moment
// without fighting the running engine.
//
// Reload safety: the new generation is fully created and onLoad()ed
// before the old one is retired, so a broken rebuild (missing file,
// missing exports, throwing factory) leaves the previous module
// running. Exceptions from module callbacks are caught and logged;
// a throwing update retires the module instead of crashing the loop.
// The engine-owned scene is never touched here — see EngineContext.
//
// Engine-branded module policy (Phase 24): only files named
// EngineRuntimeCore_<signature> (.dll/.so/.dylib) whose
// GetEngineModuleInfo handshake carries the engine ABI token and a
// matching ABI version are accepted; anything else is rejected
// before a single factory runs.
// ---------------------------------------------------------------------------
class ENGINE_CORE_API ModuleManager {
public:
    ModuleManager() = default;
    ~ModuleManager();

    ModuleManager(const ModuleManager&) = delete;
    ModuleManager& operator=(const ModuleManager&) = delete;

    // Loads the module at `path` (replacing any loaded generation
    // only after the new one initialized successfully).
    bool load(const std::filesystem::path& path, EngineContext& context);
    // Reloads from the path of the last successful load.
    bool reload(EngineContext& context);
    // Reloads when the source file's mtime moved past the loaded
    // generation's. Returns true when a reload happened.
    bool pollForChanges(EngineContext& context);
    void unload();

    void update(EngineContext& context, float deltaSeconds);
    void fixedUpdate(EngineContext& context, float fixedDeltaSeconds);

    // Naming-convention check of the Phase 24 branding policy.
    [[nodiscard]] static bool isEngineBrandedModuleName(const std::filesystem::path& path);

    [[nodiscard]] bool isLoaded() const { return m_module != nullptr; }
    [[nodiscard]] std::string_view moduleName() const { return m_moduleName; }
    // ABI version reported by the loaded module's handshake (0 = none).
    [[nodiscard]] uint32_t moduleAbiVersion() const { return m_moduleAbiVersion; }
    [[nodiscard]] uint32_t generation() const { return m_generation; }
    [[nodiscard]] const std::string& lastError() const { return m_lastError; }
    [[nodiscard]] const std::filesystem::path& sourcePath() const { return m_sourcePath; }
    [[nodiscard]] const std::filesystem::path& scratchPath() const { return m_scratchPath; }

private:
    void retireCurrent();
    bool fail(std::string message);

    DynamicLibrary m_library;
    IGameModule* m_module = nullptr;
    DestroyGameModuleFn m_destroy = nullptr;
    std::filesystem::path m_sourcePath;
    std::filesystem::path m_scratchPath;
    std::filesystem::file_time_type m_sourceWriteTime{};
    std::string m_moduleName;
    uint32_t m_moduleAbiVersion = 0;
    std::string m_lastError;
    uint32_t m_generation = 0;
};

} // namespace engine
