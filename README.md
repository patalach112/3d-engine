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
- Cursor capture (hidden, unbounded) for mouse look: when
  `captureMouseOnStart` is enabled, `Window` applies
  `GLFW_CURSOR_DISABLED` during window creation, before Vulkan or scene
  setup begins. Capture transitions also clear accumulated cursor samples
  to prevent first-frame jumps. State is cleared on focus loss, and the
  frame protocol is `beginFrame()` → `pollEvents()` → systems query state

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
| C           | Respawn/reset physics cubes |
| Click       | Capture the mouse         |
| Esc         | Release mouse, again to quit |

Movement is integrated with the variable update's delta time (diagonal
movement normalized). Mouse look accumulates GLFW cursor distances in
pixels rather than incorrectly multiplying them by frame time. Orientation
and movement velocity nevertheless use `1-exp(-dt/tau)` exponential
smoothing, so uneven cursor samples and key presses/releases ramp over
the same wall-clock time at any frame rate. Sensitivity, speed, boost,
smoothing time constants, and clip planes are configurable under
`[Camera]`/`[Input]` in `engine.ini`, and the camera's view/projection
matrices feed the existing UBO each frame. Periodic loop statistics also
report camera position/yaw/pitch.

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

### Target hardware profile (i5-3470 / GTX 970)

`src/Core/HardwareProfile.h` is the single source of truth for the
target machine (i5-3470: 4 physical cores, no Hyper-Threading, 32 KiB
L1d + 256 KiB L2 per core, 6 MiB L3, DDR3-1600; GTX 970: 4 GiB VRAM with
a ~3.5 GiB fast segment), and defaults across the engine derive from it:

- **Threads:** automatic JobSystem sizing is `workers + main = physical
  cores` — 3 workers on the target, capped even on Hyper-Threaded hosts
  so the pool never oversubscribes the part. `parallelFor` batches cap
  at ~1024 hot items, roughly one L2-resident working set per batch.
- **Memory:** pool geometry (64 KiB pages = 16 OS pages, power-of-two
  blocks up to one 4 KiB OS page, 64-byte page alignment) and the
  cache-line component layout are asserted against the profile
  constants; ECS iteration streams sequentially through dense pages to
  respect DDR3 bandwidth instead of chasing pointers.
- **GPU:** `Graphics/VramBudget.h` provides CAS-based, over-commit-proof
  accounting for device-local memory, planned at 3072 MiB (inside the
  fast segment, headroom left for driver/compositor) with a 256 MiB
  staging budget — the groundwork for budgeted buffer/descriptor
  allocation in later phases.

A ready-made preset ships in `config/profile-i5-3470-gtx970.ini`
(1080p, 3 workers, profile batching and budgets); load it with
`--config config/profile-i5-3470-gtx970.ini`. It parses with zero
warnings against the config system.

### Frustum culling (`src/Graphics/Frustum.*`, `src/Geometry/Bounds.*`)

View frustum planes are extracted from the view-projection matrix
(Gribb–Hartmann, with Vulkan's [0, w] near plane). Every `MeshComponent`
carries local AABB bounds; per frame they are transformed to world space
(exact 8-corner re-bounding) and tested with the p-vertex method.
Entities outside the frustum never reach the GPU. Toggle with
`[Render] frustumCulling`; the periodic loop stats now report
**drawn vs. culled** counts, pool usage and completed jobs.

### Unit tests

`tests/CoreTests.cpp` (CTest: `engine-core-tests`, 199 checks) covers
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
integrated frames), and hardware-profile checks (worker-count resolution,
batch granularity, config/profile agreement, VRAM budget accounting under
concurrency), and Phase 5 resource checks (stable asset/material ids,
VRAM-budgeted registration and rejection, PSO state deduplication,
descriptor indices flowing into sorted draw items), and Phase 6 HUD/loop
checks (glyph layout vertex counts and NDC bounds, HUD text composition,
and the poll → fixed → update → render callback contract the window
loop runs on) — everything verifiable without a GPU.

## Phase 5 — Asset Management, Shader Pipelines & Materials ✅

### Asset registry (`src/Assets/AssetManager.*`)

Assets are registered once at startup into dense, pre-reserved storage
and referenced everywhere by stable 1-based integer ids (deterministic
for a deterministic registration order — no heap fragmentation from
per-frame lookups, no runtime stalls). Meshes carry geometry + computed
bounds, shaders carry SPIR-V words (registerable from memory or file),
and materials carry a shader pair, a base-color tint, and a
`PipelineStateDesc`. Registration is idempotent by name and
VRAM-disciplined: GPU-bound data first passes a transient staging-budget
check, then reserves its device-local footprint against the GTX 970
`VramBudget` (fast-segment cap) — over-budget registration fails cleanly
with an invalid id instead of over-committing VRAM. Reservations are
released on manager destruction.

