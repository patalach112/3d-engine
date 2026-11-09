#pragma once
#include "Core/Export.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace engine {

// ---------------------------------------------------------------------------
// Log levels, ordered by severity. A message is emitted when its level is
// at or above the logger's configured minimum level.
// ---------------------------------------------------------------------------
enum class LogLevel : uint8_t {
    Trace = 0,
    Debug,
    Info,
    Warning,
    Error,
    Fatal,
    Off,
};

[[nodiscard]] ENGINE_FOUNDATION_API const char* toString(LogLevel level);
[[nodiscard]] ENGINE_FOUNDATION_API bool tryParseLogLevel(std::string_view text, LogLevel& outLevel);

// ---------------------------------------------------------------------------
// LoggerConfig — every knob of the logging system, populated from the
// engine configuration file (see config/engine.ini) or set in code.
// Nothing about paths, rotation or verbosity is hardcoded in the engine.
// ---------------------------------------------------------------------------
struct LoggerConfig {
    LogLevel minimumLevel = LogLevel::Info;

    // Console sink.
    bool consoleEnabled = true;
    bool consoleColorEnabled = true;

    // File sink.
    bool fileEnabled = true;
    std::filesystem::path filePath = "logs/engine.log";
    bool appendToFile = false;
    // Inserts "_YYYYMMDD-HHMMSS" before the extension of the active log file.
    bool timestampedFileName = true;
    // Size-based rotation: when the active file would exceed this many
    // bytes it is rotated to "<name>.1<ext>", older files shift up, and at
    // most maxRotatedFiles archives are kept. 0 disables rotation.
    uintmax_t maxFileSizeBytes = 5 * 1024 * 1024;
    uint32_t maxRotatedFiles = 3;

    // When true, file writes run on a dedicated background thread fed by a
    // bounded queue, so logging never blocks the game loop on disk I/O.
    // Messages beyond asyncQueueCapacity are dropped (and counted).
    bool asyncFileWrites = true;
    size_t asyncQueueCapacity = 8192;
};

namespace detail {
class ILogSink;
}

// ---------------------------------------------------------------------------
// Logger
//
// Thread-safe singleton front-end. Messages are formatted once on the
// calling thread (timestamp, level, source location) and dispatched to the
// configured sinks: a color-coded console sink and a file sink (optionally
// asynchronous, with size-based rotation). Use the ENGINE_LOG_* macros —
// they check the level first, so disabled levels cost (almost) nothing and
// automatically capture __FILE__/__LINE__.
// ---------------------------------------------------------------------------
class ENGINE_FOUNDATION_API Logger {
public:
    static Logger& instance();

    // (Re)initializes all sinks from the given configuration. File-sink
    // failures degrade to console-only logging instead of aborting startup.
    void initialize(const LoggerConfig& config);
    // Flushes and tears down all sinks. Idempotent.
    void shutdown();
    // Blocks until asynchronous sinks have written everything queued.
    void flush();

    [[nodiscard]] bool isEnabled(LogLevel level) const {
        return m_initialized.load(std::memory_order_relaxed) &&
               level >= m_minimumLevel.load(std::memory_order_relaxed) &&
               level != LogLevel::Off;
    }
    [[nodiscard]] LogLevel minimumLevel() const { return m_minimumLevel.load(std::memory_order_relaxed); }
    [[nodiscard]] bool isInitialized() const { return m_initialized.load(std::memory_order_relaxed); }
    // Path of the file actually being written (after timestamp insertion),
    // or empty when the file sink is disabled/unavailable.
    [[nodiscard]] std::string activeFilePath() const;

    // Core logging entry point used by the macros and direct callers.
    void log(LogLevel level, const char* file, int line, std::string_view message);

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

private:
    Logger() = default;
    ~Logger();

    std::vector<std::unique_ptr<detail::ILogSink>> m_sinks;
    // Guards the sink list itself (reconfiguration is rare and takes it
    // exclusively); ordinary log dispatch only holds a shared lock, so
    // concurrent producers reach the sinks' lock-free ingestion queues
    // without serializing on this mutex.
    mutable std::shared_mutex m_sinkMutex;
    std::atomic<LogLevel> m_minimumLevel{LogLevel::Info};
    std::atomic<bool> m_initialized{false};
    std::string m_activeFilePath;
};

namespace detail {

// RAII stream helper behind the ENGINE_LOG_* macros: accumulates a message
// with operator<< and submits it to the logger on destruction (end of the
// full expression), so call sites read naturally:
//   ENGINE_LOG_INFO("Loaded " << count << " assets");
class ENGINE_FOUNDATION_API LogMessage {
public:
    LogMessage(Logger& logger, LogLevel level, const char* file, int line)
        : m_logger(logger), m_level(level), m_file(file), m_line(line) {}
    ~LogMessage() {
        try {
            m_logger.log(m_level, m_file, m_line, m_stream.str());
        } catch (...) {
            // Logging must never throw into engine code.
        }
    }

    LogMessage(const LogMessage&) = delete;
    LogMessage& operator=(const LogMessage&) = delete;

    std::ostream& stream() { return m_stream; }

private:
    Logger& m_logger;
    LogLevel m_level;
    const char* m_file;
    int m_line;
    std::ostringstream m_stream;
};

} // namespace detail
} // namespace engine

// ---------------------------------------------------------------------------
// Logging macros. The level check happens before the message expression is
// evaluated, and every message carries its source file and line.
// ---------------------------------------------------------------------------
#define ENGINE_LOG_DETAIL(level, expression)                                                                   \
    do {                                                                                                       \
        ::engine::Logger& engineLogger = ::engine::Logger::instance();                                         \
        if (engineLogger.isEnabled(level)) {                                                                   \
            ::engine::detail::LogMessage engineLogMessage(engineLogger, level, __FILE__, __LINE__);            \
            engineLogMessage.stream() << expression;                                                           \
        }                                                                                                      \
    } while (false)

#define ENGINE_LOG_TRACE(expression) ENGINE_LOG_DETAIL(::engine::LogLevel::Trace, expression)
#define ENGINE_LOG_DEBUG(expression) ENGINE_LOG_DETAIL(::engine::LogLevel::Debug, expression)
#define ENGINE_LOG_INFO(expression) ENGINE_LOG_DETAIL(::engine::LogLevel::Info, expression)
#define ENGINE_LOG_WARNING(expression) ENGINE_LOG_DETAIL(::engine::LogLevel::Warning, expression)
#define ENGINE_LOG_WARN(expression) ENGINE_LOG_WARNING(expression)
#define ENGINE_LOG_ERROR(expression) ENGINE_LOG_DETAIL(::engine::LogLevel::Error, expression)
#define ENGINE_LOG_FATAL(expression) ENGINE_LOG_DETAIL(::engine::LogLevel::Fatal, expression)
