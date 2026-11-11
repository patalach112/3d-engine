#include "Net/ServerConfig.h"

#include "Core/Json.h"
#include "Scripting/DSLParser.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <system_error>

namespace engine {
namespace {

std::string trim(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0)
        ++begin;
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0)
        --end;
    return std::string(text.substr(begin, end - begin));
}

std::string toLower(std::string_view text) {
    std::string out(text);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string formatFloat(float value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(value));
    return buffer;
}

bool parseInt(std::string_view text, int& out) {
    const std::string trimmed = trim(text);
    if (trimmed.empty())
        return false;
    char* end = nullptr;
    const long value = std::strtol(trimmed.c_str(), &end, 10);
    if (end == trimmed.c_str())
        return false;
    out = static_cast<int>(value);
    return true;
}

bool parseFloat(std::string_view text, float& out) {
    const std::string trimmed = trim(text);
    if (trimmed.empty())
        return false;
    char* end = nullptr;
    const float value = std::strtof(trimmed.c_str(), &end);
    if (end == trimmed.c_str() || !std::isfinite(value))
        return false;
    out = value;
    return true;
}

// Atomic text write: temp sibling + rename (the engine's robust
// save convention), so an interrupted compile never leaves a
// half-written server.cfg behind.
bool writeTextAtomic(const std::filesystem::path& path, std::string_view text, std::string* error) {
    std::error_code ec;
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), ec);
    const std::filesystem::path tempPath = path.string() + ".tmp";
    {
        std::ofstream file(tempPath, std::ios::trunc);
        if (!file) {
            if (error != nullptr)
                *error = "cannot open " + tempPath.string();
            return false;
        }
        file << text;
        file.flush();
        if (!file) {
            if (error != nullptr)
                *error = "write failed for " + tempPath.string();
            return false;
        }
    }
    std::filesystem::rename(tempPath, path, ec);
    if (ec) {
        // Windows refuses rename-over-existing; remove then retry.
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(tempPath, path, ec);
        if (ec) {
            if (error != nullptr)
                *error = "rename failed for " + path.string() + ": " + ec.message();
            return false;
        }
    }
    return true;
}

bool readTextFile(const std::filesystem::path& path, std::string& out, std::string* error) {
    std::ifstream file(path);
    if (!file) {
        if (error != nullptr)
            *error = "cannot open " + path.string();
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

std::string sanitizeScriptStem(const std::string& name) {
    std::string stem;
    stem.reserve(name.size());
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                        c == '-';
        stem.push_back(ok ? c : '_');
    }
    return stem.empty() ? std::string("script") : stem;
}

} // namespace

// --- ServerSettings -----------------------------------------------------------

float ServerSettings::fixedDt() const {
    return 1.0f / static_cast<float>(std::max(1, tickRate));
}

void ServerSettings::clampToValid() {
    tickRate = std::clamp(tickRate, 10, 1000);
    maxPlayers = std::clamp(maxPlayers, 1, 64);
    if (serverName.empty())
        serverName = "Vibe Server";
    if (serverName.size() > 64)
        serverName.resize(64);
    auto clampMove = [](float value, float fallback) {
        return std::isfinite(value) && value > 0.0f ? value : fallback;
    };
    const MoveSettings defaults;
    move.maxSpeed = clampMove(move.maxSpeed, defaults.maxSpeed);
    move.friction = std::isfinite(move.friction) && move.friction >= 0.0f ? move.friction : defaults.friction;
    move.stopSpeed = clampMove(move.stopSpeed, defaults.stopSpeed);
    move.accelerate = clampMove(move.accelerate, defaults.accelerate);
    move.airAccelerate = clampMove(move.airAccelerate, defaults.airAccelerate);
    move.gravity = clampMove(move.gravity, defaults.gravity);
    move.jumpSpeed = clampMove(move.jumpSpeed, defaults.jumpSpeed);
    if (!std::isfinite(move.groundY))
        move.groundY = defaults.groundY;
}

const std::string* ServerSettings::findRule(std::string_view key) const {
    for (const auto& [ruleKey, ruleValue] : rules) {
        if (toLower(ruleKey) == toLower(key))
            return &ruleValue;
    }
    return nullptr;
}

void ServerSettings::setRule(std::string key, std::string value) {
    for (auto& [ruleKey, ruleValue] : rules) {
        if (toLower(ruleKey) == toLower(key)) {
            ruleValue = std::move(value);
            return;
        }
    }
    rules.emplace_back(std::move(key), std::move(value));
}

