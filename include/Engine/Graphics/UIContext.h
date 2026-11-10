#pragma once
#include "Core/Export.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Graphics/UIRenderer.h"

namespace engine {

// ---------------------------------------------------------------------------
// UIContext (Phase 26 Step 3): immediate-mode widget state on top of
// UIBatch. The caller feeds one UIInput per frame, calls widgets
// (each appends its own SDF geometry to the batch and returns its
// interaction result), and the context carries hover/active/focus
// identity across frames exactly like Dear ImGui:
//
//   hovered — widget under the mouse this frame (respects the
//             batch's current scissor: clipped-away pixels neither
//             hover nor press).
//   active  — widget that captured the current mouse press; it
//             keeps the drag even off its rect until release.
//   focused — last pressed widget (pressing elsewhere steals it,
//             pressing empty space clears it); drives the focus
//             outline and keyboard stepping.
//
// Widget identity is an FNV-1a hash of (panel seed, label), so
// labels must be unique among siblings. Widgets exist in two
// forms: layout-driven (rect taken from the enclosing panel's row
// cursor) and rect-driven (explicit rect), sharing one core.
// ---------------------------------------------------------------------------

struct UIRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    bool operator==(const UIRect&) const = default;
    [[nodiscard]] bool contains(float px, float py) const {
        return px >= x && px < x + width && py >= y && py < y + height;
    }
};

// One frame of host input, in UI pixels (window coordinates).
// A mouse position far off-screen means "no pointer" (e.g. while
// the mouse is captured for first-person flight).
// Payload carried by an in-flight drag (Phase 29). `type` is the
// contract drop targets match on (e.g. "AssetMesh"); `data` is the
// opaque payload (an asset path); `label` captions the drag ghost.
struct UIDragPayload {
    std::string type;
    std::string data;
    std::string label;
};

struct UIInput {
    float mouseX = -10000.0f;
    float mouseY = -10000.0f;
    bool mouseDown = false; // left button held
    int keyStep = 0;        // -1/0/+1 step input for the focused widget (arrow keys)
    bool mouseRightDown = false; // right button held (Phase 27: context menus)
    bool escapePressed = false;  // Esc edge this frame (dismisses popups)
    float scrollY = 0.0f;        // wheel delta in lines, positive = up
    // Fine-adjust modifier (Shift) held this frame: slider drags
    // become slow relative drags and focused key steps shrink 20x,
    // so wide-range fields (a +/-3600-degree rotation) stay
    // precisely editable. Defaulted off: the base absolute-drag /
    // 1%-step contract is unchanged without it.
    bool fineModifier = false;
    // Text typed this frame (UTF-8 codepoints from the platform
    // char callback), consumed by a focused textField. Empty in
    // tests unless a test types something.
    std::string textInput{};
    bool backspacePressed = false; // one Backspace press this frame (textField)
    bool enterPressed = false;     // one Enter press this frame (commits a textField)
};

using UIWidgetId = uint32_t;
inline constexpr UIWidgetId kUINoWidget = 0;

struct UITheme {
    // True deep-dark theme (professional charcoal surfaces, one
    // blue accent for interactive fills, one orange accent for
    // focus/active states). Every neutral surface sits in the
    // #121212-#252526 band: nothing grey may glare against the
    // panels -- the old #3e3e42 hovers and near-white slider thumb
    // read as light grey next to the docks. Docked editor panels
    // are fully opaque: the 3D viewport is a bounded rectangle
    // between them, so nothing of the world may bleed through the
    // chrome (the old 0.92 alpha let scene colour wash over text
    // whenever geometry passed behind a panel).
    glm::vec4 panelFill{0.145f, 0.145f, 0.149f, 1.0f};      // #252526 panels
    glm::vec4 toolbarFill{0.125f, 0.125f, 0.125f, 1.0f};    // #202020 toolbars/status bar
    glm::vec4 backgroundFill{0.070f, 0.070f, 0.070f, 1.0f}; // #121212 deepest chrome base
    glm::vec4 panelBorder{0.200f, 0.200f, 0.200f, 1.0f};    // #333333
    glm::vec4 text{0.93f, 0.93f, 0.93f, 1.0f};              // near-white, high contrast
    glm::vec4 buttonFill{0.145f, 0.145f, 0.149f, 1.0f};     // #252526, reads on #202020 bars
    glm::vec4 buttonHovered{0.200f, 0.200f, 0.200f, 1.0f};  // #333333 lift
    glm::vec4 buttonActive{0.055f, 0.388f, 0.612f, 1.0f};   // #0e639c
    glm::vec4 trackFill{0.094f, 0.094f, 0.094f, 1.0f};      // #181818 fields/tracks
    glm::vec4 sliderFill{0.0f, 0.478f, 0.800f, 1.0f};       // #007acc
    glm::vec4 thumbFill{0.55f, 0.55f, 0.56f, 1.0f};         // mid grey, visible without glare
    glm::vec4 checkFill{0.0f, 0.478f, 0.800f, 1.0f};        // #007acc
    glm::vec4 focusBorder{1.0f, 0.60f, 0.10f, 1.0f};        // orange accent
};

