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
| ECS       | EnTT (header-only, fetched automatically) |
| Build     | CMake ≥ 3.24 (GLFW, GLM and EnTT are fetched automatically) |

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

## Phase 3, Step 2 — ECS Architecture (EnTT) ✅

The engine is now entity-component-system driven. Game state lives in a
`Scene` (RAII owner of an `entt::registry`); hardcoded per-object logic in
the application is gone.

- **Scene / Entity** (`src/Scene/Scene.*`): entity creation/destruction with
  lifetime tracking, fluent component handles (`addComponent<T>` /
  `getComponent<T>` / `hasComponent<T>`), tag names, and one shared definition
  of the *active camera*. The Scene knows nothing about windows or Vulkan.
- **Components** (`src/Scene/Components.h`): `TagComponent`,
  `TransformComponent` (position / Euler rotation / scale → model matrix),
  `CameraComponent` (wraps the Phase 3 camera, `isActive` flag),
  `MeshComponent` (asset-id placeholder until the asset system arrives),
  `SpinComponent` (angular velocity for deterministic spin).
- **Systems** (`src/Scene/Systems/`), executed in a strict order each frame:
  1. `RotationSystem` (fixed update) — integrates spins into transforms
  2. `CameraSystem` (variable update) — input-driven look/movement on the
     active camera
  3. `RenderSystem` (render) — derives view/projection + per-entity model
     matrices from the scene and submits them to the Renderer

  The renderer itself was generalized to match: the uniform buffer now
  carries only view/projection, and each entity's model matrix is delivered
  per draw via **push constants**, so one frame draws any number of
  entities. `RenderSystem::prepareFrame()` isolates the scene→matrices
  step so it is unit-testable without a GPU.

The demo scene is built declaratively in `Application::buildScene()`: a
player camera entity, the original cube at the origin, and two spinning
companion cubes — fly around and watch them rotate at a fixed 60 Hz
regardless of frame rate.

## Phase 4 — Performance & Memory Optimization ✅

### Pooled memory for the ECS (`src/Core/Memory/PoolAllocator.*`)

The scene's EnTT registry no longer allocates component storage from the
global heap. It is instantiated with a custom STL allocator backed by a
**paginated pool**: 64 KiB pages carved into size-classed blocks
(16 B–4 KiB) with intrusive free lists, so high-frequency component
storage is reused predictably and stays cache-friendly. Oversized or
over-aligned requests fall back to aligned allocation, everything is
thread-safe and RAII (pages die with the scene), and usage statistics
(pages, blocks in use/reserved, peaks) feed the loop log. `Scene::
reserveCapacity()` pre-allocates storage for transforms/meshes/cameras/
spins — set `[Performance] entityReserve` to your expected entity count.

### Work-stealing job system (`src/Core/Jobs/JobSystem.*`)

A thread pool using `hardware_concurrency - 1` workers by default
(`[Performance] jobWorkerCount` overrides). Each worker owns a bounded
lock-free ring buffer (see Sub-step 2 below): it pops its own ring first
and steals from other rings when idle, and `parallelFor()` is a
synchronous data-parallel loop whose return acts as a **barrier**: the
calling thread helps execute batches, so it is deadlock-safe even from
workers. Job exceptions are caught, counted and logged; workers drain on
shutdown. Systems use it under a strict thread-safety contract: work is
snapshotted on the main thread, jobs write only to disjoint components,
and no structural registry changes happen concurrently.

- `RotationSystem` integrates spins in parallel during fixed updates
- `RenderSystem` computes model matrices/world bounds in parallel
  (above `[Performance] parallelMinDrawItems`), with deterministic
  sequential compaction afterwards

### Strict data-oriented layout (Phase 4, Sub-step 1)

Hot ECS components are laid out for dense, cache-friendly iteration:

- `TransformComponent` is exactly one 64-byte cache line (`alignas(64)`), so
  parallel fixed updates never false-share between neighboring entities;
  `SpinComponent` is a dense 16-byte value (four per cache line), and
  `MeshComponent` keeps its hot bounds/visibility fields ahead of the cold
  asset-id string. Layout is enforced with compile-time assertions.
