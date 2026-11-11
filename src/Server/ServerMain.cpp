// vibe_server: the headless dedicated server executable.
//
// Boots a Source-style server.cfg and runs the server-authoritative
// fixed-timestep simulation (128 Hz by default) with physics,
// server-side .dsla scripts and ENet transport. No window, no
// Vulkan, no rendering — safe for CI, containers and real hosts.
//
//   vibe_server [--config server.cfg] [--scene scene.gbq]
//               [--port N] [--ticks N] [--quiet] [--help]
//
// --ticks bounds the run for smoke tests; without it the server
// runs until SIGINT/SIGTERM.

#include "Core/Logger.h"
#include "Net/DedicatedServer.h"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace {

std::atomic<bool> g_stopRequested{false};

void handleSignal(int) {
    g_stopRequested.store(true, std::memory_order_relaxed);
}

void printUsage(const char* program) {
    std::cout << "Usage: " << program << " [--config <server.cfg>] [--scene <scene.gbq>] [--port <n>] "
                                         "[--ticks <n>] [--quiet] [--help]\n"
              << "  --config <path>  Source-style server.cfg (default: server.cfg; defaults when absent)\n"
              << "  --scene <path>   GBQ scene to simulate (physics, triggers, server scripts)\n"
              << "  --port <n>       Override the config port\n"
              << "  --ticks <n>      Stop after N fixed ticks (smoke tests); 0 = forever\n"
              << "  --quiet          Suppress periodic status lines\n"
              << "  --help           Show this message\n";
}

} // namespace

int main(int argc, char* argv[]) {
    engine::DedicatedServerOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        const auto takeValue = [&](const char* flag, std::string& target) {
            if (i + 1 < argc) {
                target = argv[++i];
                return true;
            }
            std::cerr << "Missing value for " << flag << '\n';
            return false;
        };
        std::string value;
        if (argument == "--help" || argument == "-h") {
            printUsage(argv[0]);
            return 0;
        } else if (argument == "--config") {
            if (!takeValue("--config", value))
                return 2;
            options.configPath = value;
        } else if (argument == "--scene") {
            if (!takeValue("--scene", value))
                return 2;
            options.scenePath = value;
        } else if (argument == "--manifest") {
            if (!takeValue("--manifest", value))
                return 2;
            options.scriptsManifest = value;
        } else if (argument == "--port") {
            if (!takeValue("--port", value))
                return 2;
            options.portOverride = std::atoi(value.c_str());
        } else if (argument == "--ticks") {
            if (!takeValue("--ticks", value))
                return 2;
            options.maxTicks = std::atol(value.c_str());
        } else if (argument == "--quiet") {
            options.quiet = true;
        } else {
            std::cerr << "Unknown argument: " << argument << '\n';
            printUsage(argv[0]);
            return 2;
        }
    }

    engine::Logger::instance().initialize(engine::LoggerConfig{});
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    return engine::runDedicatedServer(options, &g_stopRequested);
}
