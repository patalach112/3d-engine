#include "Graphics/UIContext.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace engine {
namespace {

constexpr UIWidgetId kRootSeed = 2166136261u; // FNV-1a offset basis
constexpr float kGlyphBoxHeight = 7.0f;       // font pixels (5x7 font + halo handled by the atlas cell)

} // namespace

UIWidgetId UIContext::hashId(UIWidgetId seed, std::string_view label) {
    uint32_t hash = seed;
    for (const char character : label) {
        hash ^= static_cast<uint32_t>(static_cast<uint8_t>(character));
        hash *= 16777619u; // FNV prime
    }
    return hash == kUINoWidget ? 1u : hash;
}

UIRect UIContext::sliderTrackRect(const UIRect& rowRect) {
    const float labelWidth = rowRect.width * kSliderLabelFraction;
    const float x = rowRect.x + labelWidth + 8.0f;
    const float width = std::max(0.0f, rowRect.width - labelWidth - 12.0f);
    return UIRect{x, rowRect.y + 0.5f * rowRect.height - 3.0f, width, 6.0f};
}

void UIContext::beginFrame(const UIInput& input, UIBatch& batch) {
    m_batch = &batch;
    m_input = input;
    m_pressedEdge = input.mouseDown && !m_prevMouseDown;
    m_releasedEdge = !input.mouseDown && m_prevMouseDown;
    m_hovered = kUINoWidget;
    m_pressClaimed = false;

    m_panels.clear();
    Panel root;
    root.rect = UIRect{0.0f, 0.0f, static_cast<float>(batch.viewportWidth()),
                       static_cast<float>(batch.viewportHeight())};
    root.cursorY = kPanelPadding;
    root.seed = kRootSeed;
    m_panels.push_back(root);
}

void UIContext::endFrame() {
    // A press nobody claimed lands on empty space: focus is lost.
    if (m_pressedEdge && !m_pressClaimed) {
        m_focused = kUINoWidget;
    }
    // Safety: an active widget that stops being drawn mid-drag (or
    // a missed release edge) must not capture the mouse forever.
    if (!m_input.mouseDown) {
        m_active = kUINoWidget;
    }
    m_prevMouseDown = m_input.mouseDown;
}

void UIContext::beginPanel(std::string_view id, const UIRect& rect, const UIRectStyle& style) {
    if (m_batch == nullptr) {
        return;
    }
    UIRectStyle effective = style;
    if (effective.cornerRadius <= 0.0f) {
        effective.cornerRadius = 6.0f;
    }
    if (effective.borderWidth <= 0.0f) {
        effective.borderWidth = 1.0f;
        effective.borderColor = m_theme.panelBorder;
    }
    m_batch->drawRoundedRect(rect.x, rect.y, rect.width, rect.height, effective, m_theme.panelFill);
    m_batch->pushScissor(rect.x, rect.y, rect.width, rect.height);

    Panel panel;
    panel.rect = rect;
    panel.cursorY = rect.y + kPanelPadding;
    panel.seed = hashId(m_panels.empty() ? kRootSeed : m_panels.back().seed, id);
    m_panels.push_back(panel);
}

void UIContext::endPanel() {
    if (m_batch != nullptr) {
        m_batch->popScissor();
    }
    if (m_panels.size() > 1) {
        m_panels.pop_back();
    }
}

void UIContext::label(std::string_view text) {
    label(nextRow(), text);
}

void UIContext::label(const UIRect& rect, std::string_view text) {
    if (m_batch == nullptr) {
        return;
    }
    m_batch->drawText(text, rect.x + 2.0f, textTop(rect), kTextScale, m_theme.text);
}

UIRect UIContext::nextRow() {
    if (m_panels.empty()) {
        return UIRect{};
    }
    Panel& panel = m_panels.back();
    const UIRect row{panel.rect.x + kPanelPadding, panel.cursorY,
                     std::max(0.0f, panel.rect.width - 2.0f * kPanelPadding), kRowHeight};
    panel.cursorY += kRowHeight + kRowSpacing;
    return row;
}

bool UIContext::hitTest(const UIRect& rect) const {
    if (m_batch == nullptr || !rect.contains(m_input.mouseX, m_input.mouseY)) {
        return false;
    }
    // Clipped pixels are dead pixels: the point must also be inside
    // the batch's effective (nested) scissor.
    const UIScissorRect clip = m_batch->currentScissor();
    return m_input.mouseX >= clip.x && m_input.mouseX < clip.x + clip.width && m_input.mouseY >= clip.y &&
           m_input.mouseY < clip.y + clip.height;
}

