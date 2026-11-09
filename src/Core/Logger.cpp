#include "Core/Logger.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace engine {
namespace {

std::string formatTimestamp(std::chrono::system_clock::time_point timePoint) {
    const std::time_t time = std::chrono::system_clock::to_time_t(timePoint);
    const long long milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(timePoint.time_since_epoch()).count() % 1000;

    std::tm localTime{};
#ifdef _WIN32
    localtime_s(&localTime, &time);
#else
    localtime_r(&time, &localTime);
#endif

    std::array<char, 32> buffer{};
    const size_t written = std::strftime(buffer.data(), buffer.size(), "%Y-%m-%d %H:%M:%S", &localTime);
    std::string result(buffer.data(), written);
    result += '.';
    const std::string ms = std::to_string(milliseconds);
    result.append(3 - ms.size(), '0');
    result += ms;
    return result;
}

std::string timestampForFileName(std::chrono::system_clock::time_point timePoint) {
    const std::time_t time = std::chrono::system_clock::to_time_t(timePoint);
    std::tm localTime{};
#ifdef _WIN32
    localtime_s(&localTime, &time);
#else
    localtime_r(&time, &localTime);
#endif
    std::array<char, 32> buffer{};
    const size_t written = std::strftime(buffer.data(), buffer.size(), "%Y%m%d-%H%M%S", &localTime);
    return {buffer.data(), written};
}

std::string_view baseName(const char* file) {
    std::string_view path = file != nullptr ? file : "";
    const size_t separator = path.find_last_of("/\\");
    return separator == std::string_view::npos ? path : path.substr(separator + 1);
}

bool streamIsTerminal(std::FILE* stream) {
#ifdef _WIN32
    return _isatty(_fileno(stream)) != 0;
#else
    return isatty(fileno(stream)) != 0;
#endif
}

const char* colorCode(LogLevel level) {
    switch (level) {
    case LogLevel::Trace: return "\033[90m";     // bright black / gray
    case LogLevel::Debug: return "\033[36m";     // cyan
    case LogLevel::Info: return "\033[32m";      // green
    case LogLevel::Warning: return "\033[33m";   // yellow
    case LogLevel::Error: return "\033[31m";     // red
    case LogLevel::Fatal: return "\033[1;31m";   // bold red
    case LogLevel::Off: break;
    }
    return "";
}

constexpr const char* kColorReset = "\033[0m";

} // namespace

const char* toString(LogLevel level) {
    switch (level) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO";
    case LogLevel::Warning: return "WARNING";
    case LogLevel::Error: return "ERROR";
    case LogLevel::Fatal: return "FATAL";
    case LogLevel::Off: return "OFF";
    }
    return "UNKNOWN";
}

