#pragma once
#include "Net/NetProtocol.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <unordered_map>

namespace engine {

// ---------------------------------------------------------------------------
// Snapshot delta compression (Source-style baseline deltas).
//
// The server keeps, per client, the last snapshot state the client
// ACKNOWLEDGED (echoed back inside every InputCommand). A new
// snapshot contains only entities whose replicated state differs
// from that baseline plus the ids that vanished; unchanged
// entities are skipped entirely — an idle world costs one packet
// header per tick. Until a client has acked anything (or its ack
// falls more than kMaxBaselineAge ticks behind), snapshots are
// FULL and replace the client's world wholesale, which is also
// the loss-recovery path: any drift heals at the next full.
//
// buildSnapshot()/applySnapshot() are pure functions over plain
// data so the whole scheme is unit-testable without sockets.
// ---------------------------------------------------------------------------

// Exact field equality: any change (including health/alive flips)
// makes the entity "changed" for delta purposes.
[[nodiscard]] inline bool snapshotStateEquals(const EntitySnapshot& a, const EntitySnapshot& b) {
    return a.entity == b.entity && a.position == b.position && a.velocity == b.velocity && a.yaw == b.yaw &&
           a.health == b.health && a.hasHealth == b.hasHealth && a.alive == b.alive;
}

// Per-client baseline bookkeeping (server side). Retains the last
// kHistory sent states so an ack for any recent tick can promote
// that state to the baseline; older acks simply leave the older
// baseline in place (deltas stay correct, just larger).
struct SnapshotBaseline {
    static constexpr std::uint32_t kMaxBaselineAge = 240; // ticks before forcing full
    static constexpr std::size_t kHistory = 32;

    std::uint32_t ackTick = 0; // tick of `acked` (0 = nothing acked)
    std::unordered_map<std::uint32_t, EntitySnapshot> acked;
    std::array<std::uint32_t, kHistory> historyTicks{};
    std::array<std::unordered_map<std::uint32_t, EntitySnapshot>, kHistory> history{};
    std::size_t historyCursor = 0;

    void reset() {
        ackTick = 0;
        acked.clear();
        historyTicks.fill(0);
        for (auto& slot : history)
            slot.clear();
        historyCursor = 0;
    }

    // Records the state sent at `tick` so a later onAck(tick) can
    // promote it. Called once per snapshot sent.
    void recordSent(std::uint32_t tick, const std::vector<EntitySnapshot>& states) {
        auto& slot = history[historyCursor];
        slot.clear();
        for (const EntitySnapshot& state : states)
            slot[state.entity] = state;
        historyTicks[historyCursor] = tick;
        historyCursor = (historyCursor + 1) % kHistory;
    }

    void onAck(std::uint32_t tick) {
        if (tick == 0 || !netTickNewer(tick, ackTick))
            return;
        for (std::size_t i = 0; i < kHistory; ++i) {
            if (historyTicks[i] == tick) {
                acked = history[i];
                ackTick = tick;
                return;
            }
        }
    }

    [[nodiscard]] bool needsFull(std::uint32_t currentTick) const {
        return ackTick == 0 || (currentTick - ackTick) > kMaxBaselineAge;
    }
};

// Builds the snapshot to send one client at `tick`. `current` is
// the full replicated world (any order; output is sorted by entity
// id for byte-level determinism). The caller records the send via
// baseline.recordSent().
[[nodiscard]] inline Snapshot buildSnapshot(const std::vector<EntitySnapshot>& current,
                                            const SnapshotBaseline& baseline, std::uint32_t tick,
                                            std::uint32_t lastCmdNumber) {
    Snapshot snapshot;
    snapshot.tick = tick;
    snapshot.lastCmdNumber = lastCmdNumber;
    snapshot.full = baseline.needsFull(tick);
    snapshot.baseline = snapshot.full ? 0 : baseline.ackTick;

    if (snapshot.full) {
        snapshot.entities = current;
    } else {
        snapshot.entities.reserve(current.size());
        for (const EntitySnapshot& state : current) {
            const auto it = baseline.acked.find(state.entity);
            if (it == baseline.acked.end() || !snapshotStateEquals(it->second, state))
                snapshot.entities.push_back(state);
        }
        for (const auto& [entity, state] : baseline.acked) {
            const bool stillPresent =
                std::any_of(current.begin(), current.end(),
                            [entity](const EntitySnapshot& s) { return s.entity == entity; });
            if (!stillPresent)
                snapshot.removed.push_back(entity);
        }
        std::sort(snapshot.removed.begin(), snapshot.removed.end());
    }
    std::sort(snapshot.entities.begin(), snapshot.entities.end(),
              [](const EntitySnapshot& a, const EntitySnapshot& b) { return a.entity < b.entity; });
    return snapshot;
}

// Applies a received snapshot to the client's world map. Full
// snapshots replace; deltas merge and honour `removed`.
inline void applySnapshot(std::unordered_map<std::uint32_t, EntitySnapshot>& world, const Snapshot& snapshot) {
    if (snapshot.full)
        world.clear();
    for (const EntitySnapshot& state : snapshot.entities)
        world[state.entity] = state;
    for (const std::uint32_t entity : snapshot.removed)
        world.erase(entity);
}

} // namespace engine