float UIContext::textTop(const UIRect& rect) const {
    return rect.y + 0.5f * (rect.height - kGlyphBoxHeight * kTextScale);
}

bool UIContext::button(std::string_view label, const UIRectStyle& style) {
    return button(nextRow(), label, style);
}

bool UIContext::button(const UIRect& rect, std::string_view label, const UIRectStyle& style) {
    if (m_batch == nullptr) {
        return false;
    }
    const UIWidgetId id = hashId(m_panels.empty() ? kRootSeed : m_panels.back().seed, label);
    const bool hit = hitTest(rect);
    if (hit && (m_active == kUINoWidget || m_active == id)) {
        m_hovered = id;
    }
    if (m_pressedEdge && hit && (m_active == kUINoWidget || m_active == id)) {
        m_active = id;
        m_focused = id; // pressing a widget steals focus
        m_pressClaimed = true;
    }
    bool clicked = false;
    if (m_releasedEdge && m_active == id) {
        clicked = hit; // click completes only if released over the widget
        m_active = kUINoWidget;
    }

    const bool pressedVisual = m_active == id && hit;
    const glm::vec4 fill =
        pressedVisual ? m_theme.buttonActive : (m_hovered == id ? m_theme.buttonHovered : m_theme.buttonFill);
    UIRectStyle effective = style;
    if (effective.cornerRadius <= 0.0f) {
        effective.cornerRadius = 5.0f;
    }
    if (m_focused == id) {
        effective.borderWidth = std::max(effective.borderWidth, 1.5f);
        effective.borderColor = m_theme.focusBorder;
    } else if (effective.borderWidth <= 0.0f) {
        effective.borderWidth = 1.0f;
        effective.borderColor = glm::vec4{1.0f, 1.0f, 1.0f, 0.10f};
    }
    m_batch->drawRoundedRect(rect.x, rect.y, rect.width, rect.height, effective, fill);
    const float textWidth = m_batch->measureText(label, kTextScale);
    m_batch->drawText(label, rect.x + std::max(0.0f, 0.5f * (rect.width - textWidth)), textTop(rect),
                      kTextScale, m_theme.text);
    return clicked;
}

bool UIContext::slider(std::string_view label, float* value, float min, float max) {
    const UIRect row = nextRow();
    return sliderRow(row, label, hashId(m_panels.empty() ? kRootSeed : m_panels.back().seed, label), value,
                     min, max, m_theme.sliderFill);
}

bool UIContext::slider(const UIRect& rect, std::string_view label, float* value, float min, float max) {
    return sliderRow(rect, label, hashId(m_panels.empty() ? kRootSeed : m_panels.back().seed, label), value,
                     min, max, m_theme.sliderFill);
}

bool UIContext::sliderRow(const UIRect& rect, std::string_view label, UIWidgetId id, float* value, float min,
                          float max, const glm::vec4& accent) {
    if (m_batch == nullptr) {
        return false;
    }
    const UIRect track = sliderTrackRect(rect);
    const bool hit = hitTest(rect);
    if (hit && (m_active == kUINoWidget || m_active == id)) {
        m_hovered = id;
    }

    bool changed = false;
    if (value != nullptr && min < max) {
        if (m_pressedEdge && hit && (m_active == kUINoWidget || m_active == id)) {
            m_active = id;
            m_focused = id;
            m_pressClaimed = true;
        }
        if (m_active == id && m_input.mouseDown) {
            const float fraction =
                std::clamp((m_input.mouseX - track.x) / std::max(track.width, 1e-5f), 0.0f, 1.0f);
            const float dragged = min + fraction * (max - min);
            if (dragged != *value) {
                *value = dragged;
                changed = true;
            }
        }
        if (m_focused == id && m_input.keyStep != 0) {
            const float stepped =
                std::clamp(*value + static_cast<float>(m_input.keyStep) * (max - min) * 0.01f, min, max);
            if (stepped != *value) {
                *value = stepped;
                changed = true;
            }
        }
    }
    if (m_releasedEdge && m_active == id) {
        m_active = kUINoWidget;
    }

    // --- drawing -------------------------------------------------------------
    float fraction = 0.0f;
    if (value != nullptr && min < max) {
        fraction = std::clamp((*value - min) / (max - min), 0.0f, 1.0f);
    }
    m_batch->drawText(label, rect.x + 2.0f, textTop(rect), kTextScale, m_theme.text);
    m_batch->drawRoundedRect(track.x, track.y, track.width, track.height, UIRectStyle{3.0f, 0.0f, {}},
                             m_theme.trackFill);
    if (fraction > 0.0f) {
        m_batch->drawRoundedRect(track.x, track.y, track.width * fraction, track.height,
                                 UIRectStyle{3.0f, 0.0f, {}}, accent);
    }
    const float thumbWidth = 12.0f;
    const float thumbHeight = rect.height - 10.0f;
    const float thumbCenter = track.x + fraction * track.width;
    m_batch->drawRoundedRect(thumbCenter - 0.5f * thumbWidth, rect.y + 5.0f, thumbWidth, thumbHeight,
                             UIRectStyle{3.0f, 0.0f, {}}, m_theme.thumbFill);
    if (m_focused == id) {
        m_batch->drawRoundedRect(rect.x, rect.y, rect.width, rect.height,
                                 UIRectStyle{5.0f, 1.0f, m_theme.focusBorder}, glm::vec4{0.0f});
    }
    if (value != nullptr) {
        char valueText[32];
        std::snprintf(valueText, sizeof(valueText), "%.2f", static_cast<double>(*value));
        const float valueWidth = m_batch->measureText(valueText, kTextScale);
        m_batch->drawText(valueText, rect.x + rect.width - valueWidth - 6.0f, textTop(rect), kTextScale,
                          m_theme.text);
    }
    return changed;
}

