#include "Graphics/TextEditor.h"

#include "Graphics/DynamicFont.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace engine {

namespace {

[[nodiscard]] bool isContinuation(unsigned char c) { return (c & 0xC0) == 0x80; }

[[nodiscard]] size_t prevCodepoint(const std::string& line, size_t col) {
    if (col == 0) {
        return 0;
    }
    size_t i = col - 1;
    while (i > 0 && isContinuation(static_cast<unsigned char>(line[i]))) {
        --i;
    }
    return i;
}

[[nodiscard]] size_t nextCodepoint(const std::string& line, size_t col) {
    if (col >= line.size()) {
        return line.size();
    }
    size_t i = col + 1;
    while (i < line.size() && isContinuation(static_cast<unsigned char>(line[i]))) {
        ++i;
    }
    return i;
}

[[nodiscard]] bool isWordByte(unsigned char c) { return std::isalnum(c) != 0 || c == '_'; }

[[nodiscard]] DslHighlightKind classifyWord(std::string_view word) {
    using enum DslHighlightKind;
    if (word == "from" || word == "start" || word == "script" || word == "end" || word == "if" || word == "for" ||
        word == "every" || word == "inhand" || word == "part" || word == "position" || word == "behavior") {
        return Keyword;
    }
    if (word == "on_touch" || word == "on_collide" || word == "on_collision" || word == "on_interact" ||
        word == "on_input" || word == "on_hold") {
        return Event;
    }
    if (word == "play_sound" || word == "emit_sound" || word == "print" || word == "destroy" || word == "freeze" ||
        word == "reset" || word == "stalker" || word == "scale") {
        return Builtin;
    }
    return Plain;
}

} // namespace

std::vector<DslHighlightSpan> highlightDslLine(std::string_view line) {
    std::vector<DslHighlightSpan> spans;
    const auto push = [&spans](size_t offset, size_t length, DslHighlightKind kind) {
        if (length == 0) {
            return;
        }
        if (!spans.empty() && spans.back().kind == kind &&
            spans.back().offset + spans.back().length == static_cast<uint32_t>(offset)) {
            spans.back().length += static_cast<uint32_t>(length);
            return;
        }
        spans.push_back(DslHighlightSpan{static_cast<uint32_t>(offset), static_cast<uint32_t>(length), kind});
    };

    size_t i = 0;
    while (i < line.size()) {
        const unsigned char c = static_cast<unsigned char>(line[i]);
        if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') {
            push(i, line.size() - i, DslHighlightKind::Comment);
            break;
        }
        if (c == '"') {
            size_t j = i + 1;
            while (j < line.size() && line[j] != '"') {
                j += (line[j] == '\\' && j + 1 < line.size()) ? 2 : 1;
            }
            if (j < line.size()) {
                ++j; // closing quote
            }
            push(i, j - i, DslHighlightKind::String);
            i = j;
            continue;
        }
        if (std::isdigit(c) != 0 || (c == '.' && i + 1 < line.size() && std::isdigit(static_cast<unsigned char>(line[i + 1])) != 0)) {
            size_t j = i;
            while (j < line.size() &&
                   (std::isdigit(static_cast<unsigned char>(line[j])) != 0 || line[j] == '.')) {
                ++j;
            }
            if (j < line.size() && (line[j] == 's' || line[j] == 'm')) {
                ++j; // duration unit (10s, 2m; ms reaches 's' through the letter scan below)
            }
            push(i, j - i, DslHighlightKind::Number);
            i = j;
            continue;
        }
        if (std::isalpha(c) != 0 || c == '_') {
            size_t j = i;
            while (j < line.size() && isWordByte(static_cast<unsigned char>(line[j]))) {
                ++j;
            }
            push(i, j - i, classifyWord(line.substr(i, j - i)));
            i = j;
            continue;
        }
        push(i, 1, DslHighlightKind::Plain);
        ++i;
    }
    if (spans.empty() && !line.empty()) {
        push(0, line.size(), DslHighlightKind::Plain);
    }
    return spans;
}

