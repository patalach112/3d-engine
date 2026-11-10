#pragma once
#include "Core/Export.h"

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
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
struct UIInput {
    float mouseX = -10000.0f;
    float mouseY = -10000.0f;
    bool mouseDown = false; // left button held
    int keyStep = 0;        // -1/0/+1 step input for the focused widget (arrow keys)
};

using UIWidgetId = uint32_t;
inline constexpr UIWidgetId kUINoWidget = 0;

struct UITheme {
    glm::vec4 panelFill{0.05f, 0.06f, 0.09f, 0.92f};
    glm::vec4 panelBorder{1.0f, 1.0f, 1.0f, 0.14f};
    glm::vec4 text{0.92f, 0.95f, 1.0f, 1.0f};
    glm::vec4 buttonFill{0.16f, 0.19f, 0.26f, 1.0f};
    glm::vec4 buttonHovered{0.23f, 0.28f, 0.38f, 1.0f};
    glm::vec4 buttonActive{0.12f, 0.38f, 0.68f, 1.0f};
    glm::vec4 trackFill{0.10f, 0.11f, 0.15f, 1.0f};
    glm::vec4 sliderFill{0.35f, 0.65f, 1.0f, 1.0f};
    glm::vec4 thumbFill{0.80f, 0.86f, 0.95f, 1.0f};
    glm::vec4 checkFill{0.35f, 0.65f, 1.0f, 1.0f};
    glm::vec4 focusBorder{1.0f, 0.80f, 0.30f, 1.0f};
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

    UIBatch* m_batch = nullptr;
    UITheme m_theme;
    UIInput m_input{};
    bool m_prevMouseDown = false;
    bool m_pressedEdge = false;
    bool m_releasedEdge = false;
    bool m_pressClaimed = false;
    UIWidgetId m_hovered = kUINoWidget;
    UIWidgetId m_active = kUINoWidget;
    UIWidgetId m_focused = kUINoWidget;
    std::vector<Panel> m_panels; // [0] is the implicit full-viewport root
};

} // namespace engine