class ENGINE_GRAPHICS_API UIContext {
public:
    // Layout geometry (pixels). Public so hosts/tests can predict
    // widget placement exactly.
    static constexpr float kRowHeight = 26.0f;
    static constexpr float kRowSpacing = 4.0f;
    static constexpr float kPanelPadding = 8.0f;
    static constexpr float kTextScale = 1.5f;
    static constexpr float kSliderLabelFraction = 0.38f;

    // Frame lifecycle: beginFrame snapshots input (computing
    // press/release edges), endFrame finalizes focus (an unclaimed
    // press clears it). Widgets may only be called in between.
    void beginFrame(const UIInput& input, UIBatch& batch);
    void endFrame();

    // Panels draw their background, push a matching scissor onto
    // the batch, and host a vertical row cursor for layout widgets.
    // A default-constructed style yields the rounded, bordered
    // house look; passing a negative cornerRadius/borderWidth is an
    // explicit opt-out (square, borderless) for panels that tile
    // flush against their neighbours, like the editor docks.
    void beginPanel(std::string_view id, const UIRect& rect, const UIRectStyle& style = {});
    void endPanel();

    // Non-interactive text row (also used for inspector readouts).
    void label(std::string_view text);
    void label(const UIRect& rect, std::string_view text);

    // True on the frame a click completes (press + release on the
    // widget). style supplies corner radius/border preferences;
    // state colors come from the theme.
    bool button(std::string_view label, const UIRectStyle& style = {});
    bool button(const UIRect& rect, std::string_view label, const UIRectStyle& style = {});
    // True when the value changed this frame (press, drag, or a
    // keyboard step while focused). Dragging captures the mouse
    // until release; keyboard steps move 1% of the range.
    bool slider(std::string_view label, float* value, float min, float max);
    bool slider(const UIRect& rect, std::string_view label, float* value, float min, float max);
    // True on the frame the box toggles.
    bool checkbox(std::string_view label, bool* value);
    bool checkbox(const UIRect& rect, std::string_view label, bool* value);
    // Three slider rows (label.x/.y/.z) with per-axis accents.
    bool vec3Sliders(std::string_view label, glm::vec3* value, float min, float max);

    // Single-line text entry. While the field holds keyboard focus
    // (click it, or pass grabFocus on the frame it appears) typed
    // characters append, Backspace deletes the last codepoint,
    // Enter commits — the one frame this returns true — and Esc
    // restores the text the field had when focus was gained. The
    // label seeds the widget id only; hosts draw their own caption.
    bool textField(std::string_view label, std::string* text, bool grabFocus = false);
    bool textField(const UIRect& rect, std::string_view label, std::string* text, bool grabFocus = false);

    // Shared scroll clamp for wheel-scrolled panels (inspector,
    // hierarchy): keeps the offset inside [0, content - view].
    [[nodiscard]] static float clampScrollOffset(float offset, float contentHeight, float viewHeight);

    // --- Context menus / popups (Phase 27) -------------------------------
    // One popup is active at a time (opening replaces). The host
    // typically opens it from a right-click and renders it LAST in
    // the frame so it paints on top:
    //   if (ctx.rightPressedRect(row)) ctx.openPopup("id", mx, my);
    //   if (ctx.beginPopup("id")) { if (ctx.menuItem("Delete")) ...; ctx.endPopup(); }
    // Dismissal is automatic: Esc closes, and a left press outside
    // the popup closes it AND consumes that press (widgets beneath
    // never see it). menuItem fires on the press edge and closes
    // the popup; disabled items render dim and never fire.
    static constexpr float kPopupWidth = 184.0f;
    static constexpr float kPopupItemHeight = 20.0f;
    static constexpr float kPopupPadding = 4.0f;

    void openPopup(std::string_view id, float x, float y);
    void closePopup();
    [[nodiscard]] bool isPopupOpen(std::string_view id) const;
    [[nodiscard]] bool popupOpen() const { return m_popup.open; }
    bool beginPopup(std::string_view id);
    bool menuItem(std::string_view label, bool enabled = true);
    void menuSeparator();
    void endPopup();

