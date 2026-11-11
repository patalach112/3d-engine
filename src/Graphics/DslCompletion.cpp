#include "Graphics/DslCompletion.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace engine {

namespace {

[[nodiscard]] bool isWordChar(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return std::isalnum(u) != 0 || c == '_' || c == '.';
}

[[nodiscard]] bool isIdentStart(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return std::isalpha(u) != 0 || c == '_';
}

const std::unordered_set<std::string>& reservedWords() {
    static const std::unordered_set<std::string> words = {
        "from", "start", "script", "end", "if", "for", "every", "inhand", "part", "position", "behavior",
        "on_touch", "on_trigger", "on_collide", "on_collision", "on_interact", "on_input", "on_input_release",
        "on_hold", "on_start", "play_sound", "emit_sound", "print", "hudprint", "hudclear", "destroy", "freeze",
        "play", "enable", "disable", "animation", "ragdoll", "effect", "over", "blend", "max", "lowest", "self",
        "this", "reset", "scale", "color", "true", "false", "infinite",
    };
    return words;
}

struct CatalogEntry {
    const char* text;
    const char* detail;
    DslCompletionKind kind;
};

const std::vector<CatalogEntry>& commandCatalog() {
    static const std::vector<CatalogEntry> catalog = {
        {"on_touch(\"", "event: solid contact with the named part", DslCompletionKind::Command},
        {"on_trigger(\"", "event: entered the named trigger volume", DslCompletionKind::Command},
        {"on_collide(\"", "event: collision with the named part", DslCompletionKind::Command},
        {"on_collision(\"", "event: collision (alias)", DslCompletionKind::Command},
        {"on_interact(\"", "event: interacted with the named part", DslCompletionKind::Command},
        {"on_input(\"", "event: key/button pressed", DslCompletionKind::Command},
        {"on_input_release(\"", "event: key/button released", DslCompletionKind::Command},
        {"on_hold(\"", "event: key/button held down", DslCompletionKind::Command},
        {"on_start() {", "runs once when the script starts", DslCompletionKind::Command},
        {"hudprint(\"", "print text on the HUD", DslCompletionKind::Command},
        {"hudclear(\"", "close this script's HUD prints", DslCompletionKind::Command},
        {"print(\"", "write a line to the script log", DslCompletionKind::Command},
        {"play_sound(\"", "play a sound at the host", DslCompletionKind::Command},
        {"emit_sound(\"", "emit a spatial sound", DslCompletionKind::Command},
        {"play.animation(\"", "play an animation clip on this rig", DslCompletionKind::Command},
        {"enable.ragdoll()", "switch this rig to physics ragdoll", DslCompletionKind::Command},
        {"disable.ragdoll()", "restore keyframed animation", DslCompletionKind::Command},
        {"part(\"", "scope or property target by name", DslCompletionKind::Command},
        {"position(\"", "zone guard block", DslCompletionKind::Command},
        {"inhand(\"", "pick up / guard on a held item", DslCompletionKind::Command},
        {"every(", "repeating timer (every(1s) { ... })", DslCompletionKind::Command},
        {"for(", "timed window (for(10s) { ... })", DslCompletionKind::Command},
        {"if (", "conditional block", DslCompletionKind::Command},
        {"destroy()", "destroy the target entity", DslCompletionKind::Command},
        {"freeze()", "freeze the target entity", DslCompletionKind::Command},
        {"from start()", "program header", DslCompletionKind::Command},
        {"script(end)", "program footer", DslCompletionKind::Command},
        {"over ", "interpolate/blend over a duration", DslCompletionKind::Keyword},
        {"blend ", "crossfade over a duration", DslCompletionKind::Keyword},
        {"effect.", "global post effect property", DslCompletionKind::Keyword},
    };
    return catalog;
}

[[nodiscard]] int kindPriority(DslCompletionKind kind) {
    switch (kind) {
    case DslCompletionKind::Variable:
    case DslCompletionKind::AnimationClip:
    case DslCompletionKind::EntityName:
        return 0; // contextual names outrank the static catalog
    case DslCompletionKind::Command:
        return 1;
    case DslCompletionKind::Keyword:
        return 2;
    }
    return 3;
}

void finish(DslCompletionResult& result) {
    std::stable_sort(result.items.begin(), result.items.end(), [](const DslCompletionItem& a, const DslCompletionItem& b) {
        const int pa = kindPriority(a.kind);
        const int pb = kindPriority(b.kind);
        return pa != pb ? pa < pb : a.text < b.text;
    });
    if (result.items.size() > 40) {
        result.items.resize(40);
    }
    result.active = !result.items.empty();
}

} // namespace

