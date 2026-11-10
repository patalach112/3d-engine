// A module with the genuine ABI token but a handshake ABI version
// the host does not speak: ModuleManager must reject it with a
// version-mismatch error (Phase 24). Deliberately does NOT use
// ENGINE_IMPLEMENT_GAME_MODULE, which would stamp the current ABI.
#include "Core/GameModule.h"

namespace {

class LegacyModule final : public engine::IGameModule {
public:
    void onLoad(engine::EngineContext&) override {}
    void onUnload() override {}
    void onUpdate(engine::EngineContext&, float) override {}
    void onFixedUpdate(engine::EngineContext&, float) override {}
    [[nodiscard]] const char* moduleName() const override { return "LegacyModule"; }
};

} // namespace

extern "C" ENGINE_PLUGIN_API engine::IGameModule* CreateGameModule() { return new LegacyModule(); }
extern "C" ENGINE_PLUGIN_API void DestroyGameModule(engine::IGameModule* module) { delete module; }
extern "C" ENGINE_PLUGIN_API const engine::EngineModuleInfo* GetEngineModuleInfo() {
    static const engine::EngineModuleInfo info{engine::kEngineModuleMagic, engine::kEngineModuleAbiVersion + 998u,
                                               static_cast<uint32_t>(sizeof(engine::EngineModuleInfo)),
                                               "LegacyModule"};
    return &info;
}