    // --- Drag & drop (Phase 29) ------------------------------------------
    // Immediate-mode drag sources and drop targets:
    //   if (ctx.beginDragSource(itemRect, "AssetMesh", path, name)) ...
    //   if (ctx.dropTarget(viewportRect, "AssetMesh")) spawn(...);
    // A press on a source arms a *pending* drag (so plain clicks still
    // work); moving kDragThresholdPx while held promotes it to a real
    // drag with a ghost following the cursor. dropTarget returns true
    // exactly once, on the release frame, when the pointer is over a
    // rect whose expected type matches the payload; the first target
    // to claim a drop consumes it. Esc cancels mid-drag (no target
    // will fire). Call drop targets after all sources each frame.
    static constexpr float kDragThresholdPx = 6.0f;

    bool beginDragSource(const UIRect& rect, std::string_view type, std::string_view data,
                         std::string_view label);
    [[nodiscard]] bool dragActive() const { return m_dragPhase == DragPhase::Dragging; }
    [[nodiscard]] const UIDragPayload& dragPayload() const { return m_dragPayload; }
    bool dropTarget(const UIRect& rect, std::string_view expectedType);
    [[nodiscard]] bool dropTargetHovered(const UIRect& rect, std::string_view expectedType) const;

    // --- Hit queries for custom widgets (outliner rows, ...) --------------
    // All respect the current scissor + popup press consumption.
    [[nodiscard]] bool hoveredRect(const UIRect& rect) const { return hitTest(rect); }
    [[nodiscard]] bool pressedRect(const UIRect& rect);
    [[nodiscard]] bool rightPressedRect(const UIRect& rect);
    [[nodiscard]] glm::vec2 mousePosition() const { return {m_input.mouseX, m_input.mouseY}; }
    // Raw left-button state for drag-style tools (Phase 28 gizmos)
    // that track their own capture instead of the widget active id.
    [[nodiscard]] bool mouseDown() const { return m_input.mouseDown; }
    [[nodiscard]] float scrollDelta() const { return m_input.scrollY; }

    [[nodiscard]] UIWidgetId hoveredId() const { return m_hovered; }
    [[nodiscard]] UIWidgetId activeId() const { return m_active; }
    [[nodiscard]] UIWidgetId focusedId() const { return m_focused; }
    [[nodiscard]] const UITheme& theme() const { return m_theme; }
    void setTheme(const UITheme& theme) { m_theme = theme; }
    [[nodiscard]] UIBatch* batch() const { return m_batch; }

    // FNV-1a over (seed, label); never returns kUINoWidget.
    [[nodiscard]] static UIWidgetId hashId(UIWidgetId seed, std::string_view label);
    // The slider track inside a row rect (label column on the left).
    [[nodiscard]] static UIRect sliderTrackRect(const UIRect& rowRect);

private:
    struct Panel {
        UIRect rect;
        float cursorY = 0.0f;
        UIWidgetId seed = 0;
    };

    [[nodiscard]] UIRect nextRow();
    [[nodiscard]] bool hitTest(const UIRect& rect) const;
    [[nodiscard]] float textTop(const UIRect& rect) const;
    bool sliderRow(const UIRect& rect, std::string_view label, UIWidgetId id, float* value, float min,
                   float max, const glm::vec4& accent);

    struct PopupState {
        UIWidgetId id = kUINoWidget;
        float x = 0.0f;
        float y = 0.0f;
        float height = 28.0f; // content height from the last render
        float cursorY = 0.0f;
        bool open = false;
    };

    UIBatch* m_batch = nullptr;
    UITheme m_theme;
    UIInput m_input{};
    float m_prevMouseX = -10000.0f; // last frame's pointer x (fine slider drags)
    UIWidgetId m_textEditId = kUINoWidget; // textField currently being edited
    std::string m_textSnapshot;            // its content when focus was gained (Esc restores)
    bool m_prevMouseDown = false;
    bool m_prevRightDown = false;
    bool m_pressedEdge = false;
    bool m_releasedEdge = false;
    bool m_rightPressedEdge = false;
    bool m_pressClaimed = false;
    bool m_pressConsumed = false; // a popup dismissal ate this frame's press
    UIWidgetId m_hovered = kUINoWidget;
    UIWidgetId m_active = kUINoWidget;
    UIWidgetId m_focused = kUINoWidget;
    PopupState m_popup;
    bool m_popupRendering = false;

    enum class DragPhase : uint8_t { Idle, Pending, Dragging };
    DragPhase m_dragPhase = DragPhase::Idle;
    UIDragPayload m_dragPayload;
    glm::vec2 m_dragStart{0.0f};
    bool m_dragDelivered = false; // a target claimed this drop already
    bool m_dragCancelled = false; // Esc mid-drag: released nowhere

    std::vector<Panel> m_panels; // [0] is the implicit full-viewport root
};

} // namespace engine
