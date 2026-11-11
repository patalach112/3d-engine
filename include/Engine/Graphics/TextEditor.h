#pragma once
#include "Core/Export.h"

#include "Graphics/UIContext.h"

#include <glm/vec4.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace engine {

// ---------------------------------------------------------------------------
// TextEditor — the engine's built-in multi-line code editor widget.
//
// Zero external dependencies: it renders through UIBatch
// (DynamicFont glyphs) inside UIContext frames like every other
// editor widget, and reads caret keys / typed text from
// UIInput::editorKeys + textInput. The buffer core (lines, caret,
// insert/delete, navigation) is deliberately independent of the
// drawing code so it is unit-testable headlessly; draw() adds
// gutter, syntax highlighting, caret and mouse mapping on top.
//
// Editing is byte-based with UTF-8-aware caret steps (the DSL is
// ASCII; other text degrades to per-codepoint movement, never to
// invalid splits). One TextEditor instance is bound to one entity
// at a time (the script dock): loadText() replaces the buffer,
// text() returns it for Save & Compile, and dirty() tracks edits
// since the last load/save.
// ---------------------------------------------------------------------------

enum class DslHighlightKind : uint8_t { Plain, Keyword, Event, Builtin, Number, String, Comment };

struct DslHighlightSpan {
    uint32_t offset = 0; // byte offset into the line
    uint32_t length = 0; // byte length
    DslHighlightKind kind = DslHighlightKind::Plain;
};

// Tokenises one DSL source line into coloured spans (comments,
// strings, durations/numbers, keywords, event names, builtins).
[[nodiscard]] ENGINE_GRAPHICS_API std::vector<DslHighlightSpan> highlightDslLine(std::string_view line);
[[nodiscard]] ENGINE_GRAPHICS_API glm::vec4 dslHighlightColor(DslHighlightKind kind);

class ENGINE_GRAPHICS_API TextEditor {
public:
    struct Result {
        bool changed = false;       // buffer text changed this frame
        bool saveRequested = false; // Ctrl+S pressed while focused
    };

    void loadText(std::string_view text);
    [[nodiscard]] std::string text() const;
    void markClean() { m_dirty = false; }
    [[nodiscard]] bool dirty() const { return m_dirty; }

    [[nodiscard]] size_t lineCount() const { return m_lines.size(); }
    [[nodiscard]] const std::string& line(size_t index) const { return m_lines[index]; }
    [[nodiscard]] size_t cursorLine() const { return m_cursorLine; }
    [[nodiscard]] size_t cursorColumn() const { return m_cursorCol; }
    void setCursor(size_t line, size_t columnBytes);

    // Buffer operations (all clamp the caret into validity).
    void insertText(std::string_view text); // handles '\n'
    void insertNewline();
    void backspace();
    void deleteForward();
    void moveLeft(bool word = false);
    void moveRight(bool word = false);
    void moveUp();
    void moveDown();
    void moveHome();
    void moveEnd();
    void movePage(int direction, size_t visibleRows);

    // Dock binding: which entity/file this buffer belongs to.
    // (A raw entity id, not entt::entity: EngineGraphics does not
    // link EnTT. kNoEntity means unbound.)
    static constexpr uint32_t kNoEntity = 0xFFFFFFFFu;
    void bind(uint32_t entityId, std::string displayName, std::string filePath) {
        m_boundEntity = entityId;
        m_displayName = std::move(displayName);
        m_filePath = std::move(filePath);
    }
    [[nodiscard]] uint32_t boundEntity() const { return m_boundEntity; }
    [[nodiscard]] const std::string& displayName() const { return m_displayName; }
    [[nodiscard]] const std::string& filePath() const { return m_filePath; }
    void setFilePath(std::string path) { m_filePath = std::move(path); }

    [[nodiscard]] bool focused() const { return m_focused; }
    void setFocused(bool focused) { m_focused = focused; }

    // Draws the editor into `rect` and applies this frame's input.
    // Call between UIContext::beginFrame/endFrame.
    Result draw(UIContext& ui, const UIRect& rect, float deltaSeconds);

    static constexpr float kTextScale = 1.45f; // editor font scale (vs UIContext::kTextScale chrome)

private:
    void clampCursor();
    void ensureCaretVisible(float rowHeight, float viewHeight);
    [[nodiscard]] size_t columnAtX(UIBatch& batch, const std::string& lineText, float localX) const;

    std::vector<std::string> m_lines{std::string{}};
    size_t m_cursorLine = 0;
    size_t m_cursorCol = 0;   // byte offset into the current line
    size_t m_goalColumn = 0;  // remembered column for vertical moves
    size_t m_scrollLine = 0;
    bool m_dirty = false;
    bool m_focused = false;
    float m_blinkSeconds = 0.0f;
    bool m_prevMouseDown = false;
    // Held-key repeat: which action is repeating and for how long.
    int m_repeatAction = 0; // 0 none, else an action code below
    float m_repeatSeconds = 0.0f;
    uint32_t m_boundEntity = kNoEntity;
    std::string m_displayName;
    std::string m_filePath;
};

} // namespace engine
