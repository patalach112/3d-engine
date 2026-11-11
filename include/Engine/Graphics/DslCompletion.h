#pragma once
#include "Core/Export.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace engine {

// ---------------------------------------------------------------------------
// DslCompletion — context-aware auto-completion for .dsla scripts.
//
// A pure function over (source, cursor, name registries): no UI, no
// scene access, so the editor, tools and unit tests all share it.
// Contexts it understands:
//   * word prefix          -> engine commands + the script's own
//                             variables (assignments / bound decls)
//   * member access        -> play. -> animation(", enable./disable.
//                             -> ragdoll(), effect. -> properties
//   * inside a string      -> animation clip names inside
//                             play.animation("..."), entity names
//                             inside part("...") / on_touch("...")
//                             / position("...") / inhand("...")
// Accepting a command stem typically ends in `("`, which moves the
// cursor into the string context — the very next completion then
// offers the clip/entity names. Two keystrokes, fully assisted.
// ---------------------------------------------------------------------------

enum class DslCompletionKind : uint8_t { Keyword, Command, Variable, AnimationClip, EntityName };

struct DslCompletionItem {
    std::string text;   // inserted at accept
    std::string detail; // dimmed explanation in the popup
    DslCompletionKind kind = DslCompletionKind::Command;
};

struct DslCompletionResult {
    bool active = false;     // a completion context exists at the cursor
    size_t replaceStart = 0; // source byte offset the partial token starts at
    std::string prefix;      // the partial text being completed
    std::vector<DslCompletionItem> items;
};

// Names a script defines itself (the left side of assignments and
// `name(max|lowest) = ...` bound declarations), sorted, deduplicated,
// reserved words excluded.
[[nodiscard]] ENGINE_GRAPHICS_API std::vector<std::string> dslScriptVariables(std::string_view source);

[[nodiscard]] ENGINE_GRAPHICS_API DslCompletionResult
dslCompletions(std::string_view source, size_t cursorOffset, const std::vector<std::string>& animationNames,
               const std::vector<std::string>& entityNames);

} // namespace engine