bool ServerSettings::operator==(const ServerSettings& other) const {
    return serverName == other.serverName && tickRate == other.tickRate && maxPlayers == other.maxPlayers &&
           port == other.port && rules == other.rules && move.maxSpeed == other.move.maxSpeed &&
           move.friction == other.move.friction && move.stopSpeed == other.move.stopSpeed &&
           move.accelerate == other.move.accelerate && move.airAccelerate == other.move.airAccelerate &&
           move.gravity == other.move.gravity && move.jumpSpeed == other.move.jumpSpeed &&
           move.groundY == other.move.groundY;
}

// --- server.cfg text ------------------------------------------------------------

ServerConfigParseResult parseServerConfig(std::string_view text) {
    ServerConfigParseResult result;
    ServerSettings& settings = result.settings;
    std::size_t lineStart = 0;
    int lineNumber = 0;
    while (lineStart <= text.size()) {
        const std::size_t lineEnd = text.find('\n', lineStart);
        const std::string_view line =
            text.substr(lineStart, lineEnd == std::string_view::npos ? std::string_view::npos : lineEnd - lineStart);
        ++lineNumber;
        lineStart = lineEnd == std::string_view::npos ? text.size() + 1 : lineEnd + 1;

        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed.rfind("//", 0) == 0 || trimmed.rfind(";", 0) == 0)
            continue;

        // Key: up to whitespace or '='.
        std::size_t keyEnd = 0;
        while (keyEnd < trimmed.size() &&
               std::isspace(static_cast<unsigned char>(trimmed[keyEnd])) == 0 && trimmed[keyEnd] != '=')
            ++keyEnd;
        const std::string key = toLower(trimmed.substr(0, keyEnd));
        std::string valueText = trim(std::string_view(trimmed).substr(keyEnd));
        if (!valueText.empty() && valueText.front() == '=')
            valueText = trim(std::string_view(valueText).substr(1));
        std::string value;
        if (!valueText.empty() && valueText.front() == '"') {
            const std::size_t close = valueText.find('"', 1);
            value = close == std::string::npos ? valueText.substr(1) : valueText.substr(1, close - 1);
        } else {
            // Unquoted: a // comment ends the value.
            const std::size_t comment = valueText.find("//");
            value = trim(comment == std::string::npos ? valueText : valueText.substr(0, comment));
        }
        if (key.empty()) {
            result.warnings.push_back("line " + std::to_string(lineNumber) + ": missing key");
            continue;
        }

        int intValue = 0;
        float floatValue = 0.0f;
        if (key == "hostname") {
            settings.serverName = value.empty() ? settings.serverName : value;
        } else if (key == "tick_rate" || key == "tickrate") {
            if (parseInt(value, intValue))
                settings.tickRate = intValue;
            else
                result.warnings.push_back("line " + std::to_string(lineNumber) + ": bad tick_rate");
        } else if (key == "max_players" || key == "maxplayers") {
            if (parseInt(value, intValue))
                settings.maxPlayers = intValue;
            else
                result.warnings.push_back("line " + std::to_string(lineNumber) + ": bad max_players");
        } else if (key == "port") {
            if (parseInt(value, intValue) && intValue >= 0 && intValue <= 65535)
                settings.port = static_cast<std::uint16_t>(intValue);
            else
                result.warnings.push_back("line " + std::to_string(lineNumber) + ": bad port");
        } else if (key == "move_max_speed" && parseFloat(value, floatValue)) {
            settings.move.maxSpeed = floatValue;
        } else if (key == "move_friction" && parseFloat(value, floatValue)) {
            settings.move.friction = floatValue;
        } else if (key == "move_stop_speed" && parseFloat(value, floatValue)) {
            settings.move.stopSpeed = floatValue;
        } else if (key == "move_accelerate" && parseFloat(value, floatValue)) {
            settings.move.accelerate = floatValue;
        } else if (key == "move_air_accelerate" && parseFloat(value, floatValue)) {
            settings.move.airAccelerate = floatValue;
        } else if (key == "move_gravity" && parseFloat(value, floatValue)) {
            settings.move.gravity = floatValue;
        } else if (key == "move_jump_speed" && parseFloat(value, floatValue)) {
            settings.move.jumpSpeed = floatValue;
        } else if (key == "move_ground_y" && parseFloat(value, floatValue)) {
            settings.move.groundY = floatValue;
        } else if (key.rfind("move_", 0) == 0) {
            result.warnings.push_back("line " + std::to_string(lineNumber) + ": bad numeric value for " + key);
        } else {
            settings.setRule(trimmed.substr(0, keyEnd), value);
        }
    }
    settings.clampToValid();
    return result;
}

