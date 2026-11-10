// Mock game module for the Phase 23 tests. Built as a standalone
// shared library (no engine linkage — the ABI header only) and
// loaded by ModuleManager through a scratch copy, exactly like a
// real game module. It appends one line per lifecycle call to the
// state file whose path arrives via EngineContext::userData, so the
// host-side test can observe behavior across the library boundary
// without sharing any C++ state with the plugin.
#include "Core/GameModule.h"

#include <cstdio>
#include <string>

namespace {

class MockGameModule final : public engine::IGameModule {
public:
    void onLoad(engine::EngineContext& context) override {
        capturePath(context);
        writeLine("load");
    }

    void onUnload() override { writeLine("unload"); }

    void onUpdate(engine::EngineContext& context, float deltaSeconds) override {
        capturePath(context);
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "update %llu %.6f",
                      static_cast<unsigned long long>(context.frameIndex), static_cast<double>(deltaSeconds));
        writeLine(buffer);
    }

    void onFixedUpdate(engine::EngineContext&, float) override { writeLine("fixed"); }

    [[nodiscard]] const char* moduleName() const override { return "MockGameModule"; }

private:
    void capturePath(const engine::EngineContext& context) {
        if (context.userData != nullptr) {
            m_statePath = static_cast<const char*>(context.userData);
        }
    }

    void writeLine(const std::string& line) const {
        if (m_statePath.empty()) {
            return;
        }
        if (FILE* file = std::fopen(m_statePath.c_str(), "a")) {
            std::fprintf(file, "%s\n", line.c_str());
            std::fclose(file);
        }
    }

    std::string m_statePath;
};

} // namespace

ENGINE_IMPLEMENT_GAME_MODULE(MockGameModule)