std::vector<std::string> dslScriptVariables(std::string_view source) {
    std::vector<std::string> variables;
    std::unordered_set<std::string> seen;
    size_t i = 0;
    const auto skipSpaces = [&]() {
        while (i < source.size() && (source[i] == ' ' || source[i] == '\t')) {
            ++i;
        }
    };
    while (i < source.size()) {
        if (!isIdentStart(source[i])) {
            ++i;
            continue;
        }
        const size_t start = i;
        while (i < source.size() && (isWordChar(source[i]) && source[i] != '.')) {
            ++i;
        }
        const std::string word(source.substr(start, i - start));
        skipSpaces();
        bool isVariable = false;
        if (i < source.size() && source[i] == '=' && (i + 1 >= source.size() || source[i + 1] != '=')) {
            isVariable = true; // name = ...
        } else if (i < source.size() && source[i] == '(') {
            size_t j = i + 1;
            while (j < source.size() && (source[j] == ' ')) {
                ++j;
            }
            const size_t innerStart = j;
            while (j < source.size() && isWordChar(source[j]) && source[j] != '.') {
                ++j;
            }
            const std::string_view inner = source.substr(innerStart, j - innerStart);
            isVariable = inner == "max" || inner == "lowest"; // name(max) = ...
        }
        if (isVariable && !reservedWords().contains(word) && seen.insert(word).second) {
            variables.push_back(word);
        }
    }
    std::sort(variables.begin(), variables.end());
    return variables;
}

DslCompletionResult dslCompletions(std::string_view source, size_t cursorOffset,
                                   const std::vector<std::string>& animationNames,
                                   const std::vector<std::string>& entityNames) {
    DslCompletionResult result;
    const size_t cursor = std::min(cursorOffset, source.size());

    // Inside a string literal? (backslash escapes honoured)
    size_t quoteStart = std::string_view::npos;
    {
        bool inString = false;
        for (size_t i = 0; i < cursor; ++i) {
            if (source[i] == '\\' && i + 1 < cursor) {
                ++i;
                continue;
            }
            if (source[i] == '"') {
                inString = !inString;
                quoteStart = inString ? i : std::string_view::npos;
            }
        }
        if (!inString) {
            quoteStart = std::string_view::npos;
        }
    }

    if (quoteStart != std::string_view::npos) {
        // Which call owns this string? Walk back: '(' then a
        // (possibly dotted) identifier chain.
        size_t at = quoteStart;
        while (at > 0 && (source[at - 1] == ' ' || source[at - 1] == '\t')) {
            --at;
        }
        if (at == 0 || source[at - 1] != '(') {
            return result; // a bare string: nothing to complete
        }
        --at; // the '('
        while (at > 0 && (source[at - 1] == ' ' || source[at - 1] == '\t')) {
            --at;
        }
        const size_t chainEnd = at;
        while (at > 0 && isWordChar(source[at - 1])) {
            --at;
        }
        const std::string_view call = source.substr(at, chainEnd - at);
        result.prefix = std::string(source.substr(quoteStart + 1, cursor - quoteStart - 1));
        result.replaceStart = quoteStart + 1;
        const auto addFiltered = [&](const std::vector<std::string>& names, DslCompletionKind kind, const char* detail) {
            for (const std::string& name : names) {
                if (name.starts_with(result.prefix)) {
                    result.items.push_back(DslCompletionItem{name, detail, kind});
                }
            }
        };
        if (call == "play.animation") {
            addFiltered(animationNames, DslCompletionKind::AnimationClip, "animation clip");
        } else if (call == "part" || call == "on_touch" || call == "on_trigger" || call == "on_collide" ||
                   call == "on_collision" || call == "on_interact" || call == "position" || call == "inhand") {
            addFiltered(entityNames, DslCompletionKind::EntityName, "scene entity");
        } else {
            return result; // free text (hudprint/print/...): no registry to offer
        }
        finish(result);
        return result;
    }

    // Word/member context: the token is [A-Za-z0-9_.]* ending at the cursor.
    size_t tokenStart = cursor;
    while (tokenStart > 0 && isWordChar(source[tokenStart - 1])) {
        --tokenStart;
    }
    const std::string token(source.substr(tokenStart, cursor - tokenStart));
    result.replaceStart = tokenStart;
    result.prefix = token;
    if (token.empty()) {
        return result;
    }

    if (const size_t dot = token.rfind('.'); dot != std::string::npos) {
        const std::string head = token.substr(0, dot);
        const std::string member = token.substr(dot + 1);
        const auto addMember = [&](const char* text, const char* detail) {
            if (std::string_view(text).starts_with(member)) {
                result.items.push_back(DslCompletionItem{text, detail, DslCompletionKind::Command});
            }
        };
        if (head == "play") {
            addMember("animation(\"", "play an animation clip on this rig");
        } else if (head == "enable" || head == "disable") {
            addMember("ragdoll()", "toggle physics ragdoll on this rig");
        } else if (head == "effect") {
            addMember("chromatic_aberration", "global post effect");
        } else {
            return result; // unknown member chain (part(...).x, variables): nothing static to offer
        }
        finish(result);
        return result;
    }

    for (const CatalogEntry& entry : commandCatalog()) {
        if (std::string_view(entry.text).starts_with(token)) {
            result.items.push_back(DslCompletionItem{entry.text, entry.detail, entry.kind});
        }
    }
    for (const std::string& variable : dslScriptVariables(source)) {
        if (variable.starts_with(token) && variable != token) {
            result.items.push_back(DslCompletionItem{variable, "script variable", DslCompletionKind::Variable});
        }
    }
    finish(result);
    return result;
}

} // namespace engine