glm::vec4 dslHighlightColor(DslHighlightKind kind) {
    switch (kind) {
    case DslHighlightKind::Keyword: return glm::vec4{0.345f, 0.612f, 0.839f, 1.0f};  // #569cd6
    case DslHighlightKind::Event: return glm::vec4{0.773f, 0.525f, 0.753f, 1.0f};    // #c586c0
    case DslHighlightKind::Builtin: return glm::vec4{0.863f, 0.863f, 0.659f, 1.0f};  // #dcdcaa
    case DslHighlightKind::Number: return glm::vec4{0.710f, 0.808f, 0.659f, 1.0f};   // #b5cea8
    case DslHighlightKind::String: return glm::vec4{0.808f, 0.569f, 0.471f, 1.0f};   // #ce9178
    case DslHighlightKind::Comment: return glm::vec4{0.416f, 0.600f, 0.333f, 1.0f};  // #6a9955
    case DslHighlightKind::Plain: break;
    }
    return glm::vec4{0.878f, 0.878f, 0.878f, 1.0f}; // theme text #e0e0e0
}

// ---------------------------------------------------------------------------
// Buffer core
// ---------------------------------------------------------------------------

void TextEditor::loadText(std::string_view text) {
    m_lines.clear();
    std::string current;
    for (const char c : text) {
        if (c == '\n') {
            m_lines.push_back(current);
            current.clear();
        } else if (c != '\r') {
            current.push_back(c);
        }
    }
    m_lines.push_back(current);
    m_cursorLine = 0;
    m_cursorCol = 0;
    m_goalColumn = 0;
    m_scrollLine = 0;
    m_selectionActive = false;
    m_dirty = false;
}

std::string TextEditor::text() const {
    std::string out;
    for (size_t i = 0; i < m_lines.size(); ++i) {
        if (i != 0) {
            out.push_back('\n');
        }
        out += m_lines[i];
    }
    return out;
}

void TextEditor::clampCursor() {
    m_cursorLine = std::min(m_cursorLine, m_lines.size() - 1);
    m_cursorCol = std::min(m_cursorCol, m_lines[m_cursorLine].size());
}

