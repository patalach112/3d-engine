#pragma once
#include "Core/Export.h"

#include <atomic>
#include <cstdint>
#include <filesystem>

namespace engine {

// ---------------------------------------------------------------------------
// Dedicated server host: boots a NetServer from a Source-style
// server.cfg, optionally loads a GBQ scene (whose ScriptComponents
// then run as server-side .dsla logic and whose physics publishes
// trigger/contact events to clients), attaches the editor Server
// workspace's deployed script set (server_scripts.json next to
// the config), and runs the fixed-timestep loop until told to
// stop. No window, no Vulkan — this is the whole vibe_server
// runtime, shared by the vibe_server executable and by
// `game-player --server`.
//
// Exit codes: 0 clean shutdown, 2 boot failure (config unreadable
// when explicit, port bind failure, scene load failure).
// ---------------------------------------------------------------------------

struct DedicatedServerOptions {
    std::filesystem::path configPath = "server.cfg";
    std::filesystem::path scenePath;        // optional GBQ scene
    std::filesystem::path scriptsManifest;  // default: <config dir>/server_scripts.json
    long maxTicks = 0;                      // 0 = run until stopFlag / forever
    int portOverride = -1;                  // >= 0 replaces the config port
    bool quiet = false;                     // suppress periodic status lines
};

ENGINE_NET_API int runDedicatedServer(const DedicatedServerOptions& options,
                                      const std::atomic<bool>* stopFlag = nullptr);

} // namespace engine