bool UIContext::checkbox(std::string_view label, bool* value) {
    return checkbox(nextRow(), label, value);
}

bool UIContext::checkbox(const UIRect& rect, std::string_view label, bool* value) {
    if (m_batch == nullptr) {
        return false;
    }
    const UIWidgetId id = hashId(m_panels.empty() ? kRootSeed : m_panels.back().seed, label);
    const bool hit = hitTest(rect);
    if (hit && (m_active == kUINoWidget || m_active == id)) {
        m_hovered = id;
    }
    if (m_pressedEdge && hit && (m_active == kUINoWidget || m_active == id)) {
        m_active = id;
        m_focused = id;
        m_pressClaimed = true;
    }
    bool toggled = false;
    if (m_releasedEdge && m_active == id) {
        if (hit && value != nullptr) {
            *value = !*value;
            toggled = true;
        }
        m_active = kUINoWidget;
    }

    const float boxSize = 16.0f;
    const UIRect box{rect.x + 1.0f, rect.y + 0.5f * (rect.height - boxSize), boxSize, boxSize};
    UIRectStyle boxStyle{3.0f, 1.0f, glm::vec4{1.0f, 1.0f, 1.0f, 0.25f}};
    if (m_focused == id) {
        boxStyle.borderColor = m_theme.focusBorder;
    }
    m_batch->drawRoundedRect(box.x, box.y, box.width, box.height, boxStyle, m_theme.trackFill);
    if (value != nullptr && *value) {
        constexpr float inset = 3.5f;
        m_batch->drawRoundedRect(box.x + inset, box.y + inset, box.width - 2.0f * inset,
                                 box.height - 2.0f * inset, UIRectStyle{2.0f, 0.0f, {}}, m_theme.checkFill);
    }
    m_batch->drawText(label, rect.x + boxSize + 8.0f, textTop(rect), kTextScale, m_theme.text);
    return toggled;
}

bool UIContext::vec3Sliders(std::string_view label, glm::vec3* value, float min, float max) {
    if (m_batch == nullptr || value == nullptr) {
        return false;
    }
    const std::string base(label);
    const UIWidgetId seed = m_panels.empty() ? kRootSeed : m_panels.back().seed;
    bool changed = false;
    changed = sliderRow(nextRow(), base + ".x", hashId(seed, base + ".x"), &value->x, min, max,
                        glm::vec4{1.0f, 0.45f, 0.40f, 1.0f}) ||
              changed;
    changed = sliderRow(nextRow(), base + ".y", hashId(seed, base + ".y"), &value->y, min, max,
                        glm::vec4{0.55f, 0.95f, 0.50f, 1.0f}) ||
              changed;
    changed = sliderRow(nextRow(), base + ".z", hashId(seed, base + ".z"), &value->z, min, max,
                        glm::vec4{0.45f, 0.65f, 1.0f, 1.0f}) ||
              changed;
    return changed;
}

} // namespace engine
