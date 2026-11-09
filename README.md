# 3D Engine

A modern, general-purpose 3D game engine built from scratch in C++20 on **Vulkan**.

Development follows a strict roadmap: the engine core and rendering pipeline come
first. Visual tools, UI, and editors are out of scope until the core is complete
and fully functional. No legacy OpenGL, no bloated frameworks.

## Technology stack

| Area      | Choice                                    |
|-----------|-------------------------------------------|
| Graphics  | Vulkan (explicit instance/device/swapchain/pipeline management) |
| Windowing | GLFW                                      |
| Math      | GLM (configured for Vulkan: radians, depth range `[0, 1]`) |
| Language  | C++20                                     |
| Build     | CMake ≥ 3.24 (GLFW and GLM are fetched automatically) |

## Phase 1 — First Render ✅

A minimal, clean application that verifies the whole rendering pipeline:

- GLFW window created with `GLFW_NO_API` (plus `GLFW_INCLUDE_NONE`, so no OpenGL
  headers are ever pulled in)
- Vulkan instance with debug-utils validation in Debug builds
  (`VK_LAYER_KHRONOS_validation`, gracefully skipped if the SDK layer is absent)
- Physical device selection (prefers a discrete GPU), logical device with
  graphics + present queues
- Swapchain (sRGB format preferred), depth buffer, render pass, and framebuffers
- Fixed graphics pipeline with dynamic viewport/scissor, depth testing, and
  back-face culling
- A unit cube with one distinct color per face (24 vertices / 36 indices),
  stored in device-local vertex/index buffers uploaded through staging buffers
- Per-frame uniform buffers feeding model/view/projection matrices (GLM) to the
  vertex shader through descriptor sets; the cube tumbles continuously around
  multiple axes at different speeds
- Frames-in-flight synchronization (2 frames): per-frame fences and
  image-available semaphores, per-swapchain-image render-finished semaphores
- Window-resize handling with swapchain recreation (including minimized /
  zero-size windows), `Esc` to exit, and ordered RAII cleanup of every object

## Phase 2 — Core Infrastructure: Logger & Robust Game Loop ✅