std::string generateServerConfig(const ServerSettings& settingsIn) {
    ServerSettings settings = settingsIn;
    settings.clampToValid();
    std::string out;
    out += "// vibe_server configuration\n";
    out += "// Generated by the editor Server workspace (Compile Server).\n";
    out += "// Source-style: key value per line, // comments.\n\n";
    out += "hostname \"" + settings.serverName + "\"\n";
    out += "tick_rate " + std::to_string(settings.tickRate) + "\n";
    out += "max_players " + std::to_string(settings.maxPlayers) + "\n";
    out += "port " + std::to_string(settings.port) + "\n\n";
    out += "// Movement rules (client prediction simulates these identically)\n";
    out += "move_max_speed " + formatFloat(settings.move.maxSpeed) + "\n";
    out += "move_friction " + formatFloat(settings.move.friction) + "\n";
    out += "move_stop_speed " + formatFloat(settings.move.stopSpeed) + "\n";
    out += "move_accelerate " + formatFloat(settings.move.accelerate) + "\n";
    out += "move_air_accelerate " + formatFloat(settings.move.airAccelerate) + "\n";
    out += "move_gravity " + formatFloat(settings.move.gravity) + "\n";
    out += "move_jump_speed " + formatFloat(settings.move.jumpSpeed) + "\n";
    out += "move_ground_y " + formatFloat(settings.move.groundY) + "\n";
    if (!settings.rules.empty()) {
        out += "\n// Custom server rules\n";
        for (const auto& [key, value] : settings.rules)
            out += key + " \"" + value + "\"\n";
    }
    return out;
}

bool loadServerConfigFile(const std::filesystem::path& path, ServerSettings& out, std::string* error) {
    std::string text;
    if (!readTextFile(path, text, error))
        return false;
    out = parseServerConfig(text).settings;
    return true;
}

bool saveServerConfigFile(const std::filesystem::path& path, const ServerSettings& settings, std::string* error) {
    return writeTextAtomic(path, generateServerConfig(settings), error);
}

// --- ServerWorkspace ---------------------------------------------------------------

std::string ServerWorkspace::toJson() const {
    json::Value root = json::Value::object();
    root.set("version", json::Value::number(1));
    json::Value settingsJson = json::Value::object();
    settingsJson.set("serverName", json::Value::string(settings.serverName));
    settingsJson.set("tickRate", json::Value::number(settings.tickRate));
    settingsJson.set("maxPlayers", json::Value::number(settings.maxPlayers));
    settingsJson.set("port", json::Value::number(settings.port));
    json::Value moveJson = json::Value::object();
    moveJson.set("maxSpeed", json::Value::number(settings.move.maxSpeed));
    moveJson.set("friction", json::Value::number(settings.move.friction));
    moveJson.set("stopSpeed", json::Value::number(settings.move.stopSpeed));
    moveJson.set("accelerate", json::Value::number(settings.move.accelerate));
    moveJson.set("airAccelerate", json::Value::number(settings.move.airAccelerate));
    moveJson.set("gravity", json::Value::number(settings.move.gravity));
    moveJson.set("jumpSpeed", json::Value::number(settings.move.jumpSpeed));
    moveJson.set("groundY", json::Value::number(settings.move.groundY));
    settingsJson.set("move", std::move(moveJson));
    json::Value rulesJson = json::Value::array();
    for (const auto& [key, value] : settings.rules) {
        json::Value ruleJson = json::Value::object();
        ruleJson.set("key", json::Value::string(key));
        ruleJson.set("value", json::Value::string(value));
        rulesJson.push(std::move(ruleJson));
    }
    settingsJson.set("rules", std::move(rulesJson));
    root.set("settings", std::move(settingsJson));
    json::Value scriptsJson = json::Value::array();
    for (const ServerScriptEntry& script : scripts) {
        json::Value scriptJson = json::Value::object();
        scriptJson.set("name", json::Value::string(script.name));
        scriptJson.set("source", json::Value::string(script.source));
        scriptJson.set("enabled", json::Value::boolean(script.enabled));
        scriptsJson.push(std::move(scriptJson));
    }
    root.set("scripts", std::move(scriptsJson));
    return json::stringify(root);
}