- The pool allocator now allocates 64-byte-aligned pages and selects block
  classes by `max(bytes, alignment)`, so pooled storage honors cache-line
  alignment without internal gaps.
- EnTT stores components in dense paged arrays (1024 elements per page, one
  contiguous allocation per page). `Scene/DenseIteration.h` resolves dense
  indices through that page table; `RotationSystem` uses an owning
  `Transform + Spin` group so both storages share an index-aligned dense
  prefix and parallel workers write disjoint elements, while
  `RenderSystem` snapshots contiguous entity handles and evaluates disjoint
  per-entity output slots before deterministic compaction.

### Lock-free queues (Phase 4, Sub-step 2)

`src/Core/Concurrency/LockFreeRingBuffer.h` is a bounded MPMC lock-free
ring buffer (Vyukov sequence-stamp design): producers and consumers claim
slots with CAS on cache-line-separated (`alignas(64)`) enqueue/dequeue
positions and publish/consume cell values with release/acquire ordering —
no mutex anywhere on the push/pop paths, and a failed push to a full ring
never consumes the caller's value. It now backs the hot multithreaded
paths:

- **JobSystem:** each worker's queue is a 1024-slot ring. Submission,
  local pops, and steals are pure atomic operations (round-robin push,
  steal on idle); if every ring is momentarily full, the submitter runs
  the job inline instead of blocking or dropping it. Condition variables
  remain only for idle-worker sleep/wake and the `waitForAll` barrier.
- **Async logger:** the file sink's ingestion queue is the same ring, so
  logging threads publish records without taking a mutex; a full ring
  drops (and counts) records rather than stalling a producer. The logger
  front-end now holds only a shared lock while dispatching, so producers
  are not serialized before reaching the ring.

Stress coverage: an MPMC hammer test (4 producers × 4 consumers, 20k
unique items, each consumed exactly once), 100k sequential wrap-around
laps, 8 concurrent `JobSystem` producers × 2.5k jobs with exactly-once
verification, and 4 threads × 250 async log messages delivered in full —
all also clean under ThreadSanitizer harnesses.

### Render queue & CPU command stream (Phase 4, Sub-step 3)

Draw submission now goes through a CPU-side render queue
(`src/Graphics/RenderQueue.*`) instead of handing the renderer entities
in ECS collection order:

- Each `DrawItem` carries a packed 64-bit sort key — material identity
  (stable FNV-1a of the mesh's asset id until real material slots exist)
  in the high bits, quantized view depth in the low bits. Sorting
  therefore groups identical GPU state together (fewer pipeline/material
  changes) and draws front-to-back inside each group, so the depth
  buffer rejects occluded fragments as early as possible (Early-Z).
- `RenderSystem` computes material/depth during its existing parallel
  evaluation (the same disjoint-slot pass that does frustum culling),
  compacts survivors into the queue in deterministic order, and sorts.
  Push constants, view/projection UBOs, and culling results are
  unchanged downstream — the `Renderer` just records the sorted span.
- Sorting is allocation-free after `reserve()` (items + scratch are
  preallocated, `clear()` retains capacity): a stable LSD radix sort
  sequentially, and for large frames a JobSystem-parallel segmented
  sort + stable merge through `parallelFor`. Toggle with
  `[Render] sortDrawItems`.

### Frustum culling (`src/Graphics/Frustum.*`, `src/Geometry/Bounds.*`)

View frustum planes are extracted from the view-projection matrix
(Gribb–Hartmann, with Vulkan's [0, w] near plane). Every `MeshComponent`
carries local AABB bounds; per frame they are transformed to world space
(exact 8-corner re-bounding) and tested with the p-vertex method.
Entities outside the frustum never reach the GPU. Toggle with
`[Render] frustumCulling`; the periodic loop stats now report
**drawn vs. culled** counts, pool usage and completed jobs.

### Unit tests

`tests/CoreTests.cpp` (CTest: `engine-core-tests`, 143 checks) covers
configuration parsing/fallbacks, log filtering and rotation, fixed-timestep
math, the frame clock, loop dispatch, ECS lifecycle/systems, pool allocator
behavior (block reuse, alignment, fallback), job system correctness
(barriers, exactly-once parallel coverage, exception containment), and
frustum/AABB math, data-oriented layout checks (component size/alignment,
dense-page pointer strides, owning-group pairing with non-spinning decoys, and
5,000 entities × 40 parallel fixed steps), and lock-free stress tests (MPMC
ring hammering, concurrent job producers, concurrent async logging), and
render-queue checks (sort-key ordering, stable ties, 5,000-item parallel
sort integrity/determinism, material grouping with front-to-back order in
integrated frames) — everything verifiable without a GPU.

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
│   └── CoreTests.cpp         # Config/logger/timing/loop/ECS tests (CTest)
└── src/
    ├── main.cpp              # Bootstrap: arguments -> config -> logger -> app
    ├── Core/
    │   ├── Application.h/.cpp  # Composition root, wires loop callbacks
    │   ├── Camera.h/.cpp       # First-person yaw/pitch camera (GLM)
    │   ├── Concurrency/LockFreeRingBuffer.h # Bounded MPMC lock-free ring buffer
    │   ├── Jobs/JobSystem.*    # Work-stealing thread pool, parallelFor
    │   ├── Memory/PoolAllocator.* # Paginated pool allocator for the ECS
    │   ├── EngineConfig.h/.cpp # INI loading, validation, fallback chain
    │   ├── Logger.h/.cpp       # Levels, console/file/async sinks, macros
    │   ├── Time.h/.cpp         # steady_clock frame timing, delta, FPS
    │   └── GameLoop.h/.cpp     # Fixed/variable loop, FPS cap, statistics
    ├── Platform/
    │   ├── Window.h/.cpp       # GLFW window, surface, resize/input callbacks
    │   └── Input.h/.cpp        # Key/mouse state, transitions, mouse delta
    ├── Scene/
    │   ├── Scene.h/.cpp        # RAII ECS world (entt::registry), Entity handle
    │   ├── DenseIteration.h    # Dense-index access into EnTT's paged storage
    │   ├── Components.h/.cpp   # Tag/Transform/Camera/Mesh/Spin components
    │   └── Systems/
    │       ├── CameraSystem.*  # Input-driven active camera (variable update)
    │       ├── RotationSystem.*# Spin integration (fixed update)
    │       └── RenderSystem.*  # Scene -> view/projection + draw items
    ├── Geometry/
    │   ├── Vertex.h            # Interleaved position+color vertex layout
    │   ├── Bounds.h/.cpp       # AABB + world-space transformation
    │   └── Cube.h/.cpp         # Colored cube geometry (24 verts, 36 indices)
    └── Graphics/
        ├── VulkanContext.h/.cpp  # Instance, devices, queues, command pool
        ├── VulkanHelpers.h/.cpp  # Buffers/images, one-off transfers, file IO
        ├── Buffer.h/.cpp         # RAII VkBuffer + staging uploads
        ├── Swapchain.h/.cpp      # Swapchain, depth, render pass, framebuffers
        ├── GraphicsPipeline.h/.cpp # Shader modules, pipeline state, UBO layout
        ├── Frustum.h/.cpp        # View frustum extraction + AABB/sphere tests
        ├── RenderQueue.h/.cpp    # CPU render command stream + parallel sort
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
- **Phase 3, Step 2 — ECS architecture ✅:** EnTT-backed Scene/Entity,
  foundation components, camera/rotation/render systems, multi-entity
  rendering via push constants
- **Phase 4 — Performance & memory ✅:** paginated pool allocator for ECS
  storage, work-stealing job system with parallel systems, frustum culling,
  strict data-oriented component layout (cache-line transforms, dense
  paged storage, owning-group parallel iteration), lock-free ring
  buffers on the job-system and async-logger hot paths, and a sorted
  CPU-side render queue (material grouping + front-to-back Early-Z)
- Later phases build the real engine core on top of this foundation (asset
  management, scene/entity systems, materials, lighting) — editors and visual
  tooling only arrive once the core is complete.