Everything is data-driven: no paths, rates or limits are hardcoded in engine
code. All tuning lives in **`config/engine.ini`** (see
[Configuration](#configuration)).

### Logger (`src/Core/Logger.*`)

- Six levels — Trace, Debug, Info, Warning, Error, Fatal — with a configurable
  minimum level; disabled levels are checked before the message expression is
  even evaluated
- Dual sinks: color-coded console output (ANSI colors, terminal-aware) and a
  log file written **asynchronously** from a background thread fed by a bounded
  queue, so disk I/O never stalls the frame loop (overflow drops are counted
  and reported)
- File sink creates directories automatically, optionally inserts a timestamp
  into the file name, and rotates by size (`engine.log` → `engine.1.log` → …)
  keeping a configurable number of archives; a missing/unwritable log file
  degrades to console-only instead of aborting startup
- Stream-style macros with automatic source location:

  ```cpp
  ENGINE_LOG_INFO("Loaded " << assetCount << " assets");
  ENGINE_LOG_ERROR("Failed to open " << path);
  ```

  Every line is timestamped and ends with `(File.cpp:line)`. Error/Fatal
  messages flush all sinks immediately. Vulkan validation messages are routed
  through the logger as warnings/errors.

### Time & game loop (`src/Core/Time.*`, `src/Core/GameLoop.*`)

- `Time`: monotonic `std::chrono::steady_clock` frame clock — delta time,
  total elapsed time, frame count, smoothed FPS
- `FixedTimestep`: pure, unit-testable accumulator that turns variable frame
  deltas into integral fixed steps, clamps hitches to `maxFrameSeconds`, caps
  catch-up work per frame and drops deeper backlogs (counted in the stats)
  instead of spiraling
- `GameLoop`: orchestrates frames through plain callbacks — no dependency on
  the window or renderer:

  ```
  shouldContinue → pollEvents → fixedUpdate × N → update(dt) → render(alpha)
  ```

  The cube's rotation is integrated in **fixed updates** (deterministic) and
  rendered with extrapolation by the interpolation `alpha`, so motion is
  smooth at any display rate. An optional software FPS cap and periodic
  loop statistics (frames, FPS, fixed steps, dropped time) are configurable.

## Phase 3, Step 1 — Input System & First-Person Camera ✅

### Input (`src/Platform/Input.*`)

Event-driven input state on top of GLFW callbacks. The `Window` owns the
GLFW callbacks and forwards events to a registered `Input` instance; every
other system polls clean, engine-level state — no GLFW key codes anywhere
outside the Platform layer:

- Held state: `isKeyDown(Key::W)`, `isMouseButtonDown(MouseButton::Left)`
- Per-frame transitions: `wasKeyPressed()`, `wasMouseButtonReleased()`, …
- Mouse motion for look controls: `mouseDelta()` (pixels per frame),
  `mousePosition()`, `scrollDelta()`
- Cursor capture (hidden, unbounded) for mouse look, cleared state on
  focus loss, and a strict frame protocol: `beginFrame()` → `pollEvents()`
  → systems query state

### First-person camera (`src/Core/Camera.*`)

Yaw/pitch fly camera built with GLM. Pitch is clamped to a configurable
limit (default ±89°), so the front vector never aligns with the world up
vector — no gimbal lock, no view flipping. Yaw wraps to [-180°, 180°).

**Controls** (with mouse captured):

| Input       | Action                    |
|-------------|---------------------------|
| Mouse       | Look around               |
| W / A / S / D | Move forward/left/back/right |
| E / Q       | Move up / down            |
| Shift       | Boost (configurable multiplier) |
| Click       | Capture the mouse         |
| Esc         | Release mouse, again to quit |

Movement is integrated with the variable update's delta time (diagonal
movement normalized), sensitivity/speed/boost/clip planes are all
configurable under `[Camera]`/`[Input]` in `engine.ini`, and the camera's
view/projection matrices feed the existing UBO each frame — the cube is
static now and the player flies around it. Periodic loop statistics now
also report camera position/yaw/pitch.

### Unit tests

`tests/CoreTests.cpp` (CTest: `engine-core-tests`) covers configuration
parsing/fallbacks, log filtering and rotation, fixed-timestep math, the
frame clock and loop dispatch — everything verifiable without a GPU.

## Configuration

`config/engine.ini` is copied next to the executable at build time and can be
edited without rebuilding. Lookup order:

1. `--config <path>` command-line argument (or `--config=<path>`)
2. `ENGINE_CONFIG_PATH` environment variable
3. `config/engine.ini` relative to the working directory
4. The copy in the source tree (development fallback)
5. Built-in defaults

Invalid or unknown keys never abort startup: they fall back to defaults and
are reported as warnings. Sections: `[Window]`, `[Render]` (`vsync` selects
FIFO vs. low-latency present modes), `[Logger]` (level, sinks, rotation,
async writes), `[Loop]` (fixed-delta, step cap, frame clamp, FPS cap),
`[Input]` (mouse capture behavior) and `[Camera]` (start pose, movement
speed/boost, mouse sensitivity, pitch limit, clip planes).

## Building

### Prerequisites

1. A C++20 compiler (GCC ≥ 11, Clang ≥ 14, or MSVC ≥ 19.30 / VS2022)
2. CMake ≥ 3.24
3. The [Vulkan SDK](https://vulkan.lunarg.com) — provides the Vulkan loader,
   headers, `glslc`/`glslangValidator`, and the validation layers.
   Make sure `VULKAN_SDK` is set (the SDK installer does this) or that
   `glslc` is on your `PATH`.

On Linux you also need the usual X11/Wayland development packages for GLFW,
e.g. on Debian/Ubuntu:

```bash
sudo apt install libx11-dev libxrandr-dev libxinerama-dev \
                 libxcursor-dev libxi-dev libwayland-dev libxkbcommon-dev
```

### Build & run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/3d-engine        # Windows: build\Debug\3d-engine.exe
ctest --test-dir build --output-on-failure
```

Or with the provided presets (output lands in `build/debug` or `build/release`):

```bash
cmake --preset debug
cmake --build --preset debug
./build/debug/3d-engine
```

GLSL shaders in `shaders/` are compiled to SPIR-V automatically during the
build; the executable finds them regardless of the working directory.
A Debug build enables Vulkan validation layers when they are installed —
watch the console or `logs/` for `[Vulkan validation]` diagnostics while
developing. The mouse is captured at startup for first-person look:
**WASD** move, **Q/E** down/up, **Shift** boost, **Esc** releases the
mouse (press again to quit).

## Project structure

```
.
├── CMakeLists.txt            # Build, dependencies, shader compilation, tests
├── CMakePresets.json         # debug / release presets
├── config/
│   └── engine.ini            # All engine tuning (window, render, logger, loop)
├── shaders/
│   ├── cube.vert             # MVP transform (UBO) + color passthrough
│   └── cube.frag             # Flat per-face color output
├── tests/
│   └── CoreTests.cpp         # Config/logger/timing/loop unit tests (CTest)
└── src/
    ├── main.cpp              # Bootstrap: arguments -> config -> logger -> app
    ├── Core/
    │   ├── Application.h/.cpp  # Composition root, wires loop callbacks
    │   ├── Camera.h/.cpp       # First-person yaw/pitch camera (GLM)
    │   ├── EngineConfig.h/.cpp # INI loading, validation, fallback chain
    │   ├── Logger.h/.cpp       # Levels, console/file/async sinks, macros
    │   ├── Time.h/.cpp         # steady_clock frame timing, delta, FPS
    │   └── GameLoop.h/.cpp     # Fixed/variable loop, FPS cap, statistics
    ├── Platform/
    │   ├── Window.h/.cpp       # GLFW window, surface, resize/input callbacks
    │   └── Input.h/.cpp        # Key/mouse state, transitions, mouse delta
    ├── Geometry/
    │   ├── Vertex.h            # Interleaved position+color vertex layout
    │   └── Cube.h/.cpp         # Colored cube geometry (24 verts, 36 indices)
    └── Graphics/
        ├── VulkanContext.h/.cpp  # Instance, devices, queues, command pool
        ├── VulkanHelpers.h/.cpp  # Buffers/images, one-off transfers, file IO
        ├── Buffer.h/.cpp         # RAII VkBuffer + staging uploads
        ├── Swapchain.h/.cpp      # Swapchain, depth, render pass, framebuffers
        ├── GraphicsPipeline.h/.cpp # Shader modules, pipeline state, UBO layout
        └── Renderer.h/.cpp       # Frames-in-flight, command recording, present
```

Ownership is strictly RAII and ordered by member declaration in
`Application`: everything is destroyed in the exact reverse order of
creation, after the device has been drained, so no GPU object ever outlives
what it depends on.

## Roadmap

- **Phase 1 — First render ✅:** window, Vulkan bootstrap, rotating colored cube
- **Phase 2 — Core infrastructure ✅:** configurable dual-sink async logger,
  robust fixed/variable game loop, data-driven configuration
- **Phase 3, Step 1 — Input & first-person camera ✅:** GLFW-callback input
  system, yaw/pitch FPP camera wired into the UBO update loop
- Later phases build the real engine core on top of this foundation (asset
  management, scene/entity systems, materials, lighting) — editors and visual
  tooling only arrive once the core is complete.
