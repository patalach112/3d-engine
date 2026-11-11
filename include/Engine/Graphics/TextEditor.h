#pragma once
#include "Core/Export.h"

#include "Graphics/UIContext.h"

#include <glm/vec4.hpp>

#include <cstdint>
#include <functional>
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

enum class DslHighlightKind : uint8_t { Plain, Keyword, Event, Builtin, Number, String, Comment, Variable };

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
    void markDirty() { m_dirty = true; }
    [[nodiscard]] bool dirty() const { return m_dirty; }

    // Real-time diagnostics (the host parses the buffer as it is
    // edited and pushes the result): error lines get a red gutter
    // number, a faint red row wash and an underline in draw().
    struct Diagnostic {
        size_t line = 0; // 0-based
        std::string message;
    };
    void setDiagnostics(std::vector<Diagnostic> diagnostics);
    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const { return m_diagnostics; }
    // Byte offset of the caret inside text() (host completion maths).
    [[nodiscard]] size_t cursorTextOffset() const;

    // Auto-completion popup. The host computes candidates (see
    // DslCompletion) and hands them over with the source offset the
    // accepted text should replace; the widget owns navigation
    // (Up/Down), acceptance (Enter/Tab or click) and dismissal
    // (Esc, remembered until the buffer changes). All state lives
    // in these methods so the behaviour is testable without draw().
    struct CompletionItem {
        std::string text;
        std::string detail;
    };
    void showCompletions(std::vector<CompletionItem> items, size_t replaceStartOffset);
    void closeCompletions();
    [[nodiscard]] bool completionsOpen() const { return m_completionsOpen; }
    [[nodiscard]] size_t completionCount() const { return m_completions.size(); }
    [[nodiscard]] size_t completionSelection() const { return m_completionIndex; }
    void moveCompletionSelection(int delta);
    bool acceptCompletion(); // false when closed/empty

    [[nodiscard]] size_t lineCount() const { return m_lines.size(); }
    [[nodiscard]] const std::string& line(size_t index) const { return m_lines[index]; }
    [[nodiscard]] size_t cursorLine() const { return m_cursorLine; }
    [[nodiscard]] size_t cursorColumn() const { return m_cursorCol; }
    void setCursor(size_t line, size_t columnBytes);

    // Buffer operations (all clamp the caret into validity).
    // Insert/delete replace the current selection first, like
    // every modern editor.
    void insertText(std::string_view text); // handles '\n'
    void insertNewline();
    void backspace();
    void deleteForward();
    void moveLeft(bool word = false, bool extend = false);
    void moveRight(bool word = false, bool extend = false);
    void moveUp(bool extend = false);
    void moveDown(bool extend = false);
    void moveHome(bool extend = false);
    void moveEnd(bool extend = false);
    void movePage(int direction, size_t visibleRows);

    // Selection (anchor + caret): mouse drags and Shift+caret
    // keys build it, typing replaces it.
    void setSelection(size_t anchorLine, size_t anchorColumn);
    void selectAll();
    void clearSelection() { m_selectionActive = false; }
    [[nodiscard]] bool hasSelection() const;
    [[nodiscard]] std::string selectionText() const;
    void deleteSelection();
    void selectionRange(size_t& startLine, size_t& startCol, size_t& endLine, size_t& endCol) const;

    // Clipboard: the host injects platform hooks (GLFW clipboard
    // via Window); without hooks these are safe no-ops returning
    // false, so headless tests exercise selection logic directly.
    struct ClipboardHooks {
        std::function<std::string()> getText;
        std::function<void(std::string_view)> setText;
    };
    void setClipboardHooks(ClipboardHooks hooks) { m_clipboard = std::move(hooks); }
    bool copySelection();
    bool cutSelection();
    bool pasteClipboard();

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

    static constexpr float kTextScale = 1.95f; // editor font scale (UI chrome is 2.0; code sits just under it)

private:
    void clampCursor();
    void beginSelection(); // anchor at caret when starting to extend
    void ensureCaretVisible(float rowHeight, float viewHeight);
    [[nodiscard]] size_t columnAtX(UIBatch& batch, const std::string& lineText, float localX) const;

    std::vector<std::string> m_lines{std::string{}};
    size_t m_cursorLine = 0;
    size_t m_cursorCol = 0;   // byte offset into the current line
    size_t m_anchorLine = 0;  // selection anchor (other end from the caret)
    size_t m_anchorCol = 0;
    bool m_selectionActive = false;
    bool m_dragging = false; // mouse drag-select in progress
    ClipboardHooks m_clipboard;
    size_t m_goalColumn = 0;  // remembered column for vertical moves
    size_t m_scrollLine = 0;
    bool m_dirty = false;
    bool m_focused = false;
    float m_blinkSeconds = 0.0f;
    bool m_prevMouseDown = false;
    // Diagnostics + completion state.
    std::vector<Diagnostic> m_diagnostics;
    std::vector<CompletionItem> m_completions;
    size_t m_completionIndex = 0;
    size_t m_completionReplaceLine = 0;
    size_t m_completionReplaceCol = 0;
    bool m_completionsOpen = false;
    bool m_completionDismissed = false; // Esc: stay closed until the buffer changes
    UIRect m_completionPopupRect{};
    bool m_completionPopupValid = false;
    // Held-key repeat: which action is repeating and for how long.
    int m_repeatAction = 0; // 0 none, else an action code below
    float m_repeatSeconds = 0.0f;
    uint32_t m_boundEntity = kNoEntity;
    std::string m_displayName;
    std::string m_filePath;
};

} // namespace engine
