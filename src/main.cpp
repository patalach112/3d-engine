#include "Core/Application.h"
#include "Core/EngineConfig.h"
#include "Core/Logger.h"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <utility>

namespace {

void printUsage(const char* program) {
    std::cout << "Usage: " << program << " [--config <path>]\n"
              << "  --config <path>  Engine configuration file (default: config/engine.ini, "
                 "or $ENGINE_CONFIG_PATH)\n"
              << "  --help           Show this message\n";
}

// Returns the config path (possibly empty), or false for --help/errors.
bool parseArguments(int argc, char* argv[], std::string& configPath) {
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--help" || argument == "-h") {
            printUsage(argv[0]);
            return false;
        }
        if (argument == "--config" && i + 1 < argc) {
            configPath = argv[++i];
        } else if (argument.starts_with("--config=")) {
            configPath = argument.substr(std::string("--config=").size());
        } else {
            std::cerr << "Unknown argument: " << argument << '\n';
            printUsage(argv[0]);
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    std::string configPath;
    if (!parseArguments(argc, argv, configPath)) {
        return EXIT_SUCCESS;
    }

    // Bootstrap order matters: configuration first, then the logger (so all
    // engine systems can log), then the application itself.
    engine::EngineConfig config = engine::EngineConfig::load(configPath);
    engine::Logger::instance().initialize(config.logger);

    for (const std::string& warning : config.loadWarnings) {
        ENGINE_LOG_WARNING(warning);
    }
    if (config.sourcePath.empty()) {
        ENGINE_LOG_INFO("Running with built-in default configuration");
    } else {
        ENGINE_LOG_INFO("Configuration loaded from " << config.sourcePath);
    }
    if (const std::string logFile = engine::Logger::instance().activeFilePath(); !logFile.empty()) {
        ENGINE_LOG_INFO("Writing log file: " << logFile);
    }

    int exitCode = EXIT_SUCCESS;
    try {
        engine::Application app(std::move(config));
        app.run();
    } catch (const std::exception& error) {
        ENGINE_LOG_FATAL("Fatal error: " << error.what());
        exitCode = EXIT_FAILURE;
    }

    engine::Logger::instance().shutdown();
    return exitCode;
}