### Pipeline state objects & pipeline cache (`src/Graphics/PipelineState.*`, `GraphicsPipeline`)

`PipelineStateDesc` (topology, polygon/cull mode, depth state, blending)
is hashed deterministically (FNV-1a); `PipelineStateCache` interns
states to stable PSO ids so N materials sharing a state cost one real
pipeline. `GraphicsPipeline` is now a PSO manager: it owns a
`VkPipelineCache` (fed to every `vkCreateGraphicsPipelines`), creates
pipelines deterministically per material, deduplicates by
(state, shader pair), and shares one pipeline layout/descriptor layout
across all of them so descriptor bindings survive per-material pipeline
switches.

### Descriptor indexing groundwork & renderer integration

Materials get dense descriptor indices (`materialId - 1`) — the slot
each material will occupy in a descriptor-indexing material array.
`MeshComponent` now references mesh + material by name; `RenderSystem`
resolves them through the `AssetManager` into `DrawItem`s carrying the
stable material id and descriptor index (the deterministic name hash
remains the fallback for asset-less scenes), the `RenderQueue` sorts by
that id as before, and the `Renderer` rebinds pipelines only at material
group boundaries while recording — push constants and the
view/projection UBO flow are unchanged. The demo scene registers a
`default` and a `doubleSided` material and creates both PSOs at startup.

## Phase 6 — Window, Main Loop & FPS HUD ✅

The GLFW window, Vulkan context (validation layers in debug builds,
GTX 970-oriented device selection), and the fixed/variable main loop
(JobSystem → ECS fixed update → camera update → sorted RenderSystem →
Renderer with PSO pipelines from the AssetManager) were built in
Phases 1–5 and are wired as one application. Startup logs the active
hardware profile: worker count, parallel batch size, and VRAM/staging
budgets.

### FPS HUD (`src/Graphics/HudFont.*`, `src/Graphics/HudRenderer.*`)

A zero-overhead bitmap-font overlay in the top-left corner shows live
**FPS**, **frame time (ms)**, **worker threads**, **VRAM used/budget**,
and **drawn/culled** counts:

- `HudFont` is a pure-CPU 5×7 bitmap font (A–Z, digits, punctuation)
  that lays text out as colored triangles in NDC — no font textures, no
  descriptor sets, fully unit-testable without a GPU.
- `HudRenderer` owns a dedicated overlay pipeline (no depth test, alpha
  blending, `hud.vert`/`hud.frag`) and a persistently mapped vertex
  buffer. Text is re-laid-out only when it changes (refreshed every
  `[Render] hudUpdateIntervalSeconds`, default 0.25 s) or the viewport
  resizes; per frame it costs exactly one pipeline bind + one draw,
  recorded last inside the scene's render pass.
- Toggle with `[Render] hudEnabled`.

## Phase 7 — Custom Physics & Rigid-Body Collisions ✅

### Components (`src/Physics/PhysicsComponents.h`)

Physics is ECS-native, not an external object hierarchy:

- `RigidBodyComponent` (dense 80-byte value): linear velocity, angular
  velocity (degrees/second), last-step acceleration, accumulated force
  (cleared after integration), mass/inverse-mass, restitution, friction,
  static and gravity flags.
- `ColliderComponent` (dense 48-byte value): box (half-extents), sphere
  (radius), or plane (`normal·x = offset`). Box/sphere dimensions are
  local and scaled by `TransformComponent::scale`.
- `PhysicsSettings`: gravity, damping, restitution velocity threshold,
  contact slop, position-correction factor, solver iterations, and the
  parallel batching threshold — all configurable under `[Physics]`.

Scene storage reservation now includes rigid bodies and colliders, so
their EnTT dense paged arrays are preallocated with the other hot ECS data.

### PhysicsSystem (`src/Scene/Systems/PhysicsSystem.*`)

Runs only in the fixed timestep. Each step snapshots
`Transform + RigidBody + Collider` entities into a flat body list, then:

1. integrates gravity/forces with semi-implicit Euler in parallel,
2. computes world collider geometry in parallel,
3. runs an X-axis sort-and-sweep broad phase (planes handled separately),
4. evaluates box/box, sphere/sphere, box/sphere, and shape/plane narrow
   phase pairs in parallel into disjoint contact slots, and
5. resolves contacts sequentially in deterministic order with normal
   impulses, restitution, Coulomb friction, and penetration correction.

Slow impacts use zero restitution and a small contact slop, so cubes can
settle and stack without micro-bouncing. Scratch vectors retain capacity
between steps, and `parallelFor()` barriers separate every parallel phase
from sequential solver work.

