#include "Core/EngineConfig.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

#ifndef ENGINE_DEFAULT_CONFIG_PATH
#define ENGINE_DEFAULT_CONFIG_PATH "config/engine.ini"
#endif

namespace engine {
namespace {

std::string trim(std::string_view text) {
    const auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    size_t begin = 0;
    while (begin < text.size() && isSpace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin && isSpace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

std::string toLower(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const char c : text) {
        result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return result;
}

using SectionMap = std::map<std::string, std::map<std::string, std::string>>;

bool parseBool(const std::string& text, bool& out) {
    const std::string value = toLower(text);
    if (value == "true" || value == "1" || value == "on" || value == "yes") {
        out = true;
        return true;
    }
    if (value == "false" || value == "0" || value == "off" || value == "no") {
        out = false;
        return true;
    }
    return false;
}

bool parseDouble(const std::string& text, double& out) {
    try {
        size_t consumed = 0;
        const double value = std::stod(text, &consumed);
        if (consumed != text.size()) {
            return false;
        }
        out = value;
        return true;
    } catch (...) {
        return false;
    }
}

bool parseInteger(const std::string& text, long long& out) {
    try {
        size_t consumed = 0;
        const long long value = std::stoll(text, &consumed);
        if (consumed != text.size()) {
            return false;
        }
        out = value;
        return true;
    } catch (...) {
        return false;
    }
}

struct ConfigReader {
    EngineConfig& config;
    std::string currentKey; // "section.key", for warning messages

    void warnInvalid(const std::string& value, const char* expected) {
        config.loadWarnings.push_back("Config key '" + currentKey + "' has invalid value '" + value +
                                      "' (expected " + expected + "); keeping default");
    }
    void warnOutOfRange(const std::string& value, const char* requirement) {
        config.loadWarnings.push_back("Config key '" + currentKey + "' value '" + value + "' violates " +
                                      requirement + "; keeping default");
    }

    // The apply* helpers return true (and assign) only when the value both
    // parses and lies in range; otherwise they record a warning and the
    // caller's field keeps its default.
    bool apply(const std::string& key, bool& field) {
        bool value = false;
        if (!parseBool(key, value)) {
            warnInvalid(key, "a boolean");
            return false;
        }
        field = value;
        return true;
    }
    bool applyDouble(const std::string& text, double& field, double minExclusive, double maxInclusive) {
        double value = 0.0;
        if (!parseDouble(text, value)) {
            warnInvalid(text, "a number");
            return false;
        }
        if (!(value > minExclusive && value <= maxInclusive)) {
            warnOutOfRange(text, "the allowed range");
            return false;
        }
        field = value;
        return true;
    }
    bool applyInteger(const std::string& text, long long& field, long long min, long long max) {
        long long value = 0;
        if (!parseInteger(text, value)) {
            warnInvalid(text, "an integer");
            return false;
        }
        if (value < min || value > max) {
            warnOutOfRange(text, "the allowed range");
            return false;
        }
        field = value;
        return true;
    }
};

bool applyValue(EngineConfig& config, const std::string& section, const std::string& key,
                const std::string& value) {
    ConfigReader reader{config, section + "." + key};
    long long intField = 0;

    // --- [Logger] ------------------------------------------------------------
    if (section == "logger") {
        if (key == "level") {
            LogLevel level = LogLevel::Info;
            if (tryParseLogLevel(value, level)) {
                config.logger.minimumLevel = level;
            } else {
                reader.warnInvalid(value, "one of Trace/Debug/Info/Warning/Error/Fatal/Off");
            }
        } else if (key == "console") {
            reader.apply(value, config.logger.consoleEnabled);
        } else if (key == "consolecolor") {
            reader.apply(value, config.logger.consoleColorEnabled);
        } else if (key == "file") {
            reader.apply(value, config.logger.fileEnabled);
        } else if (key == "filepath") {
            config.logger.filePath = value;
        } else if (key == "appendtofile") {
            reader.apply(value, config.logger.appendToFile);
        } else if (key == "timestampedfilename") {
            reader.apply(value, config.logger.timestampedFileName);
        } else if (key == "maxfilesizebytes") {
            if (reader.applyInteger(value, intField, 0, 1LL << 40)) {
                config.logger.maxFileSizeBytes = static_cast<uintmax_t>(intField);
            }
        } else if (key == "maxrotatedfiles") {
            if (reader.applyInteger(value, intField, 0, 100)) {
                config.logger.maxRotatedFiles = static_cast<uint32_t>(intField);
            }
        } else if (key == "asyncfilewrites") {
            reader.apply(value, config.logger.asyncFileWrites);
        } else if (key == "asyncqueuecapacity") {
            if (reader.applyInteger(value, intField, 16, 1 << 24)) {
                config.logger.asyncQueueCapacity = static_cast<size_t>(intField);
            }
        } else {
            return false;
        }
        return true;
    }

    // --- [Loop] --------------------------------------------------------------
    if (section == "loop") {
        if (key == "fixeddeltaseconds") {
            reader.applyDouble(value, config.loop.fixedDeltaSeconds, 0.0, 1.0);
        } else if (key == "maxfixedstepsperframe") {
            if (reader.applyInteger(value, intField, 1, 1000)) {
                config.loop.maxFixedStepsPerFrame = static_cast<uint32_t>(intField);
            }
        } else if (key == "maxframeseconds") {
            reader.applyDouble(value, config.loop.maxFrameSeconds, 0.0, 10.0);
        } else if (key == "maxfps") {
            if (reader.applyInteger(value, intField, 0, 10000)) {
                config.loop.maxFps = static_cast<uint32_t>(intField);
            }
        } else if (key == "fixedtimestepsenabled") {
            reader.apply(value, config.loop.fixedTimestepEnabled);
        } else if (key == "statslogintervalseconds") {
            reader.applyDouble(value, config.loop.statsLogIntervalSeconds, -1.0, 3600.0);
        } else {
            return false;
        }
        return true;
    }

    // --- [Window] --------------------------------------------------------------
    if (section == "window") {
        if (key == "width") {
            if (reader.applyInteger(value, intField, 1, 16384)) {
                config.window.width = static_cast<int>(intField);
            }
        } else if (key == "height") {
            if (reader.applyInteger(value, intField, 1, 16384)) {
                config.window.height = static_cast<int>(intField);
            }
        } else if (key == "title") {
            config.window.title = value;
        } else if (key == "resizable") {
            reader.apply(value, config.window.resizable);
        } else {
            return false;
        }
        return true;
    }

    // --- [Render] --------------------------------------------------------------
    if (section == "render") {
        if (key == "vsync") {
            reader.apply(value, config.render.vsync);
        } else if (key == "frustumculling") {
            reader.apply(value, config.render.frustumCulling);
        } else {
            return false;
        }
        return true;
    }

    // --- [Performance] -----------------------------------------------------------
    if (section == "performance") {
        if (key == "jobworkercount") {
            if (reader.applyInteger(value, intField, 0, 256)) {
                config.performance.jobWorkerCount = static_cast<uint32_t>(intField);
            }
        } else if (key == "entityreserve") {
            if (reader.applyInteger(value, intField, 0, 1000000)) {
                config.performance.entityReserve = static_cast<uint32_t>(intField);
            }
        } else if (key == "parallelmindrawitems") {
            if (reader.applyInteger(value, intField, 1, 1000000)) {
                config.performance.parallelMinDrawItems = static_cast<uint32_t>(intField);
            }
        } else {
            return false;
        }
        return true;
    }

    // --- [Camera] --------------------------------------------------------------
    if (section == "camera") {
        double doubleField = 0.0;
        if (key == "positionx") {
            if (reader.applyDouble(value, doubleField, -1e6, 1e6)) {
                config.camera.startPosition.x = static_cast<float>(doubleField);
            }
        } else if (key == "positiony") {
            if (reader.applyDouble(value, doubleField, -1e6, 1e6)) {
                config.camera.startPosition.y = static_cast<float>(doubleField);
            }
        } else if (key == "positionz") {
            if (reader.applyDouble(value, doubleField, -1e6, 1e6)) {
                config.camera.startPosition.z = static_cast<float>(doubleField);
            }
        } else if (key == "yawdegrees") {
            if (reader.applyDouble(value, doubleField, -1e6, 1e6)) {
                config.camera.startYawDegrees = static_cast<float>(doubleField);
            }
        } else if (key == "pitchdegrees") {
            if (reader.applyDouble(value, doubleField, -90.0, 90.0)) {
                config.camera.startPitchDegrees = static_cast<float>(doubleField);
            }
        } else if (key == "movementspeed") {
            if (reader.applyDouble(value, doubleField, 0.0, 1000.0)) {
                config.camera.movementSpeed = static_cast<float>(doubleField);
            }
        } else if (key == "boostmultiplier") {
            if (reader.applyDouble(value, doubleField, 0.0, 100.0)) {
                config.camera.boostMultiplier = static_cast<float>(doubleField);
            }
        } else if (key == "mousesensitivity") {
            if (reader.applyDouble(value, doubleField, 0.0, 10.0)) {
                config.camera.mouseSensitivity = static_cast<float>(doubleField);
            }
        } else if (key == "inverty") {
            reader.apply(value, config.camera.invertY);
        } else if (key == "pitchlimitdegrees") {
            if (reader.applyDouble(value, doubleField, 0.0, 90.0)) {
                config.camera.pitchLimitDegrees = static_cast<float>(doubleField);
            }
        } else if (key == "fieldofviewdegrees") {
            if (reader.applyDouble(value, doubleField, 1.0, 179.0)) {
                config.camera.fieldOfViewDegrees = static_cast<float>(doubleField);
            }
        } else if (key == "nearplane") {
            if (reader.applyDouble(value, doubleField, 0.0, 1000.0)) {
                config.camera.nearPlane = static_cast<float>(doubleField);
            }
        } else if (key == "farplane") {
            if (reader.applyDouble(value, doubleField, 0.0, 1e6)) {
                config.camera.farPlane = static_cast<float>(doubleField);
            }
        } else {
            return false;
        }
        return true;
    }

    // --- [Input] ---------------------------------------------------------------
    if (section == "input") {
        if (key == "capturemouseonstart") {
            reader.apply(value, config.input.captureMouseOnStart);
        } else if (key == "capturemouseonclick") {
            reader.apply(value, config.input.captureMouseOnClick);
        } else {
            return false;
        }
        return true;
    }

    return false;
}

SectionMap parseIniFile(const std::filesystem::path& path, EngineConfig& config) {
    SectionMap sections;
    std::ifstream file(path);
    if (!file.is_open()) {
        config.loadWarnings.push_back("Could not open config file '" + path.string() + "'; using defaults");
        return sections;
    }

    std::string section = "general";
    std::string line;
    int lineNumber = 0;
    while (std::getline(file, line)) {
        ++lineNumber;
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed.starts_with(';') || trimmed.starts_with('#')) {
            continue;
        }
        if (trimmed.starts_with('[') && trimmed.ends_with(']')) {
            section = toLower(trim(trimmed.substr(1, trimmed.size() - 2)));
            continue;
        }
        const size_t equals = trimmed.find('=');
        if (equals == std::string::npos) {
            config.loadWarnings.push_back("Config file '" + path.string() + "' line " +
                                          std::to_string(lineNumber) + " is not a key=value pair; ignored");
            continue;
        }
        const std::string key = toLower(trim(trimmed.substr(0, equals)));
        std::string value = trim(trimmed.substr(equals + 1));
        // Strip inline comments from unquoted values.
        if (const size_t comment = value.find_first_of(";#"); comment != std::string::npos) {
            value = trim(value.substr(0, comment));
        }
        sections[section][key] = value;
    }
    return sections;
}

EngineConfig loadFromExistingFile(const std::filesystem::path& path) {
    EngineConfig config;
    const SectionMap sections = parseIniFile(path, config);
    config.sourcePath = path.string();

    for (const auto& [section, entries] : sections) {
        for (const auto& [key, value] : entries) {
            if (!applyValue(config, section, key, value)) {
                config.loadWarnings.push_back("Unknown config key '" + section + "." + key + "'; ignored");
            }
        }
    }

    // Cross-field validation that single-key ranges cannot express.
    if (config.camera.nearPlane >= config.camera.farPlane) {
        config.loadWarnings.push_back("camera.nearPlane must be smaller than camera.farPlane; "
                                      "keeping default clip planes");
        const CameraConfig defaults;
        config.camera.nearPlane = defaults.nearPlane;
        config.camera.farPlane = defaults.farPlane;
    }
    return config;
}

} // namespace

EngineConfig EngineConfig::loadFromFile(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        EngineConfig config;
        config.loadWarnings.push_back("Config file '" + path.string() + "' not found; using defaults");
        return config;
    }
    return loadFromExistingFile(path);
}

EngineConfig EngineConfig::load(const std::string& explicitPath) {
    std::vector<std::filesystem::path> candidates;
    if (!explicitPath.empty()) {
        candidates.emplace_back(explicitPath);
    }
    if (const char* envPath = std::getenv("ENGINE_CONFIG_PATH"); envPath != nullptr && envPath[0] != '\0') {
        candidates.emplace_back(envPath);
    }
    candidates.emplace_back("config/engine.ini");
    candidates.emplace_back(ENGINE_DEFAULT_CONFIG_PATH);

    for (const std::filesystem::path& candidate : candidates) {
        if (std::filesystem::exists(candidate)) {
            return loadFromExistingFile(candidate);
        }
    }

    EngineConfig config;
    config.loadWarnings.push_back("No config file found (looked for config/engine.ini); "
                                  "using built-in defaults");
    if (!explicitPath.empty()) {
        config.loadWarnings.push_back("Explicitly requested config file '" + explicitPath + "' was not found");
    }
    return config;
}

} // namespace engine
