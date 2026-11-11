#pragma once
#include "Core/Export.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace engine {

// ---------------------------------------------------------------------------
// Backrooms DSL — lexer, parser and AST (the spec's DSLParser)
//
// The DSL abstracts spatial / temporal / physics / state logic of
// psychological-horror scenarios into event-driven blocks that a
// level designer writes without C++. Grammar (whitespace-free,
// `//` comments allowed):
//
//   program     := 'from' 'start' '(' ')' statement* 'script' '(' 'end' ')'
//   statement   := assignment | partAssign | partCall | eventBlock
//                | inhandCall | inhandBlock | temporal | ifBlock
//                | partBlock | positionBlock | soundCall | boundDecl
//                | hudPrint | effectAssign
//   assignment  := IDENT '=' expr ('over' duration)?
//                  -- sanity = sanity + 50, speed = 6.0 over 1s (lerp)
//                  'position' '=' 'reset'          (respawn context entity)
//                  'behavior' '=' IDENT            (behavior = stalker)
//   boundDecl   := IDENT '(' 'max'|'lowest' ')' '=' expr
//                  -- health(max) = 100: assignments clamp natively
//   dottedAssign:= IDENT '.' IDENT '=' expr ('over' duration)?
//                  -- light.intensity = 0.0 over 2s (part() shorthand)
//   hudPrint    := 'hudprint' '(' STRING ')' '{' hudKeys '}'
//                  -- hudKeys: position = X%, Y% / duration = 3s /
//                     color = #FFAA00 / fade = true|false
//   effectAssign:= 'effect' '.' IDENT '=' expr ('over' duration)?
//                  -- effect.chromatic_aberration = 0.3 (global post)
//   partAssign  := 'part' '(' target ')' '.' IDENT '=' expr
//                  -- part("safe_lights").intensity = 1.0
//                  'part' '(' target ')' '.' 'scale' '(' 'x'|'y'|'z' ')' '=' expr
//   partCall    := 'part' '(' target ')' '.' ('destroy'|'freeze') '(' ')'
//   eventBlock  := ('on_input'|'on_input_release'|'on_hold'|'on_touch'
//                  |'on_trigger'|'on_collide'|'on_collision'|'on_interact')
//                  '(' arg ')' block | 'on_start' '(' ')' block
//                  -- on_touch = solid collider impacts, on_trigger =
//                     ghost-volume entries, on_start runs once at setup
//   inhandCall  := 'inhand' '(' STRING ')'        (pick the item up)
//   inhandBlock := 'inhand' '(' STRING ')' block  (guard: while held)
//   temporal    := ('every'|'for') '(' duration ')' block
//                  -- durations: 10s, 500ms, or a plain number (seconds)
//   ifBlock     := 'if' ('(' expr ')')? block
//   partBlock   := 'part' '(' target ')' block    (scoped context entity)
//   positionBlk := 'position' '(' STRING ')' block (guard: inside zone)
//   soundCall   := ('play_sound'|'emit_sound'|'print') '(' STRING ')'
//   expr        := comparison of additive terms over numbers,
//                  strings, identifiers, parentheses and unary '-';
//                  comparisons yield 1/0. A bare identifier with no
//                  variable behind it evaluates as a text tag
//                  (that's how `behavior = stalker` works).
//
// Duration composition is the heart of the language: `for(10s)`
// opens a window, `every(1s)` inside it ticks until the window
// closes, and the two nest arbitrarily (Almond Water regeneration:
// for(10s) { every(1s) { health = health + 2 } }).
// ---------------------------------------------------------------------------

enum class DSLNodeKind : uint8_t {
    Program,
    Assign,      // name = expr            (name/property in `property` for special forms)
    PartAssign,  // part(name).property = expr  (axis in `extra` for scale)
    PartCall,    // part(name).destroy()/freeze()  (call in `name`)
    EventBlock,  // on_*(name) { body }     (event kind in `event`)
    InhandCall,  // inhand(name)
    InhandBlock, // inhand(name) { body }
    Every,       // every(duration) { body }
    For,         // for(duration) { body }
    If,          // if (expr)? { body }
    PartBlock,   // part(name) { body }
    PositionBlock, // position(name) { body }
    SoundCall,   // play_sound/emit_sound/print(name)  (call in `extra`)
    BoundDecl,   // name(max|lowest) = expr   (property = "max"/"lowest")
    HudPrint,    // hudprint("text") { position/duration/color/fade }
    HudClear,    // hudclear("text")? -- removes this script's HUD prints
    EffectAssign, // effect.<property> = expr   (property name in `name`)
};

enum class DSLEventKind : uint8_t { None, Interact, Touch, Trigger, Collide, InputPress, InputHold, InputRelease, Start };

struct DSLExpr {
    enum class Kind : uint8_t { Number, Text, Variable, Binary, Unary } kind = Kind::Number;
    double number = 0.0;              // Number value
    std::string text;                 // Text value / Variable name
    std::string op;                   // Binary/Unary operator
    std::vector<DSLExpr> args;        // operands (1 unary, 2 binary)
};

struct DSLNode {
    DSLNodeKind kind = DSLNodeKind::Program;
    DSLEventKind event = DSLEventKind::None; // EventBlock only
    std::string name;     // variable / part target / item / zone / key name
    std::string property; // PartAssign property ("intensity", "scale", ...)
    std::string extra;    // PartAssign scale axis, SoundCall verb, PartCall verb
    bool hasCondition = false; // If only
    // Smooth interpolation: `= expr over 1.5s` on Assign / PartAssign /
    // EffectAssign spawns a non-blocking lerp task in the runtime.
    bool hasLerp = false;
    double lerpDuration = 0.0; // seconds
    // Compound assignment (health -= 25): empty means plain;
    // otherwise one of "+","-","*","/" applied as
    // target = target <op> expr through the clamped write path.
    std::string compoundOp;
    // HudPrint settings (percent canvas anchors, #RRGGBB colour).
    // No duration (or `duration = infinite`) means persistent:
    // the print stays until hudclear closes it.
    float hudXPercent = 50.0f;
    float hudYPercent = 50.0f;
    double hudDuration = 0.0;
    bool hudPersistent = true;
    float hudRed = 1.0f, hudGreen = 1.0f, hudBlue = 1.0f;
    bool hudFade = false;
    // Hex colour literal on a `color` property assignment
    // (part("lamp").color = #FF8000, lights only).
    bool colorLiteral = false;
    float colorRed = 1.0f, colorGreen = 1.0f, colorBlue = 1.0f;
    DSLExpr expr;              // Assign/PartAssign value, If condition, Every/For duration
    std::vector<DSLNode> body; // nested statements
    int line = 0;              // source line for diagnostics
};

struct DSLProgram {
    std::vector<DSLNode> statements;
    bool valid = false;
};

struct DSLParseResult {
    DSLProgram program;
    std::vector<std::string> errors; // "line N: ..." entries; empty == clean

    [[nodiscard]] bool ok() const { return errors.empty() && program.valid; }
};

// Parses one DSL program. Never throws on malformed input: every
// lexical or grammatical problem lands in `errors` with its line.
[[nodiscard]] ENGINE_SCENE_API DSLParseResult parseBackroomsScript(std::string_view source);

} // namespace engine
