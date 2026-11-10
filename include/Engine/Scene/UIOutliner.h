#pragma once
#include "Core/Export.h"

#include <entt/entt.hpp>

#include <cstdint>

#include "Graphics/UIContext.h"

namespace engine {

class Scene;

// ---------------------------------------------------------------------------
// UIOutliner (Phase 27): docked Scene Hierarchy panel. Lists every
// entity in the scene registry as an indented tree (roots sorted
// by tag, children in scene order), with:
//
//   * left-click row selection (pressedRect) — feeds the
//     reflection-driven Component Inspector,
//   * wheel scrolling inside the panel (rows are scissor-clipped
//     by the panel, and hit-testing respects that clip),
//   * a right-click context menu per entity (Popup in UIContext):
//     Duplicate / Create Child / Delete, executed directly on the
//     Scene. Delete delegates to Scene::destroyEntity, so its
//     HierarchyDestroyMode decides the children's fate (orphaned
//     to roots by default, destroyed with the parent in Cascade
//     mode). A duplicate copies the data components, adopts the
//     same parent, and re-creates any NativeScriptComponent fresh
//     so no script instance is ever shared).
//
// Selection survives deletion safely: a stale selection (deleted
// directly or via an ancestor's cascade) is cleared on the next
// build. Lives in EngineScene beside the registry it drives.
// ---------------------------------------------------------------------------
class ENGINE_SCENE_API UIOutliner {
public:
    enum class Action : uint8_t { None, Duplicated, Deleted, CreatedChild };

    struct Result {
        Action action = Action::None;
        entt::entity entity = entt::null; // created entity, or the deleted one
        bool selectionChanged = false;
    };

    static constexpr float kRowHeight = 22.0f;

    Result build(UIContext& ui, const UIRect& rect, Scene& scene);

    [[nodiscard]] entt::entity selected() const { return m_selected; }
    void setSelected(entt::entity entity) { m_selected = entity; }
    [[nodiscard]] float scrollOffset() const { return m_scroll; }

private:
    entt::entity m_selected = entt::null;
    entt::entity m_contextTarget = entt::null;
    float m_scroll = 0.0f; // pixels
};

} // namespace engine
