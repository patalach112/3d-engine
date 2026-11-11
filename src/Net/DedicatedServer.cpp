#include "Net/DedicatedServer.h"

#include "Core/Json.h"
#include "Core/Logger.h"
#include "Core/Time.h"
#include "Net/NetServer.h"
#include "Scripting/DSLComponents.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

namespace engine {
namespace {

bool readTextFile(const std::filesystem::path& path, std::string& out) {
    std::ifstream file(path);
    if (!file)
        return false;
    out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

// Attaches the editor-deployed server script set: every manifest
// entry becomes a ScriptComponent on a fresh entity, executed by
// the server's ScriptSystem from the next tick on.
void attachServerScripts(NetServer& server, const std::filesystem::path& manifestPath) {
    std::string text;
    if (!readTextFile(manifestPath, text))
        return; // no deployed script set: normal for a bare server.cfg boot
    json::Value root;
    std::string error;
    if (!json::parse(text, root, &error)) {
        ENGINE_LOG_WARNING("vibe_server: ignoring malformed " << manifestPath.string() << ": " << error);
        return;
    }
    const json::Value* scripts = root.find("scripts");
    if (scripts == nullptr)
        return;
    const std::filesystem::path baseDir = manifestPath.parent_path();
    for (const json::Value& entry : scripts->items()) {
        const json::Value* nameValue = entry.find("name");
        const json::Value* fileValue = entry.find("file");
        if (fileValue == nullptr)
            continue;
        const std::string name = nameValue != nullptr ? nameValue->asString() : fileValue->asString();
        std::string source;
        if (!readTextFile(baseDir / fileValue->asString(), source)) {
            ENGINE_LOG_WARNING("vibe_server: server script file missing: " << fileValue->asString());
            continue;
        }
        Entity entity = server.scene().createEntity(name);
        ScriptComponent script;
        script.source = std::move(source);
        script.enabled = true;
        entity.addComponent<ScriptComponent>(std::move(script));
        ENGINE_LOG_INFO("vibe_server: attached server script '" << name << "'");
    }
}

} // namespace

int runDedicatedServer(const DedicatedServerOptions& options, const std::atomic<bool>* stopFlag) {
    ServerSettings settings;
    {
        std::string error;
        if (std::filesystem::exists(options.configPath)) {
            ServerSettings loaded;
            if (!loadServerConfigFile(options.configPath, loaded, &error)) {
                std::cerr << "vibe_server: cannot read config " << options.configPath.string() << ": " << error
                          << '\n';
                return 2;
            }
            settings = loaded;
        } else {
            std::cout << "vibe_server: no config at " << options.configPath.string()
                      << ", using defaults (tick_rate 128, port 27015)\n";
        }
    }
    if (options.portOverride >= 0 && options.portOverride <= 65535)
        settings.port = static_cast<std::uint16_t>(options.portOverride);
    settings.clampToValid();

    NetServer server;
    if (!server.start(settings)) {
        std::cerr << "vibe_server: failed to start (port " << settings.port << " unavailable?)\n";
        return 2;
    }

    if (!options.scenePath.empty()) {
        if (!server.scene().loadFromGbqFile(options.scenePath.string())) {
            std::cerr << "vibe_server: failed to load scene " << options.scenePath.string() << '\n';
            server.stop();
            return 2;
        }
        std::cout << "vibe_server: loaded scene " << options.scenePath.string() << '\n';
    }

    const std::filesystem::path manifestPath = options.scriptsManifest.empty()
                                                   ? options.configPath.parent_path() / "server_scripts.json"
                                                   : options.scriptsManifest;
    attachServerScripts(server, manifestPath);

    std::cout << "vibe_server: '" << settings.serverName << "' running on port " << server.port() << " at "
              << settings.tickRate << " Hz (fixed dt " << settings.fixedDt() << " s), max " << settings.maxPlayers
              << " players\n";

    auto lastStatus = Time::now();
    auto lastFrame = Time::now();
    while (stopFlag == nullptr || !stopFlag->load(std::memory_order_relaxed)) {
        if (options.maxTicks > 0 && static_cast<long>(server.tick()) >= options.maxTicks)
            break;
        const auto now = Time::now();
        const double frameSeconds = Time::secondsBetween(lastFrame, now);
        lastFrame = now;
        const std::uint32_t steps = server.advance(frameSeconds);
        if (steps == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!options.quiet && Time::secondsBetween(lastStatus, now) >= 5.0) {
            lastStatus = now;
            const NetStatus status = server.status();
            std::cout << "vibe_server: tick " << status.serverTick << ", players " << status.playerCount << "/"
                      << status.maxPlayers << '\n';
        }
    }

    std::cout << "vibe_server: shutting down at tick " << server.tick() << '\n';
    server.stop();
    return 0;
}

} // namespace engine
