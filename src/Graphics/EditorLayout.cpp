#include "Graphics/EditorLayout.h"

#include <algorithm>

namespace engine {

EditorLayout computeEditorLayout(float framebufferWidth, float framebufferHeight, bool playing) {
    return computeEditorLayout(framebufferWidth, framebufferHeight, playing, EditorDockSizes{});
}

EditorLayout computeEditorLayout(float framebufferWidth, float framebufferHeight, bool playing,
                                 const EditorDockSizes& dockSizes) {
    EditorLayout layout;
    layout.playing = playing;
    if (framebufferWidth <= 0.0f || framebufferHeight <= 0.0f) {
        return layout;
    }

    // Scale UI chrome gently with the framebuffer: at the 1280x720
    // reference the factor is 1; small windows shrink towards 0.75 so
    // the docks still fit, large/HiDPI framebuffers grow to at most
    // 1.35 so text and hit targets track the display instead of
    // staying windowed-in-2010 sized.
    layout.uiScale = std::clamp(std::min(framebufferWidth / 1280.0f, framebufferHeight / 720.0f),
                                0.75f, 1.35f);
    const float scale = layout.uiScale;

    // Flush, edge-to-edge docking (no gutters): every dock touches
    // its neighbours and the window edges exactly, so the viewport
    // reads as one clean surface cut out of a single workspace --
    // no black seams, no floating frames. Separation between docks
    // comes from their own fills, not from background gaps, and the
    // viewport reclaims every pixel the old margins wasted.
    const float toolbarHeight = std::clamp(52.0f * scale, 40.0f, 64.0f);
    const float statusHeight = std::clamp(28.0f * scale, 22.0f, 36.0f);
    layout.toolbar = UIRect{0.0f, 0.0f, framebufferWidth, toolbarHeight};
    layout.statusBar = UIRect{0.0f, framebufferHeight - statusHeight, framebufferWidth, statusHeight};

    const float middleTop = toolbarHeight;
    const float middleBottom = framebufferHeight - statusHeight;
    const float middleHeight = std::max(0.0f, middleBottom - middleTop);

    if (playing) {
        // Play: no authoring docks. The viewport owns everything
        // between toolbar and status bar; the toolbar keeps Stop and
        // the status bar keeps the "click viewport / Esc" hints.
        layout.viewport = UIRect{0.0f, middleTop, framebufferWidth, middleHeight};
        return layout;
    }

    // Auto sizes are the classic fractions; explicit (splitter-
    // dragged) sizes get wider, still-sane ranges. Either way the
    // minimum-viewport guarantee below has the final word.
    float leftWidth = dockSizes.leftWidth > 0.0f
                          ? std::clamp(dockSizes.leftWidth, 120.0f * scale, 560.0f * scale)
                          : std::clamp(framebufferWidth * 0.19f, 168.0f * scale, 300.0f * scale);
    float rightWidth = dockSizes.rightWidth > 0.0f
                           ? std::clamp(dockSizes.rightWidth, 140.0f * scale, 640.0f * scale)
                           : std::clamp(framebufferWidth * 0.21f, 184.0f * scale, 340.0f * scale);
    float assetsHeight = dockSizes.bottomHeight > 0.0f
                             ? std::clamp(dockSizes.bottomHeight, 72.0f * scale, 520.0f * scale)
                             : std::clamp(framebufferHeight * 0.24f, 104.0f * scale, 208.0f * scale);

    // Guarantee a usable viewport on cramped windows by shrinking the
    // docks (never the other way round): keep at least ~220x130 px of
    // world visible, or whatever the window can offer.
    const float minViewportWidth = std::min(220.0f, framebufferWidth * 0.35f);
    const float minViewportHeight = std::min(130.0f, middleHeight * 0.45f);
    const float maxDocksWidth = framebufferWidth - minViewportWidth;
    if (leftWidth + rightWidth > maxDocksWidth && maxDocksWidth > 0.0f) {
        const float shrink = maxDocksWidth / (leftWidth + rightWidth);
        leftWidth *= shrink;
        rightWidth *= shrink;
    }
    const float maxAssetsHeight = middleHeight - minViewportHeight;
    assetsHeight = std::min(assetsHeight, std::max(0.0f, maxAssetsHeight));

    layout.hierarchy = UIRect{0.0f, middleTop, leftWidth, middleHeight};
    layout.inspector = UIRect{framebufferWidth - rightWidth, middleTop, rightWidth, middleHeight};

    const float centreX = leftWidth;
    const float centreWidth = std::max(0.0f, framebufferWidth - rightWidth - centreX);
    const float assetsTop = middleBottom - assetsHeight;
    layout.assetBrowser = UIRect{centreX, assetsTop, centreWidth, assetsHeight};
    layout.viewport = UIRect{centreX, middleTop, centreWidth,
                             std::max(0.0f, assetsTop - middleTop)};
    return layout;
}

} // namespace engine
