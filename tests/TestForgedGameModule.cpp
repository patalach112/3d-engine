// A module with perfectly valid factories but a handshake descriptor
// carrying a forged ABI token: ModuleManager must reject it before
// any factory runs (Phase 24). Deliberately does NOT use
// ENGINE_IMPLEMENT_GAME_MODULE, which would stamp the genuine token.
#include "Core/GameModule.h"

namespace {

class ForgedModule final : public engine::IGameModule {
public:
    void onLoad(engine::EngineContext&) override {}
    void onUnload() override {}
    void onUpdate(engine::EngineContext&, float) override {}
    void onFixedUpdate(engine::EngineContext&, float) override {}
    [[nodiscard]] const char* moduleName() const override { return "ForgedModule"; }
};

} // namespace

extern "C" ENGINE_PLUGIN_API engine::IGameModule* CreateGameModule() { return new ForgedModule(); }
extern "C" ENGINE_PLUGIN_API void DestroyGameModule(engine::IGameModule* module) { delete module; }
extern "C" ENGINE_PLUGIN_API const engine::EngineModuleInfo* GetEngineModuleInfo() {
    static const engine::EngineModuleInfo info{0xDEADBEEFu, engine::kEngineModuleAbiVersion,
                                               static_cast<uint32_t>(sizeof(engine::EngineModuleInfo)),
                                               "ForgedModule"};
    return &info;
}
