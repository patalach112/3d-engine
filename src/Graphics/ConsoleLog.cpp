#include "Graphics/ConsoleLog.h"

#include "Graphics/DynamicFont.h"

#include <algorithm>

namespace engine {

void ConsoleLog::push(LogLevel level, std::string text) {
    std::lock_guard lock(m_mutex);
    if (m_entries.size() >= kCapacity) {
        m_entries.pop_front();
    }
    m_entries.push_back(ConsoleEntry{level, std::move(text)});
    ++m_totalReceived;
}

void ConsoleLog::clear() {
    std::lock_guard lock(m_mutex);
    m_entries.clear();
}

std::vector<ConsoleEntry> ConsoleLog::snapshot() const {
    std::lock_guard lock(m_mutex);
    return {m_entries.begin(), m_entries.end()};
}

size_t ConsoleLog::size() const {
    std::lock_guard lock(m_mutex);
    return m_entries.size();
}

glm::vec4 ConsoleLog::levelColor(const ConsoleEntry& entry) {
    if (isScriptEntry(entry)) {
        return glm::vec4{0.45f, 0.85f, 0.55f, 1.0f}; // script green
    }
    switch (entry.level) {
    case LogLevel::Error:
    case LogLevel::Fatal: return glm::vec4{1.0f, 0.42f, 0.38f, 1.0f};
    case LogLevel::Warning: return glm::vec4{1.0f, 0.78f, 0.35f, 1.0f};
    case LogLevel::Debug:
    case LogLevel::Trace: return glm::vec4{0.55f, 0.60f, 0.70f, 1.0f};
    case LogLevel::Info:
    case LogLevel::Off: break;
    }
    return glm::vec4{0.85f, 0.87f, 0.90f, 1.0f};
}

void drawConsolePanel(UIContext& ui, const UIRect& rect, ConsoleLog& log, ConsolePanelState& state) {
    UIBatch* batch = ui.batch();
    if (batch == nullptr || rect.width < 80.0f || rect.height < 48.0f) {
        return;
    }
    const UITheme& theme = ui.theme();
    const float textScale = 1.9f; // console matches the script editor's scale
    float lineHeight = 15.0f * textScale;
    if (batch->font() != nullptr) {
        lineHeight = batch->font()->lineHeight(fontPixelSizeForScale(textScale)) * 1.15f;
    }
    const float headerHeight = 26.0f;

    ui.beginPanel("ConsolePanel", rect, UIRectStyle{-1.0f, -1.0f, glm::vec4{}});

    // Header: title + entry count, filter cycle, autoscroll, clear.
    batch->drawText("Console", rect.x + 8.0f, rect.y + 4.0f, 1.55f, theme.text);
    const std::string count = std::to_string(log.size()) + " entries";
    batch->drawText(count, rect.x + 118.0f, rect.y + 4.0f, 1.5f, glm::vec4{0.6f, 0.65f, 0.75f, 1.0f});
    const UIRect clearButton{rect.x + rect.width - 76.0f, rect.y + 3.0f, 68.0f, 20.0f};
    if (ui.button(clearButton, "Clear")) {
        log.clear();
        state.scrollOffset = 0.0f;
    }
    const UIRect scrollButton{rect.x + rect.width - 196.0f, rect.y + 3.0f, 112.0f, 20.0f};
    if (ui.button(scrollButton, state.autoScroll ? "Scroll: Auto" : "Scroll: Manual")) {
        state.autoScroll = !state.autoScroll;
    }
    const char* filterLabel = state.minimumLevel == LogLevel::Trace     ? "Level: All"
                              : state.minimumLevel == LogLevel::Info    ? "Level: Info+"
                              : state.minimumLevel == LogLevel::Warning ? "Level: Warn+"
                                                                        : "Level: Error";
    const UIRect filterButton{rect.x + rect.width - 292.0f, rect.y + 3.0f, 88.0f, 20.0f};
    if (ui.button(filterButton, filterLabel)) {
        state.minimumLevel = state.minimumLevel == LogLevel::Trace     ? LogLevel::Info
                             : state.minimumLevel == LogLevel::Info    ? LogLevel::Warning
                             : state.minimumLevel == LogLevel::Warning ? LogLevel::Error
                                                                       : LogLevel::Trace;
    }

    // Body: newest at the bottom; the wheel scrolls back through
    // history and disables autoscroll until it returns to the end.
    const std::vector<ConsoleEntry> entries = log.snapshot();
    std::vector<const ConsoleEntry*> visible;
    visible.reserve(entries.size());
    for (const ConsoleEntry& entry : entries) {
        if (ConsoleLog::levelVisible(entry.level, state.minimumLevel)) {
            visible.push_back(&entry);
        }
    }
    const float bodyTop = rect.y + headerHeight;
    const float bodyHeight = rect.height - headerHeight - 4.0f;
    const size_t rowsVisible = static_cast<size_t>(std::max(1.0f, bodyHeight / lineHeight));
    if (ui.hoveredRect(rect) && ui.scrollDelta() != 0.0f) {
        state.scrollOffset = std::max(0.0f, state.scrollOffset + ui.scrollDelta() * 3.0f);
        state.autoScroll = state.scrollOffset <= 0.0f;
    }
    const size_t total = visible.size();
    const size_t maxScroll = total > rowsVisible ? total - rowsVisible : 0;
    state.scrollOffset = std::min(state.scrollOffset, static_cast<float>(maxScroll));
    const size_t back = state.autoScroll ? 0 : static_cast<size_t>(state.scrollOffset);
    const size_t end = total > back ? total - back : 0;
    const size_t begin = end > rowsVisible ? end - rowsVisible : 0;
    batch->pushScissor(rect.x, bodyTop, rect.width, bodyHeight);
    float y = bodyTop + 2.0f;
    for (size_t i = begin; i < end; ++i) {
        const ConsoleEntry& entry = *visible[i];
        std::string_view text = entry.text;
        constexpr size_t kMaxChars = 220;
        const bool truncated = text.size() > kMaxChars;
        if (truncated) {
            text = text.substr(0, kMaxChars);
        }
        batch->drawText(text, rect.x + 8.0f, y, textScale, ConsoleLog::levelColor(entry));
        if (truncated) {
            batch->drawText("...", rect.x + 8.0f + batch->measureText(text, textScale), y, textScale,
                            ConsoleLog::levelColor(entry));
        }
        y += lineHeight;
    }
    batch->popScissor();
    ui.endPanel();
}

} // namespace engine