bool ServerWorkspace::fromJson(std::string_view text, std::string* error) {
    json::Value root;
    std::string parseError;
    if (!json::parse(text, root, &parseError)) {
        if (error != nullptr)
            *error = parseError;
        return false;
    }
    ServerWorkspace parsed;
    if (const json::Value* settingsJson = root.find("settings")) {
        ServerSettings& s = parsed.settings;
        if (const json::Value* v = settingsJson->find("serverName"))
            s.serverName = v->asString();
        if (const json::Value* v = settingsJson->find("tickRate"))
            s.tickRate = static_cast<int>(v->asNumber(s.tickRate));
        if (const json::Value* v = settingsJson->find("maxPlayers"))
            s.maxPlayers = static_cast<int>(v->asNumber(s.maxPlayers));
        if (const json::Value* v = settingsJson->find("port"))
            s.port = static_cast<std::uint16_t>(v->asNumber(s.port));
        if (const json::Value* moveJson = settingsJson->find("move")) {
            if (const json::Value* v = moveJson->find("maxSpeed"))
                s.move.maxSpeed = static_cast<float>(v->asNumber(s.move.maxSpeed));
            if (const json::Value* v = moveJson->find("friction"))
                s.move.friction = static_cast<float>(v->asNumber(s.move.friction));
            if (const json::Value* v = moveJson->find("stopSpeed"))
                s.move.stopSpeed = static_cast<float>(v->asNumber(s.move.stopSpeed));
            if (const json::Value* v = moveJson->find("accelerate"))
                s.move.accelerate = static_cast<float>(v->asNumber(s.move.accelerate));
            if (const json::Value* v = moveJson->find("airAccelerate"))
                s.move.airAccelerate = static_cast<float>(v->asNumber(s.move.airAccelerate));
            if (const json::Value* v = moveJson->find("gravity"))
                s.move.gravity = static_cast<float>(v->asNumber(s.move.gravity));
            if (const json::Value* v = moveJson->find("jumpSpeed"))
                s.move.jumpSpeed = static_cast<float>(v->asNumber(s.move.jumpSpeed));
            if (const json::Value* v = moveJson->find("groundY"))
                s.move.groundY = static_cast<float>(v->asNumber(s.move.groundY));
        }
        if (const json::Value* rulesJson = settingsJson->find("rules")) {
            for (const json::Value& ruleJson : rulesJson->items()) {
                const json::Value* key = ruleJson.find("key");
                const json::Value* value = ruleJson.find("value");
                if (key != nullptr)
                    s.rules.emplace_back(key->asString(), value != nullptr ? value->asString() : std::string{});
            }
        }
        s.clampToValid();
    }
    if (const json::Value* scriptsJson = root.find("scripts")) {
        for (const json::Value& scriptJson : scriptsJson->items()) {
            ServerScriptEntry entry;
            if (const json::Value* v = scriptJson.find("name"))
                entry.name = v->asString();
            if (const json::Value* v = scriptJson.find("source"))
                entry.source = v->asString();
            if (const json::Value* v = scriptJson.find("enabled"))
                entry.enabled = v->asBool(true);
            parsed.scripts.push_back(std::move(entry));
        }
    }
    *this = std::move(parsed);
    return true;
}

bool ServerWorkspace::saveToFile(const std::filesystem::path& path, std::string* error) const {
    return writeTextAtomic(path, toJson(), error);
}

bool ServerWorkspace::loadFromFile(const std::filesystem::path& path, std::string* error) {
    std::string text;
    if (!readTextFile(path, text, error))
        return false;
    return fromJson(text, error);
}

ServerWorkspace::CompileResult ServerWorkspace::compile(const std::filesystem::path& outputDir) const {
    CompileResult result;
    // Phase 1: validate every enabled server script with the real
    // DSL parser. Any failure aborts the whole compile and nothing
    // is written — vibe_server never deploys a half-broken set.
    std::vector<std::string> stems;
    for (const ServerScriptEntry& script : scripts) {
        if (!script.enabled)
            continue;
        const std::string stem = sanitizeScriptStem(script.name);
        if (std::find(stems.begin(), stems.end(), stem) != stems.end())
            result.errors.push_back("server script '" + script.name + "': duplicate deployed name '" + stem + "'");
        stems.push_back(stem);
        const DSLParseResult parsed = parseBackroomsScript(script.source);
        for (const std::string& parseError : parsed.errors)
            result.errors.push_back("server script '" + script.name + "': " + parseError);
    }
    if (!result.errors.empty())
        return result;

    // Phase 2: write the deployable set.
    std::string error;
    const auto write = [&](const std::filesystem::path& path, const std::string& text) {
        if (writeTextAtomic(path, text, &error))
            result.writtenFiles.push_back(path.string());
        else
            result.errors.push_back(error);
    };
    write(outputDir / "server.cfg", generateServerConfig(settings));
    write(outputDir / "server_workspace.json", toJson());

    json::Value manifest = json::Value::object();
    json::Value manifestScripts = json::Value::array();
    std::size_t stemIndex = 0;
    for (const ServerScriptEntry& script : scripts) {
        if (!script.enabled)
            continue;
        const std::string& stem = stems[stemIndex++];
        write(outputDir / "server_scripts" / (stem + ".dsla"), script.source);
        json::Value entry = json::Value::object();
        entry.set("name", json::Value::string(script.name));
        entry.set("file", json::Value::string("server_scripts/" + stem + ".dsla"));
        manifestScripts.push(std::move(entry));
    }
    manifest.set("scripts", std::move(manifestScripts));
    write(outputDir / "server_scripts.json", json::stringify(manifest));

    result.ok = result.errors.empty();
    return result;
}

} // namespace engine