### Demo scene

The startup scene now has a static floor (flattened cube + plane collider
at `y = -1.25`) and six rigid-body cubes that fall, bounce, collide, slide,
and stack in front of the camera. Physics statistics (body/contact counts)
are available from `PhysicsSystem::lastStats()`.

Press **C** to respawn the cubes. After scene construction,
`PhysicsSystem::captureSpawnStates()` stores each dynamic body's entity
handle, staggered spawn position/rotation/scale, and initial linear and
angular velocity. A C-key edge press restores those values, clears
acceleration, accumulated forces, and stale contact scratch, and restores
any `SpinComponent` velocity — without creating/destroying entities or
reallocating ECS storage, so repeated resets are stable and leak-free.

## Configuration

`config/engine.ini` is copied next to the executable at build time and can be
edited without rebuilding. Lookup order:

1. `--config <path>` command-line argument (or `--config=<path>`)
2. `ENGINE_CONFIG_PATH` environment variable
3. `config/engine.ini` relative to the working directory
4. The copy in the source tree (development fallback)
5. Built-in defaults

Invalid or unknown keys never abort startup: they fall back to defaults and
are reported as warnings. Sections: `[Window]`, `[Render]`, `[Logger]`
(level, sinks, rotation, async writes), `[Loop]` (fixed-delta, step cap,
frame clamp, FPS cap), `[Input]` (mouse capture behavior), `[Camera]`
(start pose, movement speed/boost, mouse sensitivity, pitch limit, clip
planes), and `[Physics]` (gravity, solver iterations, contact slop,
resting-impact threshold, and parallel-body threshold).

Presentation is controlled in `[Render]`: `presentMode` accepts `auto`,
`fifo`, `mailbox`, `immediate`, and `fifo_relaxed`; an explicit mode
overrides `vsync`. In `auto`, `vsync = true` selects FIFO while
`vsync = false` prefers IMMEDIATE then MAILBOX. The shipped default is
MAILBOX. Mailbox/Immediate also use one frame in flight instead of two,
so input sampled for a frame is not queued behind an extra CPU frame;
FIFO retains two frames in flight for throughput. The startup log prints
the requested mode, the mode actually selected after surface fallback,
and the frame-in-flight count. FPS is measured from actually presented
frames over a 0.25-second window: swapchain-rebuilding/skipped iterations
and resize hitches reset the window instead of creating false spikes.

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
    │   ├── HardwareProfile.h   # i5-3470/GTX 970 target constants + tuning policy
    │   ├── Jobs/JobSystem.*    # Work-stealing thread pool, parallelFor
    │   ├── Memory/PoolAllocator.* # Paginated pool allocator for the ECS
    │   ├── EngineConfig.h/.cpp # INI loading, validation, fallback chain
    │   ├── Logger.h/.cpp       # Levels, console/file/async sinks, macros
    │   ├── Time.h/.cpp         # steady_clock frame timing, delta, FPS
    │   └── GameLoop.h/.cpp     # Fixed/variable loop, FPS cap, statistics
    ├── Platform/
    │   ├── Window.h/.cpp       # GLFW window, surface, resize/input callbacks
    │   └── Input.h/.cpp        # Key/mouse state, transitions, mouse delta
    ├── Assets/
    │   └── AssetManager.h/.cpp # Mesh/shader/material registry, VRAM-budgeted
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
        ├── VramBudget.h          # CAS-based VRAM accounting policy (GTX 970)
        ├── PipelineState.h/.cpp  # PSO descriptions, hashing, state cache
        ├── HudFont.h/.cpp        # 5x7 bitmap font + HUD text layout (pure CPU)
        ├── HudRenderer.h/.cpp    # Screen-space FPS HUD overlay pipeline
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
  buffers on the job-system and async-logger hot paths, a sorted
  CPU-side render queue (material grouping + front-to-back Early-Z), and
  i5-3470/GTX 970 hardware-profile tuning (core-exact thread pool, L2
  batching, VRAM budgeting, preset config)
- **Phase 5 — Assets, pipelines & materials ✅:** stable-id asset
  registry with VRAM-budgeted registration, deterministic PSO manager
  over a VkPipelineCache, dense descriptor indices, and material-grouped
  pipeline binding in the sorted render queue
- **Phase 6 — Window, loop & HUD ✅:** integrated GLFW/Vulkan application
  loop on the hardware profile, with a zero-overhead bitmap-font FPS
  HUD (FPS, frame time, workers, VRAM, draw stats)
- Later phases build the real engine core on top of this foundation (asset
  management, scene/entity systems, materials, lighting) — editors and visual
  tooling only arrive once the core is complete.