bool tryParseLogLevel(std::string_view text, LogLevel& outLevel) {
    std::string lowered;
    lowered.reserve(text.size());
    for (const char c : text) {
        lowered += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (lowered == "trace") {
        outLevel = LogLevel::Trace;
    } else if (lowered == "debug") {
        outLevel = LogLevel::Debug;
    } else if (lowered == "info") {
        outLevel = LogLevel::Info;
    } else if (lowered == "warning" || lowered == "warn") {
        outLevel = LogLevel::Warning;
    } else if (lowered == "error") {
        outLevel = LogLevel::Error;
    } else if (lowered == "fatal") {
        outLevel = LogLevel::Fatal;
    } else if (lowered == "off" || lowered == "none") {
        outLevel = LogLevel::Off;
    } else {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Sinks
// ---------------------------------------------------------------------------
namespace detail {

struct LogRecord {
    LogLevel level;
    std::string text; // fully formatted line, without trailing newline
};

class ILogSink {
public:
    virtual ~ILogSink() = default;
    virtual void write(const LogRecord& record) = 0;
    virtual void flush() = 0;
};

class ConsoleSink final : public ILogSink {
public:
    explicit ConsoleSink(bool colorEnabled)
        : m_colorEnabled(colorEnabled),
          m_stdoutIsTerminal(streamIsTerminal(stdout)),
          m_stderrIsTerminal(streamIsTerminal(stderr)) {
#ifdef _WIN32
        if (colorEnabled) {
            // Enable ANSI escape sequences on the Windows console.
            for (const DWORD handleId : {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE}) {
                HANDLE handle = GetStdHandle(handleId);
                DWORD mode = 0;
                if (handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode)) {
                    SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
                }
            }
        }
#endif
    }

    void write(const LogRecord& record) override {
        const bool toError = record.level >= LogLevel::Error;
        std::lock_guard lock(m_mutex);
        std::ostream& stream = toError ? std::cerr : std::cout;
        const bool color = m_colorEnabled && (toError ? m_stderrIsTerminal : m_stdoutIsTerminal);
        if (color) {
            stream << colorCode(record.level) << record.text << kColorReset << '\n';
        } else {
            stream << record.text << '\n';
        }
    }

    void flush() override {
        std::lock_guard lock(m_mutex);
        std::cout.flush();
        std::cerr.flush();
    }

private:
    bool m_colorEnabled;
    bool m_stdoutIsTerminal;
    bool m_stderrIsTerminal;
    std::mutex m_mutex;
};

class FileSink final : public ILogSink {
public:
    explicit FileSink(LoggerConfig config) : m_config(std::move(config)) {
        openFile();
    }

    [[nodiscard]] std::string filePath() const { return m_activePath.string(); }

    void write(const LogRecord& record) override {
        std::lock_guard lock(m_mutex);
        const size_t lineSize = record.text.size() + 1; // + newline
        if (m_config.maxFileSizeBytes > 0 && m_bytesWritten > 0 &&
            m_bytesWritten + lineSize > m_config.maxFileSizeBytes) {
            rotate();
        }
        m_stream << record.text << '\n';
        m_bytesWritten += lineSize;
    }

    void flush() override {
        std::lock_guard lock(m_mutex);
        m_stream.flush();
    }

private:
    std::filesystem::path rotatedPath(uint32_t index) const {
        std::filesystem::path path = m_activePath;
        const std::string archiveName =
            path.stem().string() + "." + std::to_string(index) + path.extension().string();
        return path.parent_path() / archiveName;
    }

    void openFile() {
        std::filesystem::path path = m_config.filePath;
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path());
        }
        if (m_config.timestampedFileName) {
            const std::string stampedName = path.stem().string() + "_" +
                                            timestampForFileName(std::chrono::system_clock::now()) +
                                            path.extension().string();
            path = path.parent_path() / stampedName;
        }
        m_activePath = path;

        std::error_code sizeError;
        const uintmax_t existingSize =
            m_config.appendToFile ? std::filesystem::file_size(path, sizeError) : 0;
        m_bytesWritten = sizeError ? 0 : existingSize;

        const std::ios::openmode mode = std::ios::out | (m_config.appendToFile ? std::ios::app : std::ios::trunc);
        m_stream.open(path, mode);
        if (!m_stream.is_open()) {
            throw std::runtime_error("failed to open log file: " + path.string());
        }
    }

    void rotate() {
        m_stream.flush();
        m_stream.close();

        std::error_code error;
        if (m_config.maxRotatedFiles == 0) {
            std::filesystem::remove(m_activePath, error);
        } else {
            std::filesystem::remove(rotatedPath(m_config.maxRotatedFiles), error);
            for (uint32_t i = m_config.maxRotatedFiles - 1; i >= 1; --i) {
                std::filesystem::rename(rotatedPath(i), rotatedPath(i + 1), error);
            }
            std::filesystem::rename(m_activePath, rotatedPath(1), error);
        }

        m_bytesWritten = 0;
        m_stream.open(m_activePath, std::ios::out | std::ios::trunc);
        if (!m_stream.is_open()) {
            throw std::runtime_error("failed to reopen log file after rotation: " + m_activePath.string());
        }
    }

    LoggerConfig m_config;
    std::filesystem::path m_activePath;
    std::ofstream m_stream;
    uintmax_t m_bytesWritten = 0;
    std::mutex m_mutex;
};

// Decorator that moves another sink's writes onto a background thread.
class AsyncSink final : public ILogSink {
public:
    AsyncSink(std::unique_ptr<ILogSink> inner, size_t queueCapacity)
        : m_inner(std::move(inner)), m_queueCapacity(queueCapacity), m_worker([this] { run(); }) {}

    ~AsyncSink() override {
        {
            std::lock_guard lock(m_mutex);
            m_stopping = true;
        }
        m_condition.notify_all();
        if (m_worker.joinable()) {
            m_worker.join();
        }
        const size_t dropped = m_droppedCount.load(std::memory_order_relaxed);
        if (dropped > 0 && m_inner != nullptr) {
            try {
                m_inner->write({LogLevel::Warning,
                                "[Logger] " + std::to_string(dropped) +
                                    " log message(s) were dropped because the async queue was full"});
                m_inner->flush();
            } catch (...) {
            }
        }
    }

    void write(const LogRecord& record) override {
        {
            std::lock_guard lock(m_mutex);
            if (m_stopping) {
                return;
            }
            if (m_queue.size() >= m_queueCapacity) {
                m_droppedCount.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            m_queue.push_back(record);
        }
        m_condition.notify_one();
    }

    void flush() override {
        std::unique_lock lock(m_mutex);
        m_condition.wait(lock, [this] { return m_queue.empty() && !m_writing; });
        lock.unlock();
        if (m_inner != nullptr) {
            m_inner->flush();
        }
    }

private:
    void run() {
        while (true) {
            std::deque<LogRecord> batch;
            {
                std::unique_lock lock(m_mutex);
                m_condition.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
                if (m_queue.empty() && m_stopping) {
                    return;
                }
                batch.swap(m_queue);
                m_writing = true;
            }
            for (const LogRecord& record : batch) {
                try {
                    m_inner->write(record);
                } catch (...) {
                    // A failing sink must never take the logging thread down.
                }
            }
            if (m_inner != nullptr) {
                m_inner->flush();
            }
            {
                std::lock_guard lock(m_mutex);
                m_writing = false;
            }
            m_condition.notify_all();
        }
    }

    std::unique_ptr<ILogSink> m_inner;
    size_t m_queueCapacity;
    std::deque<LogRecord> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_condition;
    bool m_stopping = false;
    bool m_writing = false;
    std::atomic<size_t> m_droppedCount{0};
    std::thread m_worker;
};

} // namespace detail

// ---------------------------------------------------------------------------
// Logger
// ---------------------------------------------------------------------------
Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

Logger::~Logger() {
    shutdown();
}

void Logger::initialize(const LoggerConfig& config) {
    shutdown();

    std::lock_guard lock(m_sinkMutex);
    m_minimumLevel.store(config.minimumLevel, std::memory_order_relaxed);
    m_activeFilePath.clear();

    if (config.consoleEnabled) {
        m_sinks.push_back(std::make_unique<detail::ConsoleSink>(config.consoleColorEnabled));
    }

    if (config.fileEnabled) {
        try {
            auto fileSink = std::make_unique<detail::FileSink>(config);
            m_activeFilePath = fileSink->filePath();
            if (config.asyncFileWrites) {
                m_sinks.push_back(
                    std::make_unique<detail::AsyncSink>(std::move(fileSink), config.asyncQueueCapacity));
            } else {
                m_sinks.push_back(std::move(fileSink));
            }
        } catch (const std::exception& error) {
            // Degrade gracefully: keep logging to console, report once there.
            m_activeFilePath.clear();
            if (!config.consoleEnabled) {
                m_sinks.push_back(std::make_unique<detail::ConsoleSink>(false));
            }
            std::cerr << "[Logger] File sink unavailable (" << error.what() << "); logging to console only.\n";
        }
    }

    m_initialized.store(true, std::memory_order_relaxed);
}

void Logger::shutdown() {
    if (!m_initialized.exchange(false, std::memory_order_relaxed) && m_sinks.empty()) {
        return;
    }
    flush();
    std::lock_guard lock(m_sinkMutex);
    m_sinks.clear(); // AsyncSink destructors drain their queues first.
}

void Logger::flush() {
    std::lock_guard lock(m_sinkMutex);
    for (const auto& sink : m_sinks) {
        try {
            sink->flush();
        } catch (...) {
        }
    }
}

std::string Logger::activeFilePath() const {
    std::lock_guard lock(m_sinkMutex);
    return m_activeFilePath;
}

void Logger::log(LogLevel level, const char* file, int line, std::string_view message) {
    if (!isEnabled(level)) {
        // Pre-initialization fallback: never swallow errors silently during
        // early startup (e.g. while the configuration itself is loading).
        if (!m_initialized.load(std::memory_order_relaxed) && level >= LogLevel::Warning) {
            std::cerr << '[' << toString(level) << "] " << message << '\n';
        }
        return;
    }

    detail::LogRecord record;
    record.level = level;
    record.text = "[" + formatTimestamp(std::chrono::system_clock::now()) + "] [" + toString(level) + "] ";
    record.text.append(message);
    record.text += " (" + std::string(baseName(file)) + ":" + std::to_string(line) + ")";

    {
        std::lock_guard lock(m_sinkMutex);
        for (const auto& sink : m_sinks) {
            try {
                sink->write(record);
            } catch (...) {
                // Logging must never throw into engine code.
            }
        }
    }

    // Errors are almost always followed by a crash or shutdown; make sure
    // they reach the file before the process can die.
    if (level >= LogLevel::Error) {
        flush();
    }
}

} // namespace engine
