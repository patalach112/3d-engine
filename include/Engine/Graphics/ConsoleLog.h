#pragma once
#include "Core/Export.h"

#include "Core/Logger.h"
#include "Graphics/UIContext.h"

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace engine {

// ---------------------------------------------------------------------------
// ConsoleLog — the in-engine console's data + panel widget.
//
// Application registers a Logger listener that pushes every enabled
// log call here, so the console shows engine warnings/errors and
// DSL print() output (the interpreter logs those as "[script] ...")
// in real time, in Edit and Play mode alike. The buffer is a
// capped, mutex-guarded deque: the Logger may call from any thread,
// the panel only ever reads a snapshot.
//
// drawConsolePanel renders it as a native UI panel (level filter,
// Clear, autoscroll) through UIContext/UIBatch — no external UI
// library, like the rest of the editor.
// ---------------------------------------------------------------------------

struct ConsoleEntry {
    LogLevel level = LogLevel::Info;
    std::string text;
};

class ENGINE_GRAPHICS_API ConsoleLog {
public:
    static constexpr size_t kCapacity = 1024;

    void push(LogLevel level, std::string text);
    void clear();
    [[nodiscard]] std::vector<ConsoleEntry> snapshot() const;
    [[nodiscard]] size_t size() const;
    [[nodiscard]] uint64_t totalReceived() const { return m_totalReceived; }

    // "[script] ..." entries come from DSL print()/script logging
    // and get their own colour in the panel.
    [[nodiscard]] static bool isScriptEntry(const ConsoleEntry& entry) {
        return entry.text.starts_with("[script]");
    }
    [[nodiscard]] static bool levelVisible(LogLevel level, LogLevel minimum) { return level >= minimum; }
    [[nodiscard]] static glm::vec4 levelColor(const ConsoleEntry& entry);

private:
    mutable std::mutex m_mutex;
    std::deque<ConsoleEntry> m_entries;
    uint64_t m_totalReceived = 0;
};

struct ConsolePanelState {
    float scrollOffset = 0.0f;       // lines-from-bottom when not autoscrolling
    bool autoScroll = true;          // pin to the newest entry
    LogLevel minimumLevel = LogLevel::Trace; // All
};

// Draws the console into `rect` (header: title, count, level
// filter cycle, Clear; body: scrollable coloured lines). Call
// between UIContext::beginFrame/endFrame.
ENGINE_GRAPHICS_API void drawConsolePanel(UIContext& ui, const UIRect& rect, ConsoleLog& log,
                                          ConsolePanelState& state);

} // namespace engine
