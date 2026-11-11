#pragma once

namespace engine {

// ---------------------------------------------------------------------------
// Scenario 18 — the canonical Backrooms DSL acceptance script.
//
// It exercises the whole language in one program: variable init,
// interact + inventory pickup with world-item destruction, the
// guarded input composition if { inhand { on_input } }, the
// Almond Water regeneration window for(10s) { every(1s) }, the
// safe-room heal window with a light mutation, and the nested
// event composition on_trigger { on_collision(part) { position =
// reset } }. Shared verbatim by the scripting demo executable and
// the core test suite so the two can never drift apart.
// ---------------------------------------------------------------------------
inline constexpr const char* kScenario18Script = R"DSL(from start()
    sanity = 100
    health = 100

    // The monster hunts with the engine's native stalker AI
    // (player tracking + raycast obstacle avoidance); the script
    // only assigns the behaviour, never steers.
    part("stalker") {
        behavior = stalker
    }

    // Picking up the Almond Water: it goes in hand and the world
    // item is destroyed.
    on_interact("almond_water") {
        inhand("almond_water")
        part("almond_water").destroy()
        print("Picked up Almond Water")
    }

    // Using it (LMB while held): +50 sanity and +2 HP every
    // second, for 10 seconds.
    if {
        inhand("almond_water") {
            on_input("LMB") {
                for(10s) {
                    every(1s) {
                        sanity = sanity + 50
                        health = health + 2
                    }
                }
            }
        }
    }

    // The safe room: heals over time, lights come up, and the
    // stalker can never stay inside — any collision it has after
    // the room is touched resets it to its spawn point.
    on_trigger("safe_room") {
        for(10s) {
            every(3s) {
                health = health + 2
                sanity = sanity + 50
            }
        }
        part("safe_lights").intensity = 1.0
        on_collision(part("stalker")) {
            position = reset
        }
    }
script(end)
)DSL";

} // namespace engine
