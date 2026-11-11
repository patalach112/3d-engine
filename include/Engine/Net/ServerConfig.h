#pragma once
#include "Core/Export.h"
#include "Net/NetProtocol.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine {

// ---------------------------------------------------------------------------
// Server configuration: the Source-style server.cfg model.
//
// ServerSettings is the single source of truth shared by:
//   * the editor's Server workspace (authoring),
//   * generateServerConfig() (the classic KEY VALUE text file the
//     editor writes on a successful server compile),
//   * parseServerConfig() (what vibe_server boots from),
//   * NetServer (the runtime that consumes it).
//
// server.cfg grammar (Source flavour): one statement per line,
// `key value` or `key = value`, `//` line comments, values may be
// double-quoted. Known keys map to typed fields; every unknown key
// is preserved verbatim in `rules` and written back, so custom
// server rules survive the editor round-trip.
// ---------------------------------------------------------------------------

struct ServerSettings {
    std::string serverName = "Vibe Server";
    int tickRate = kDefaultTickRate;    // fixed steps per second (128 Hz default)
    int maxPlayers = kDefaultMaxPlayers;
    std::uint16_t port = kDefaultServerPort; // 0 = OS-assigned (tests/tools)
    MoveSettings move{};                     // movement rules both sides simulate
    std::vector<std::pair<std::string, std::string>> rules; // custom key/value rules

    [[nodiscard]] ENGINE_NET_API float fixedDt() const;
    // Clamps every field into its legal envelope (tick rate
    // 10..1000, players 1..64, finite positive movement values).
    ENGINE_NET_API void clampToValid();
    [[nodiscard]] ENGINE_NET_API const std::string* findRule(std::string_view key) const;
    ENGINE_NET_API void setRule(std::string key, std::string value);

    // Exported like every other member: the definition lives in
    // ServerConfig.cpp, so without ENGINE_NET_API the symbol is
    // invisible across the DLL boundary (MinGW/Windows linkers
    // then report an undefined reference from consumer objects).
    [[nodiscard]] ENGINE_NET_API bool operator==(const ServerSettings& other) const;
    [[nodiscard]] bool operator!=(const ServerSettings& other) const { return !(*this == other); }
};

struct ServerConfigParseResult {
    ServerSettings settings;
    std::vector<std::string> warnings; // unknown-but-kept keys are NOT warnings; malformed lines are
};

// Parses server.cfg text. Never fails hard: malformed lines are
// skipped with a warning, missing keys keep defaults.
[[nodiscard]] ENGINE_NET_API ServerConfigParseResult parseServerConfig(std::string_view text);

// Renders the classic server.cfg document (stable key order,
// custom rules last). parse(generate(s)) == s for clamped s.
[[nodiscard]] ENGINE_NET_API std::string generateServerConfig(const ServerSettings& settings);

ENGINE_NET_API bool loadServerConfigFile(const std::filesystem::path& path, ServerSettings& out, std::string* error);
ENGINE_NET_API bool saveServerConfigFile(const std::filesystem::path& path, const ServerSettings& settings,
                                         std::string* error);

// ---------------------------------------------------------------------------
// ServerWorkspace: everything the editor's Server tab authors —
// settings plus the server-side .dsla script set — persisted as
// JSON next to the generated server.cfg. This workspace is fully
// isolated from the client scene: server scripts never enter the
// client build, they deploy to vibe_server only.
// ---------------------------------------------------------------------------

struct ServerScriptEntry {
    std::string name;     // logical name; also the deployed .dsla file stem
    std::string source;   // .dsla program text (server-side logic)
    bool enabled = true;
};

struct ServerWorkspace {
    ServerSettings settings;
    std::vector<ServerScriptEntry> scripts;

    [[nodiscard]] ENGINE_NET_API std::string toJson() const;
    ENGINE_NET_API bool fromJson(std::string_view text, std::string* error);
    ENGINE_NET_API bool saveToFile(const std::filesystem::path& path, std::string* error) const;
    ENGINE_NET_API bool loadFromFile(const std::filesystem::path& path, std::string* error);

    struct CompileResult {
        bool ok = false;
        std::vector<std::string> errors;       // per-script parse failures
        std::vector<std::string> writtenFiles; // paths written on success
    };

    // The Server target's independent compilation pipeline:
    // validates every enabled server script against the DSL
    // grammar, and only when all of them compile writes the
    // deployable set into outputDir —
    //   server.cfg            (from settings, always regenerated)
    //   server_workspace.json (the authoring state)
    //   server_scripts/<name>.dsla + server_scripts.json manifest
    // On failure nothing is written and errors name the script.
    [[nodiscard]] ENGINE_NET_API CompileResult compile(const std::filesystem::path& outputDir) const;
};

} // namespace engine
