#pragma once
#include "Core/Export.h"

#include "Graphics/UIContext.h"
#include "Scene/Reflection.h"

namespace engine {

// ---------------------------------------------------------------------------
// Component Inspector (Phase 26 Step 3): builds an interactive
// panel from Phase 25 reflection metadata. The mapping is purely
// type-driven, so any reflected struct becomes editable UI with
// no per-type code:
//
//   Float        -> slider (options.floatMin..floatMax)
//   Bool         -> checkbox toggle
//   Vec3         -> three component sliders (options.…Min/…Max)
//   Int32/UInt32 -> read-only label row (value readout)
//
// Writes go through the reflected offsets (writeProperty), so the
// target is edited in place; the return value reports whether any
// property changed this frame. Only the header-only reflection
// templates are used, so this lives in EngineGraphics without a
// link dependency on EngineScene.
// ---------------------------------------------------------------------------

struct UIInspectorOptions {
    float floatMin = 0.0f;
    float floatMax = 100.0f;
    float componentMin = -10.0f;
    float componentMax = 10.0f;
};

ENGINE_GRAPHICS_API bool inspectComponent(UIContext& ui, const UIRect& rect, const TypeInfo& type,
                                          void* instance, const UIInspectorOptions& options = {});

} // namespace engine
