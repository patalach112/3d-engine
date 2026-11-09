#pragma once
#include "Core/Export.h"

#include <glm/vec2.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace engine {

class Window;

// Engine-level key codes. The rest of the engine only ever sees these;
// translation from GLFW codes happens inside Input (Platform layer), so
// no GLFW constant leaks into Core/Graphics code.
enum class Key : uint8_t {
    Unknown = 0,
    A, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    Space,
    Escape,
    Enter,
    Tab,
    Backspace,
    LeftShift,
    RightShift,
    LeftControl,
    RightControl,
    LeftAlt,
    RightAlt,
    Up,
    Down,
    Left,
    Right,
    Count
};

enum class MouseButton : uint8_t {
    Left = 0,
    Right,
    Middle,
    Count
};

// Input tuning that does not belong to a single camera (see [Input] in
// config/engine.ini).
struct InputConfig {
    // Grab and hide the cursor at startup (first-person mouse look).
    // Afterwards: click the window to capture, Esc to release.
    bool captureMouseOnStart = true;
    bool captureMouseOnClick = true;
};

// ---------------------------------------------------------------------------
// Input
//
// Event-driven input state for one window. GLFW callbacks arrive through
// the Window (which owns the GLFW window and forwards events here); this
// class translates them into engine-level state that any system can poll:
//
//   * held state          — isKeyDown() / isMouseButtonDown()
//   * per-frame transitions — wasKeyPressed() / wasKeyReleased() / ...
//   * mouse motion          — mousePosition(), mouseDelta(), scrollDelta()
//   * named mappings        — bindAction()/bindAxis() map physical keys
//     and buttons to gameplay intents ("Jump", "MoveForward"), so
//     gameplay code never hardcodes raw key codes (Phase 17).
//
// Frame protocol (driven by the Application's event-polling step):
//   1. beginFrame() clears per-frame data (transitions, deltas),
//   2. Window::pollEvents() lets GLFW fire callbacks into this object,
//   3. engine systems query state during update/render.
//
// The default constructor creates a headless instance with no window:
// no GLFW events ever arrive, but the full state machine — including
// mappings — can be driven through the inject*() entry points. That
// is how unit tests, replays and tooling exercise input.
// ---------------------------------------------------------------------------
class ENGINE_PLATFORM_API Input {
public:
    explicit Input(Window& window);
    Input(); // headless: no window, state driven via inject*()
    ~Input();

    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;
    Input(Input&&) = delete;
    Input& operator=(Input&&) = delete;

    // Clears per-frame transitions and motion deltas. Call immediately
    // before Window::pollEvents() each frame.
    void beginFrame();

    // Clears all held/transitional state (e.g. after focus loss, so keys
    // don't appear stuck when the window regains focus).
    void resetState();

    [[nodiscard]] bool isKeyDown(Key key) const;
    [[nodiscard]] bool wasKeyPressed(Key key) const;
    [[nodiscard]] bool wasKeyReleased(Key key) const;

    [[nodiscard]] bool isMouseButtonDown(MouseButton button) const;
    [[nodiscard]] bool wasMouseButtonPressed(MouseButton button) const;
    [[nodiscard]] bool wasMouseButtonReleased(MouseButton button) const;

    // Absolute cursor position in pixels (window coordinates).
    [[nodiscard]] glm::vec2 mousePosition() const { return m_mousePosition; }
    // Cursor movement accumulated during this frame's event polling, in
    // pixels. Positive Y means the cursor moved down.
    [[nodiscard]] glm::vec2 mouseDelta() const { return m_mouseDelta; }
    // Scroll wheel movement accumulated this frame.
    [[nodiscard]] glm::vec2 scrollDelta() const { return m_scrollDelta; }

    // Cursor capture hides the cursor and reports unbounded movement —
    // what first-person mouse look needs.
    void setMouseCaptured(bool captured);
    [[nodiscard]] bool isMouseCaptured() const { return m_mouseCaptured; }

    // --- Named action/axis mappings -------------------------------------
    // An action collects any number of keys and mouse buttons; it is
    // down while ANY binding is down, and its pressed/released edges
    // fire when any binding produces that edge this frame. An axis
    // reads -1 (negative key), +1 (positive key), or 0 (neither or
    // both). Rebinding a name replaces the previous mapping of that
    // kind; unbound names read as up/0.
    void bindAction(std::string_view name, Key key);
    void bindActionButton(std::string_view name, MouseButton button);
    void clearActionBindings(std::string_view name);
    [[nodiscard]] bool isActionDown(std::string_view name) const;
    [[nodiscard]] bool wasActionPressed(std::string_view name) const;
    [[nodiscard]] bool wasActionReleased(std::string_view name) const;

    void bindAxis(std::string_view name, Key negative, Key positive);
    void clearAxisBinding(std::string_view name);
    [[nodiscard]] float axisValue(std::string_view name) const;

    // --- Event entry points (called by Window's GLFW callbacks) -----------
    void onKeyEvent(int glfwKey, int glfwAction);
    void onMouseButtonEvent(int glfwButton, int glfwAction);
    void onCursorPositionEvent(double xPosition, double yPosition);
    void onScrollEvent(double xOffset, double yOffset);
    void onFocusChanged(bool focused);

    // --- Engine-level event injection (tests, replays, tooling) ---------
    // The same state transitions the GLFW callbacks feed, expressed in
    // engine key/button codes. A repeated "down" while already down is
    // not a new press edge, mirroring GLFW_REPEAT handling.
    void injectKeyState(Key key, bool down);
    void injectMouseButtonState(MouseButton button, bool down);
    void injectCursorPosition(glm::vec2 position);
    void injectScrollDelta(glm::vec2 delta);

private:
    static constexpr size_t kKeyCount = static_cast<size_t>(Key::Count);
    static constexpr size_t kButtonCount = static_cast<size_t>(MouseButton::Count);

    struct ActionBinding {
        std::vector<Key> keys;
        std::vector<MouseButton> buttons;
    };
    struct AxisBinding {
        Key negative = Key::Unknown;
        Key positive = Key::Unknown;
    };

    Window* m_window = nullptr;

    std::array<bool, kKeyCount> m_keysDown{};
    std::array<bool, kKeyCount> m_keysPressed{};
    std::array<bool, kKeyCount> m_keysReleased{};

    std::array<bool, kButtonCount> m_buttonsDown{};
    std::array<bool, kButtonCount> m_buttonsPressed{};
    std::array<bool, kButtonCount> m_buttonsReleased{};

    glm::vec2 m_mousePosition{0.0f};
    glm::vec2 m_mouseDelta{0.0f};
    glm::vec2 m_scrollDelta{0.0f};
    bool m_hasMousePosition = false;
    bool m_mouseCaptured = false;

    std::unordered_map<std::string, ActionBinding> m_actions;
    std::unordered_map<std::string, AxisBinding> m_axes;
};

} // namespace engine