void TextEditor::setCursor(size_t line, size_t columnBytes) {
    m_cursorLine = line;
    m_cursorCol = columnBytes;
    clampCursor();
    m_goalColumn = m_cursorCol;
    m_selectionActive = false;
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

void TextEditor::beginSelection() {
    if (!m_selectionActive) {
        m_anchorLine = m_cursorLine;
        m_anchorCol = m_cursorCol;
        m_selectionActive = true;
    }
}

void TextEditor::setSelection(size_t anchorLine, size_t anchorColumn) {
    m_anchorLine = std::min(anchorLine, m_lines.size() - 1);
    m_anchorCol = std::min(anchorColumn, m_lines[m_anchorLine].size());
    m_cursorLine = m_anchorLine;
    m_cursorCol = m_anchorCol;
    m_selectionActive = true;
}

void TextEditor::selectAll() {
    m_anchorLine = 0;
    m_anchorCol = 0;
    m_cursorLine = m_lines.size() - 1;
    m_cursorCol = m_lines.back().size();
    m_goalColumn = m_cursorCol;
    m_selectionActive = true;
}

void TextEditor::selectionRange(size_t& startLine, size_t& startCol, size_t& endLine, size_t& endCol) const {
    startLine = m_anchorLine;
    startCol = m_anchorCol;
    endLine = m_cursorLine;
    endCol = m_cursorCol;
    if (startLine > endLine || (startLine == endLine && startCol > endCol)) {
        std::swap(startLine, endLine);
        std::swap(startCol, endCol);
    }
}

bool TextEditor::hasSelection() const {
    return m_selectionActive && (m_anchorLine != m_cursorLine || m_anchorCol != m_cursorCol);
}

std::string TextEditor::selectionText() const {
    if (!hasSelection()) {
        return {};
    }
    size_t sl, sc, el, ec;
    selectionRange(sl, sc, el, ec);
    if (sl == el) {
        return m_lines[sl].substr(sc, ec - sc);
    }
    std::string out = m_lines[sl].substr(sc);
    for (size_t line = sl + 1; line < el; ++line) {
        out.push_back('\n');
        out += m_lines[line];
    }
    out.push_back('\n');
    out += m_lines[el].substr(0, ec);
    return out;
}

void TextEditor::deleteSelection() {
    if (!hasSelection()) {
        m_selectionActive = false;
        return;
    }
    size_t sl, sc, el, ec;
    selectionRange(sl, sc, el, ec);
    if (sl == el) {
        m_lines[sl].erase(sc, ec - sc);
    } else {
        m_lines[sl] = m_lines[sl].substr(0, sc) + m_lines[el].substr(ec);
        m_lines.erase(m_lines.begin() + static_cast<std::ptrdiff_t>(sl + 1),
                      m_lines.begin() + static_cast<std::ptrdiff_t>(el + 1));
    }
    m_cursorLine = sl;
    m_cursorCol = sc;
    m_goalColumn = sc;
    m_selectionActive = false;
    m_dirty = true;
}

bool TextEditor::copySelection() {
    if (!hasSelection() || !m_clipboard.setText) {
        return false;
    }
    m_clipboard.setText(selectionText());
    return true;
}

bool TextEditor::cutSelection() {
    if (!copySelection()) {
        return false;
    }
    deleteSelection();
    return true;
}

bool TextEditor::pasteClipboard() {
    if (!m_clipboard.getText) {
        return false;
    }
    const std::string text = m_clipboard.getText();
    if (text.empty()) {
        return false;
    }
    insertText(text);
    return true;
}

void TextEditor::insertText(std::string_view textToInsert) {
    if (hasSelection()) {
        deleteSelection();
    }
    for (const char c : textToInsert) {
        if (c == '\n') {
            insertNewline();
        } else if (c == '\r') {
            continue;
        } else if (c == '\t') {
            insertText("  ");
        } else {
            std::string& line = m_lines[m_cursorLine];
            line.insert(m_cursorCol, 1, c);
            ++m_cursorCol;
            m_dirty = true;
        }
    }
    m_goalColumn = m_cursorCol;
}

void TextEditor::insertNewline() {
    if (hasSelection()) {
        deleteSelection();
    }
    std::string& line = m_lines[m_cursorLine];
    std::string rest = line.substr(m_cursorCol);
    line.erase(m_cursorCol);
    m_lines.insert(m_lines.begin() + static_cast<std::ptrdiff_t>(m_cursorLine + 1), rest);
    ++m_cursorLine;
    m_cursorCol = 0;
    m_goalColumn = 0;
    m_dirty = true;
}

void TextEditor::backspace() {
    if (hasSelection()) {
        deleteSelection();
        return;
    }
    if (m_cursorCol > 0) {
        const size_t prev = prevCodepoint(m_lines[m_cursorLine], m_cursorCol);
        m_lines[m_cursorLine].erase(prev, m_cursorCol - prev);
        m_cursorCol = prev;
        m_dirty = true;
    } else if (m_cursorLine > 0) {
        const size_t joinAt = m_lines[m_cursorLine - 1].size();
        m_lines[m_cursorLine - 1] += m_lines[m_cursorLine];
        m_lines.erase(m_lines.begin() + static_cast<std::ptrdiff_t>(m_cursorLine));
        --m_cursorLine;
        m_cursorCol = joinAt;
        m_dirty = true;
    }
    m_goalColumn = m_cursorCol;
}

void TextEditor::deleteForward() {
    if (hasSelection()) {
        deleteSelection();
        return;
    }
    std::string& line = m_lines[m_cursorLine];
    if (m_cursorCol < line.size()) {
        const size_t next = nextCodepoint(line, m_cursorCol);
        line.erase(m_cursorCol, next - m_cursorCol);
        m_dirty = true;
    } else if (m_cursorLine + 1 < m_lines.size()) {
        line += m_lines[m_cursorLine + 1];
        m_lines.erase(m_lines.begin() + static_cast<std::ptrdiff_t>(m_cursorLine + 1));
        m_dirty = true;
    }
    m_goalColumn = m_cursorCol;
}

void TextEditor::moveLeft(bool word, bool extend) {
    if (extend) {
        beginSelection();
    } else if (hasSelection()) {
        size_t sl, sc, el, ec;
        selectionRange(sl, sc, el, ec);
        m_cursorLine = sl;
        m_cursorCol = sc;
        m_goalColumn = sc;
        m_selectionActive = false;
        return; // collapse to the selection start, no step
    } else {
        m_selectionActive = false;
    }
    if (word) {
        const std::string& line = m_lines[m_cursorLine];
        size_t col = m_cursorCol;
        while (col > 0 && !isWordByte(static_cast<unsigned char>(line[col - 1]))) {
            col = prevCodepoint(line, col);
        }
        while (col > 0 && isWordByte(static_cast<unsigned char>(line[col - 1]))) {
            col = prevCodepoint(line, col);
        }
        m_cursorCol = col;
    } else if (m_cursorCol > 0) {
        m_cursorCol = prevCodepoint(m_lines[m_cursorLine], m_cursorCol);
    } else if (m_cursorLine > 0) {
        --m_cursorLine;
        m_cursorCol = m_lines[m_cursorLine].size();
    }
    m_goalColumn = m_cursorCol;
}

void TextEditor::moveRight(bool word, bool extend) {
    if (extend) {
        beginSelection();
    } else if (hasSelection()) {
        size_t sl, sc, el, ec;
        selectionRange(sl, sc, el, ec);
        m_cursorLine = el;
        m_cursorCol = ec;
        m_goalColumn = ec;
        m_selectionActive = false;
        return; // collapse to the selection end, no step
    } else {
        m_selectionActive = false;
    }
    const std::string& line = m_lines[m_cursorLine];
    if (word) {
        size_t col = m_cursorCol;
        while (col < line.size() && isWordByte(static_cast<unsigned char>(line[col]))) {
            col = nextCodepoint(line, col);
        }
        while (col < line.size() && !isWordByte(static_cast<unsigned char>(line[col]))) {
            col = nextCodepoint(line, col);
        }
        m_cursorCol = col;
    } else if (m_cursorCol < line.size()) {
        m_cursorCol = nextCodepoint(line, m_cursorCol);
    } else if (m_cursorLine + 1 < m_lines.size()) {
        ++m_cursorLine;
        m_cursorCol = 0;
    }
    m_goalColumn = m_cursorCol;
}

void TextEditor::moveUp(bool extend) {
    if (extend) {
        beginSelection();
    } else {
        m_selectionActive = false;
    }
    if (m_cursorLine > 0) {
        --m_cursorLine;
        m_cursorCol = std::min(m_goalColumn, m_lines[m_cursorLine].size());
    }
}

void TextEditor::moveDown(bool extend) {
    if (extend) {
        beginSelection();
    } else {
        m_selectionActive = false;
    }
    if (m_cursorLine + 1 < m_lines.size()) {
        ++m_cursorLine;
        m_cursorCol = std::min(m_goalColumn, m_lines[m_cursorLine].size());
    }
}

void TextEditor::moveHome(bool extend) {
    if (extend) {
        beginSelection();
    } else {
        m_selectionActive = false;
    }
    m_cursorCol = 0;
    m_goalColumn = 0;
}

void TextEditor::moveEnd(bool extend) {
    if (extend) {
        beginSelection();
    } else {
        m_selectionActive = false;
    }
    m_cursorCol = m_lines[m_cursorLine].size();
    m_goalColumn = m_cursorCol;
}

void TextEditor::movePage(int direction, size_t visibleRows) {
    const size_t step = std::max<size_t>(1, visibleRows);
    if (direction < 0) {
        m_cursorLine = m_cursorLine > step ? m_cursorLine - step : 0;
    } else {
        m_cursorLine = std::min(m_lines.size() - 1, m_cursorLine + step);
    }
    m_cursorCol = std::min(m_goalColumn, m_lines[m_cursorLine].size());
}

void TextEditor::ensureCaretVisible(float rowHeight, float viewHeight) {
    const size_t visible = static_cast<size_t>(std::max(1.0f, viewHeight / std::max(1.0f, rowHeight)));
    if (m_cursorLine < m_scrollLine) {
        m_scrollLine = m_cursorLine;
    } else if (m_cursorLine >= m_scrollLine + visible) {
        m_scrollLine = m_cursorLine - visible + 1;
    }
}

size_t TextEditor::columnAtX(UIBatch& batch, const std::string& lineText, float localX) const {
    if (localX <= 0.0f) {
        return 0;
    }
    size_t col = 0;
    float bestDistance = std::abs(localX);
    size_t best = 0;
    while (col < lineText.size()) {
        const size_t next = nextCodepoint(lineText, col);
        const float width = batch.measureText(std::string_view(lineText).substr(0, next), kTextScale);
        if (std::abs(localX - width) < bestDistance) {
            bestDistance = std::abs(localX - width);
            best = next;
        }
        col = next;
    }
    return best;
}

// ---------------------------------------------------------------------------
// Widget rendering + input
// ---------------------------------------------------------------------------

TextEditor::Result TextEditor::draw(UIContext& ui, const UIRect& rect, float deltaSeconds) {
    Result result;
    UIBatch* batch = ui.batch();
    if (batch == nullptr || rect.width < 40.0f || rect.height < 40.0f) {
        return result;
    }
    const UIInput& input = ui.frameInput();
    const UITheme& theme = ui.theme();

    float rowHeight = 14.5f * kTextScale;
    if (batch->font() != nullptr) {
        rowHeight = batch->font()->lineHeight(fontPixelSizeForScale(kTextScale)) * 1.22f;
    }
    const float gutterWidth = 46.0f;
    const float textX = rect.x + gutterWidth + 10.0f;
    const float textY = rect.y + 6.0f;
    const float viewHeight = rect.height - 12.0f;

    // Focus + caret placement + drag selection from the mouse:
    // press plants both anchor and caret, dragging sweeps the
    // caret while the anchor stays, release ends the drag.
    const bool pressEdge = input.mouseDown && !m_prevMouseDown;
    m_prevMouseDown = input.mouseDown;
    const auto caretFromMouse = [&]() {
        const float localY = input.mouseY - textY;
        const size_t clicked = m_scrollLine + static_cast<size_t>(std::max(0.0f, localY) / rowHeight);
        m_cursorLine = std::min(clicked, m_lines.size() - 1);
        m_cursorCol = columnAtX(*batch, m_lines[m_cursorLine], input.mouseX - textX);
        m_goalColumn = m_cursorCol;
        m_blinkSeconds = 0.0f;
    };
    if (pressEdge) {
        if (rect.contains(input.mouseX, input.mouseY)) {
            m_focused = true;
            caretFromMouse();
            m_anchorLine = m_cursorLine;
            m_anchorCol = m_cursorCol;
            m_selectionActive = true;
            m_dragging = true;
        } else {
            m_focused = false;
            m_dragging = false;
        }
    }
    if (m_dragging) {
        if (input.mouseDown) {
            caretFromMouse();
        } else {
            m_dragging = false;
        }
    }
    if (ui.hoveredRect(rect) && input.scrollY != 0.0f) {
        const long next = static_cast<long>(m_scrollLine) - static_cast<long>(input.scrollY * 3.0f);
        m_scrollLine = static_cast<size_t>(std::max(0L, next));
    }

    // Keyboard editing.
    if (m_focused) {
        m_blinkSeconds += deltaSeconds;
        const bool word = input.ctrlModifier;
        const bool extend = input.fineModifier; // Shift builds the selection
        // Key repeat: an action starts on its edge, repeats after
        // 0.38 s at ~24/s. Repeat state is per-editor, frame-driven.
        enum Action : int {
            None = 0, Left, Right, Up, Down, Back, Del, Home, End
        };
        const auto edge = [&](bool pressed, int action) {
            if (pressed) {
                m_repeatAction = action;
                m_repeatSeconds = 0.0f;
                return true;
            }
            return false;
        };
        bool fired = false;
        const auto fire = [&](int action) {
            switch (action) {
            case Left: moveLeft(word, extend); break;
            case Right: moveRight(word, extend); break;
            case Up: moveUp(extend); break;
            case Down: moveDown(extend); break;
            case Back: backspace(); result.changed = true; break;
            case Del: deleteForward(); result.changed = true; break;
            case Home: moveHome(extend); break;
            case End: moveEnd(extend); break;
            default: break;
            }
            fired = true;
            m_blinkSeconds = 0.0f;
        };
        const UIInput::EditorKeys& keys = input.editorKeys;
        if (edge(keys.left, Left)) fire(Left);
        if (edge(keys.right, Right)) fire(Right);
        if (edge(keys.up, Up)) fire(Up);
        if (edge(keys.down, Down)) fire(Down);
        if (edge(input.backspacePressed, Back)) fire(Back);
        if (edge(keys.del, Del)) fire(Del);
        if (edge(keys.home, Home)) fire(Home);
        if (edge(keys.end, End)) fire(End);
        if (keys.pageUp) {
            movePage(-1, static_cast<size_t>(viewHeight / rowHeight));
            m_blinkSeconds = 0.0f;
        }
        if (keys.pageDown) {
            movePage(+1, static_cast<size_t>(viewHeight / rowHeight));
            m_blinkSeconds = 0.0f;
        }
        if (keys.saveCombo) {
            result.saveRequested = true;
        }
        if (keys.tab) {
            insertText("  ");
            result.changed = true;
            m_blinkSeconds = 0.0f;
        }
        // Standard clipboard combos (host hooks provide the OS
        // clipboard; without hooks they are safe no-ops).
        if (keys.copyCombo) {
            copySelection();
        }
        if (keys.cutCombo && cutSelection()) {
            result.changed = true;
            m_blinkSeconds = 0.0f;
        }
        if (keys.pasteCombo && pasteClipboard()) {
            result.changed = true;
            m_blinkSeconds = 0.0f;
        }
        if (keys.selectAllCombo) {
            selectAll();
            m_blinkSeconds = 0.0f;
        }
        if (!fired && m_repeatAction != None) {
            // Held-key repeat driven by the host's level state:
            // first repeat after 0.38 s, then ~25 per second.
            const UIInput::EditorHeld& held = input.editorHeld;
            const bool stillHeld = (m_repeatAction == Left && held.left) || (m_repeatAction == Right && held.right) ||
                                   (m_repeatAction == Up && held.up) || (m_repeatAction == Down && held.down) ||
                                   (m_repeatAction == Back && held.backspace);
            if (!stillHeld) {
                m_repeatAction = None;
            } else {
                m_repeatSeconds += deltaSeconds;
                if (m_repeatSeconds > 0.38f) {
                    m_repeatSeconds = 0.34f;
                    fire(m_repeatAction);
                }
            }
        }
        if (input.enterPressed) {
            insertNewline();
            result.changed = true;
            m_repeatAction = None;
            m_blinkSeconds = 0.0f;
        }
        if (!input.textInput.empty()) {
            insertText(input.textInput);
            result.changed = true;
            m_repeatAction = None;
            m_blinkSeconds = 0.0f;
        }
    } else {
        m_repeatAction = 0;
    }
    ensureCaretVisible(rowHeight, viewHeight);
    const size_t visibleRows = static_cast<size_t>(viewHeight / rowHeight) + 1;
    m_scrollLine = std::min(m_scrollLine, m_lines.empty() ? size_t{0} : m_lines.size() - 1);

    // Paint: background, gutter, current-line wash, text, caret.
    batch->drawRect(rect.x, rect.y, rect.width, rect.height, theme.trackFill);
    batch->drawRect(rect.x, rect.y, gutterWidth, rect.height, theme.panelFill);
    batch->pushScissor(rect.x, rect.y, rect.width, rect.height);
    const glm::vec4 gutterText{0.45f, 0.48f, 0.55f, 1.0f};
    for (size_t row = 0; row < visibleRows; ++row) {
        const size_t lineIndex = m_scrollLine + row;
        if (lineIndex >= m_lines.size()) {
            break;
        }
        const float y = textY + static_cast<float>(row) * rowHeight;
        if (lineIndex == m_cursorLine && m_focused) {
            batch->drawRect(textX - 4.0f, y - 1.0f, rect.x + rect.width - textX + 4.0f, rowHeight,
                            glm::vec4{1.0f, 1.0f, 1.0f, 0.045f});
        }
        // Selection background under this line's covered range.
        if (hasSelection()) {
            size_t selStartLine, selStartCol, selEndLine, selEndCol;
            selectionRange(selStartLine, selStartCol, selEndLine, selEndCol);
            if (lineIndex >= selStartLine && lineIndex <= selEndLine) {
                const size_t from = lineIndex == selStartLine ? selStartCol : 0;
                const size_t to = lineIndex == selEndLine ? selEndCol : m_lines[lineIndex].size();
                const std::string& text = m_lines[lineIndex];
                const float x0 = textX + batch->measureText(std::string_view(text).substr(0, from), kTextScale);
                const float x1 = textX + batch->measureText(std::string_view(text).substr(0, to), kTextScale);
                batch->drawRect(x0, y - 1.0f, std::max(3.0f, x1 - x0), rowHeight,
                                glm::vec4{0.0f, 0.478f, 0.800f, 0.38f});
            }
        }
        const std::string number = std::to_string(lineIndex + 1);
        const float numberWidth = batch->measureText(number, kTextScale);
        batch->drawText(number, rect.x + gutterWidth - 8.0f - numberWidth, y, kTextScale, gutterText);
        const std::string& lineText = m_lines[lineIndex];
        float x = textX;
        for (const DslHighlightSpan& span : highlightDslLine(lineText)) {
            const std::string_view segment =
                std::string_view(lineText).substr(span.offset, span.length);
            batch->drawText(segment, x, y, kTextScale, dslHighlightColor(span.kind));
            x += batch->measureText(segment, kTextScale);
        }
        if (lineIndex == m_cursorLine && m_focused && std::fmod(m_blinkSeconds, 1.1f) < 0.62f) {
            const float caretX = textX + batch->measureText(std::string_view(lineText).substr(0, m_cursorCol), kTextScale);
            batch->drawRect(caretX, y - 1.0f, 2.0f, rowHeight, theme.focusBorder);
        }
    }
    batch->popScissor();
    // Border (accent while focused).
    const glm::vec4 border = m_focused ? theme.focusBorder : theme.panelBorder;
    batch->drawRect(rect.x, rect.y, rect.width, 1.0f, border);
    batch->drawRect(rect.x, rect.y + rect.height - 1.0f, rect.width, 1.0f, border);
    batch->drawRect(rect.x, rect.y, 1.0f, rect.height, border);
    batch->drawRect(rect.x + rect.width - 1.0f, rect.y, 1.0f, rect.height, border);
    return result;
}

} // namespace engine
