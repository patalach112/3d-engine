// A game module whose onLoad always throws: the host must catch it
// and keep running without a module (Phase 23).
#include "Core/GameModule.h"

#include <stdexcept>

namespace {

class ThrowGameModule final : public engine::IGameModule {
public:
    void onLoad(engine::EngineContext&) override { throw std::runtime_error("mock init failure"); }
    void onUnload() override {}
    void onUpdate(engine::EngineContext&, float) override {}
    void onFixedUpdate(engine::EngineContext&, float) override {}
};

} // namespace

ENGINE_IMPLEMENT_GAME_MODULE(ThrowGameModule)
