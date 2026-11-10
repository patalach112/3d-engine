#pragma once
#include "Core/Export.h"

#include "Graphics/UIContext.h"

namespace engine {

// ---------------------------------------------------------------------------
// EditorLayout
//
// Pure-CPU dock geometry for the editor shell. The OS window is divided
// exactly once, here, into a top toolbar, a bottom status bar and -- in
// Edit mode -- Hierarchy (left), Inspector (right) and Asset Browser
// (bottom-centre) docks around a single bounded 3D viewport. Nothing
// else in the editor may invent its own full-window rectangles: the
// scene pass, camera projection, picking rays, gizmo projection and
// mouse-capture decisions all consume `viewport` from this struct, so
// rendering and input can never disagree about where the world lives.
//
// The layout is a function of the *current* framebuffer size only, so
// resizing (or moving between monitors with different DPI/content
// scales, which changes the framebuffer size for the same logical
// window) reflows every dock on the next frame. In Play mode the
// authoring docks collapse and the viewport expands between the same
// toolbar/status bars, which keep Stop reachable with a free cursor.
// ---------------------------------------------------------------------------
struct EditorLayout {
    UIRect toolbar{};
    UIRect hierarchy{};
    UIRect inspector{};
    UIRect assetBrowser{};
    UIRect viewport{};
    UIRect statusBar{};
    float uiScale = 1.0f; // framebuffer pixels per reference UI pixel
    bool playing = false;

    [[nodiscard]] bool containsViewport(float x, float y) const {
        return viewport.contains(x, y);
    }
    [[nodiscard]] float viewportAspect() const {
        return viewport.height > 0.0f ? viewport.width / viewport.height : 1.0f;
    }
    // True when the rectangle named by `rect` and the viewport share no
    // pixels: docked panels must satisfy this for the scene viewport.
    [[nodiscard]] bool viewportIsDockBounded() const {
        const auto overlaps = [](const UIRect& a, const UIRect& b) {
            return a.width > 0.0f && a.height > 0.0f && b.width > 0.0f && b.height > 0.0f &&
                   a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height &&
                   b.y < a.y + a.height;
        };
        return !overlaps(viewport, hierarchy) && !overlaps(viewport, inspector) &&
               !overlaps(viewport, assetBrowser) && !overlaps(viewport, toolbar) &&
               !overlaps(viewport, statusBar);
    }
};

// Computes the dock layout for a framebuffer of the given size (pixels,
// top-left origin). Degenerate sizes yield a zero viewport rather than
// negative rectangles.
[[nodiscard]] ENGINE_GRAPHICS_API EditorLayout computeEditorLayout(float framebufferWidth,
                                                                   float framebufferHeight,
                                                                   bool playing);

} // namespace engine
