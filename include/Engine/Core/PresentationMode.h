#pragma once

namespace engine {

// Presentation policy requested in [Render] (config/engine.ini).
//
// Auto keeps the historical vsync switch in charge:
//   vsync = true  -> FIFO
//   vsync = false -> IMMEDIATE, then MAILBOX, then FIFO
//
// An explicit mode overrides the vsync switch. FIFO_RELAXED is still a
// vsync mode, but may tear when a frame misses its refresh deadline.
enum class PresentationMode {
    Auto,
    Fifo,
    Mailbox,
    Immediate,
    FifoRelaxed,
};

[[nodiscard]] constexpr const char* presentationModeName(PresentationMode mode) {
    switch (mode) {
    case PresentationMode::Auto: return "auto";
    case PresentationMode::Fifo: return "fifo";
    case PresentationMode::Mailbox: return "mailbox";
    case PresentationMode::Immediate: return "immediate";
    case PresentationMode::FifoRelaxed: return "fifo_relaxed";
    }
    return "auto";
}

} // namespace engine
