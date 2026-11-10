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
- A unit cube (24 vertices / 36 indices) with exact outward face normals
  and one uniform material tint, stored in device-local vertex/index
  buffers uploaded through staging buffers
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

`tests/CoreTests.cpp` (CTest: `engine-core-tests`, 335 checks) covers
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
loop runs on), and Phase 8 checks (shared-module link/export surface,
uniform material color and normals, solid depth/cull pipeline defaults,
and capsule-player landing, cube pushing, wall blocking, and sliding), and
Phase 9 checks (sandbox palette separation, per-material specular/rim
parameters, 112-byte push-constant layout, packed lighting in draw items),
and Phase 10 checks (shadow light-frustum math, the clockwise-winding /
depth contract, and 1:1 quaternion-to-render-matrix sync over a tumbling
body) — everything verifiable without a GPU.

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

## Phase 7 — Production-Grade Physics & Rigid-Body Simulation ✅

### Components (`src/Physics/PhysicsComponents.h`)

Physics is ECS-native and data-oriented, not an external object hierarchy:

- `RigidBodyComponent` is a dense, 192-byte value containing linear and
  angular velocity, world-space angular momentum, acceleration, accumulated
  force/torque, an authoritative quaternion orientation, full 3×3 local
  inertia and inverse-inertia tensors, mass/inverse-mass, restitution,
  scalar plus anisotropic friction, gravity scale, and static/gravity flags.
  Automatic inertia is derived from the collider (box/OBB, sphere, capsule,
  or convex-hull bounds); explicit principal/full tensors can replace it.
- `ColliderComponent` is a dense, 160-byte value supporting **Box, Sphere,
  Plane, OBB, Capsule, and ConvexHull** (up to eight hull points). Local
  dimensions and the local `centerOffset` are scaled by
  `TransformComponent::scale`; the derived world geometry carries oriented
  axes, swept bounds, capsule axes, and hull points.
- `GravityFieldComponent` attaches a localized radial field
  (`mu = G*M`, optional finite radius with smooth falloff) to any entity
  with a transform — planets do not need rigid bodies. `PhysicsSettings`
  also supports a global point-mass field with softening.
- `JointComponent` supports **Distance, Hinge, and SpringDamper**
  constraints between two bodies or a body and a world anchor. A negative
  rest length captures the current anchor distance.
- `ThrusterComponent` applies a local force at a local offset point (plus
  optional local torque), so offset propulsion naturally spins a craft.
- `PhysicsSettings` covers uniform/zero gravity, global radial gravity,
  linear damping, restitution threshold, contact slop, position correction,
  solver iterations, parallel thresholds, CCD, and the demo mode — all
  configurable under `[Physics]`.

Scene storage reservation includes rigid bodies, colliders, gravity fields,
joints, and thrusters, so their EnTT dense paged arrays are preallocated
with the other hot ECS data.

### PhysicsSystem (`src/Scene/Systems/PhysicsSystem.*`)

The solver runs only in the fixed timestep. Each step snapshots bodies,
gravity fields, joints, and thrusters into flat arrays, then:

1. applies thruster and spring-damper forces,
2. integrates forces with semi-implicit Euler in parallel — linear motion
   plus quaternion rotational dynamics. Angular momentum is the conserved
   world quantity, `omega = I_world^-1 * L` is re-derived each step from
   the rotated inverse-inertia tensor, so asymmetric bodies exhibit
   torque-free gyroscopic precession while momentum is conserved,
3. builds world collider geometry in parallel,
4. runs an X-axis sort-and-sweep broad phase (planes paired separately),
   with swept expansion so fast bodies stay candidates against obstacles
   they crossed during the step,
5. evaluates narrow phase in parallel into disjoint contact slots,
   including SAT OBB/OBB, capsule/sphere, hull support geometry, and
   **swept CCD**: fast spheres are ray-cast against planes and expanded
   box slabs, bodies are rolled back to the time of impact, and approach
   velocity is cancelled so projectiles cannot tunnel through thin walls,
6. builds solver **islands** from the contact/joint graph and solves each
   island with accumulated Sequential Impulses — restitution targets,
   non-negative normal-impulse clamping, anisotropic Coulomb friction in
   two tangent directions, positional correction, and **warm starting**
   from the previous step's impulses. Islands are disjoint by
   construction, so they can solve in parallel without races while the
   result stays deterministic across worker counts.

Slow impacts use zero restitution and a small contact slop, so stacks
settle without micro-bouncing. Scratch vectors retain capacity between
steps, and `parallelFor()` barriers separate every parallel phase from
solver work. Statistics (bodies, candidate pairs, contacts, islands,
largest island, joints) are available from `PhysicsSystem::lastStats()`.

### Demo scenes (`[Physics] demoMode`)

- **Terrestrial** (default): a static floor plus six falling cubes and a
  twelve-cube stacking stress scene that settles under gravity.
- **Space** (`demoMode = space`): zero uniform gravity with a localized
  radial-gravity planet, an orbiting capsule craft at circular-orbit
  velocity, an offset-thruster drone, distance-jointed cargo, a
  spring-anchored craft, and a 35 m/s projectile fired at a thin OBB wall
  to demonstrate CCD.

Press **C** to respawn the dynamic bodies. After scene construction,
`PhysicsSystem::captureSpawnStates()` stores each body's entity handle,
spawn transform, and initial linear/angular velocity, angular momentum,
and orientation. A C-key edge press restores those values, clears
acceleration, accumulated forces/torques, and stale contact/warm-start
scratch, and restores any `SpinComponent` velocity — without
creating/destroying entities or reallocating ECS storage, so repeated
resets are stable and leak-free.

## Phase 8 — Dynamic Modules, Physical Player & Solid Materials ✅

### Shared-library module architecture (`CMakeLists.txt`, `src/Core/Export.h`)

The engine no longer builds as one monolithic executable target. CMake
builds layered shared libraries — `.dll` on Windows, `.so`/`.dylib`
elsewhere — and the executable is a thin host containing only `main()`:

| Module | Contents |
|---|---|
| `EngineFoundation` | Logger, time, pool allocator, job system |
| `EnginePlatform` | GLFW window and input |
| `EngineCore` | Camera, game loop, configuration |
| `EngineGeometry` | Bounds and cube geometry |
| `EngineGraphics` | Vulkan renderer, pipelines, HUD, render queue |
| `EngineAssets` | Mesh/shader/material registry |
| `EngineScene` | ECS scene and camera/physics/player/render systems |
| `EngineApp` | Application composition root |

Public classes and functions carry per-module `ENGINE_*_API` annotations
(`__declspec(dllexport/dllimport)` on Windows, visibility attributes on
ELF/Mach-O). Each module privately defines its `ENGINE_BUILDING_<MODULE>`
macro; `WINDOWS_EXPORT_ALL_SYMBOLS` remains a safety net for templates and
auxiliary symbols. `ENGINE_BUILD_SHARED` defaults to `ON`; set it to `OFF`
for a static build (CMake then defines `ENGINE_STATIC`, so no DLL import
annotations leak into static consumers). On Windows, runtime DLLs are
copied next to the executable and test binary automatically. The test
executable links every module directly, so a missing public export fails
at link time.

### Physical first-person player (`PlayerSystem`, `PhysicsSystem`)

The active camera entity is now a real simulated character, not a free-fly
camera that happens to render the scene:

- It owns a `TransformComponent`, an 80 kg dynamic `RigidBodyComponent`,
  and a capsule `ColliderComponent` (radius 0.30 m, segment half-height
  0.45 m, total height 1.50 m). The camera eye sits 0.50 m above the
  capsule center, inside the capsule; with the default 0.1 m near plane,
  the view plane stays within the capsule's 0.30 m clearance — the view
  cannot enter geometry the capsule cannot.
- Fixed-update order is `PlayerSystem` (input → smoothed rigid-body
  velocity) → `PhysicsSystem` (gravity, contacts, solver) →
  `PlayerSystem::syncCamera` (capsule center + eye offset) →
  `RotationSystem`. `CameraSystem` still handles mouse look, but for a
  physical player it never teleports the camera; non-physical cameras
  keep the previous free-fly behavior.
- Capsule collision is exact rather than bounding-sphere approximate:
  capsule/capsule uses closest points between the two segments, and
  capsule/box (including rotated OBBs) transforms the capsule segment
  into box-local space and measures the true segment-to-box distance, so
  contact starts precisely at a face, edge, or corner. The player therefore
  lands on the floor, is stopped by walls, slides along them, and pushes
  dynamic cubes with rigid-body impulses. The demo floor's collider is a
  box matching its rendered 9 x 0.1 x 9 slab exactly.

### Uniform solid materials (`Vertex`, `Cube`, shaders, push constants)

- Each sandbox material is a solid color from `MaterialDesc::baseColor`;
  geometry carries a single white tint. The old per-face debug colors are
  gone — visible face variation comes only from lighting. Phase 9 gives
  the floor, falling cubes, stacking cubes, environment, and player
  accent distinct materials (see the section below).
- `Vertex` is position + normal + color (36 bytes). The vertex shader
  transforms normals with the inverse-transpose model matrix, and the
  fragment shader applies ambient + diffuse solid-material shading.
- Each draw pushes a `DrawPushConstants` payload (model matrix + material
  color/lighting), inside Vulkan's guaranteed 128-byte push-constant
  minimum.
- The default pipeline state is a solid material: depth test and depth
  write enabled, `Less` depth comparison, back-face culling with
  clockwise front faces (compensating GLM's Y-flip), and no blending —
  nearer geometry occludes farther geometry and neither light nor color
  bleeds through solid surfaces.

## Phase 9 — Distinct Materials & Directional Lighting ✅

The sandbox no longer renders as one flat gray-blue mass. Every scene
category owns a solid material with its own base color and lighting
response (`src/Assets/AssetManager.*`, `makeSandbox*Material` factories):

| Material | Used for | Base color | Character |
|---|---|---|---|
| `floor` | terrestrial floor slab | dark asphalt `(0.10, 0.115, 0.135)` | matte, weak specular, almost no rim |
| `cube` | falling dynamic cubes | warm copper `(0.84, 0.43, 0.18)` | strong warm specular, high shininess |
| `stackCube` | stacking stress cubes | teal `(0.08, 0.52, 0.58)` | cool specular, stronger rim |
| `environment` | planet, CCD wall, double-sided surfaces | blue-violet `(0.27, 0.33, 0.58)` | moderate cool specular/rim |
| `player` | orbital craft, fast projectile | mint green `(0.16, 0.76, 0.46)` | strongest rim in the palette |

`MaterialDesc::lighting` (`MaterialLighting`) carries specular color and
strength, shininess, rim strength/power, and ambient strength.
`Application::buildScene` assigns materials per object category; the
first-person player itself stays a camera + physical capsule (it has no
rendered mesh), and `player` is the accent for player-adjacent craft in
the zero-G scene.

### Shading model (`shaders/cube.vert`, `shaders/cube.frag`)

The fragment shader now evaluates a directional lighting model per pixel:

- **Ambient** — per-material strength, so shadowed faces keep material
  identity instead of collapsing to one flat tone.
- **Lambert diffuse** — against a fixed warm key light using correct
  world-space normals (inverse-transpose model matrix, verified exact in
  tests), giving each face a clean brightness gradient so edges and
  stacking corners read immediately.
- **Blinn-Phong specular** — per-material color, strength, and shininess;
  the camera position is recovered in the vertex shader from the view
  matrix, so specular highlights cost no extra uniform.
- **Rim lighting** — a view-dependent Fresnel-style term (strength/power
  per material) that separates silhouettes from same-depth backgrounds
  without transparent halos.

Back faces flip their normal (`gl_FrontFacing`), so double-sided
materials light both sides correctly. The pipeline state is unchanged
and still solid: depth test + depth write with `Less` comparison,
back-face culling, no blending — lighting and color cannot bleed through
opaque geometry.

### Push-constant payload

`DrawPushConstants` grew deliberately from 80 to **112 bytes**: model
matrix (64), base color (16), specular color + shininess (16), and a
surface vec4 — specular strength, rim strength, rim power, ambient
strength (16). Layout offsets (0/64/80/96) are pinned by `static_assert`s
and cross-checked against the SPIR-V block with `glslangValidator`. At
112 bytes the payload stays inside Vulkan's guaranteed 128-byte
push-constant minimum, so richer per-material lighting needs no
descriptor updates and no extra draw calls; material palette entries with
identical pipeline state still deduplicate to a single `VkPipeline`.

## Phase 10 — Shadow Mapping, Depth Integrity & Rotation Sync ✅

### Directional shadow mapping

A real shadow pass now grounds every object. Before the main pass, the
renderer draws all draw items a second time from the key light's point
of view into a 2048×2048 `D32_SFLOAT` depth texture (depth-only render
pass; artifacts created in `Renderer::createShadowResources`):

- The light uses a fixed orthographic frustum
  (`Graphics/ShadowMap.*`, `directionalLightViewProjection`) aimed at the
  sandbox center, wide enough to cover the floor and the zero-G cluster;
  its matrix and direction travel in the per-frame UBO.
- The shadow pipeline (`shadow.vert`, front-face culling + constant/slope
  depth bias) stores back-face depth, the standard acne-resistant setup.
- `cube.frag` reprojects each fragment into light space and applies a
  3×3 PCF comparison with a slope-scaled bias and a small normal offset;
  the shadow factor attenuates the diffuse and specular terms only, so
  shadowed surfaces keep their material's ambient identity. Fragments
  outside the light frustum are always lit.

### Depth, culling and the see-through investigation

The depth contract was audited end to end and is now pinned by CPU tests
that reproduce the rasterizer's math:

- The swapchain render pass clears depth to 1.0 and the graphics
  pipeline enforces depth test + depth write with
  `VK_COMPARE_OP_LESS`; materials are opaque (blending off), so nearer
  geometry always wins and nothing blends through.
- Cube triangles are wound counter-clockwise in model space; the camera
  projection's Y-flip makes camera-facing triangles **clockwise** in
  Vulkan NDC, which is exactly what `VK_FRONT_FACE_CLOCKWISE` +
  back-face culling keeps. A unit test projects all 12 cube triangles
  through the real projection/view math and asserts this sign split, so
  the winding convention cannot silently regress into inside-out
  rendering while rotating.
- The previous see-through/floating appearance traced to missing
  contact grounding (now fixed by shadow mapping) and the orientation
  mismatch below, not to depth state.

### 1:1 rigid-body rotation sync

Physics integrates orientation as a quaternion, but rendering used to
rebuild the model matrix from Euler angles extracted from that
quaternion every step — a round-trip that jitters near gimbal lock and
let the rendered mesh disagree with its collider. Now:

- `TransformComponent` carries the authoritative `glm::quat orientation`
  (stored in the cache line's tail padding; still exactly 64 bytes).
- `PhysicsSystem` publishes the integrated quaternion directly via
  `setOrientation()`, and `modelMatrix()` builds the render matrix with
  `glm::mat4_cast` — bit-for-bit the simulated rotation (the q / −q
  double cover renders identically). Euler angles remain as a derived
  view for spawn capture and tooling, and `RotationSystem` keeps the
  quaternion in lockstep for Euler-authored spin.
- The spawn/reset path restores the quaternion too. Tests verify the
  render matrix matches `mat3_cast(body.orientation)` after every one
  of 240 tumbling steps at high angular velocity.

## Phase 11 — Sub-Stepped Physics, Contact Manifolds & Sleeping ✅

The physics upgrade targets mechanical realism and removes the classic
player-jump, tunneling and stacking glitches by changing *where* contact
impulses act, not just how hard they push:

### Internal sub-stepping

`PhysicsSystem::update()` now splits every fixed step into
`subSteps` (default 4, config `subSteps =`) full simulation sub-steps —
snapshot, force capture, integration, broad/narrow phase, islands and
solving all re-run per sub-step and the world geometry (including swept
CCD bounds) is re-derived each time. Fast capsules and projectiles are
sampled four times as often, so a 120 m/s body crossing a thin floor is
caught discretely even with CCD disabled (verified by an A/B test that
also keeps a CCD-only control case). Gameplay forces are captured once
and act over the whole fixed step; thruster/spring constraint forces
re-apply per sub-step, so nothing is silently divided by the sub-step
count.

### Multi-point contact manifolds

- Box–box: corners of each box inside the other (with a directional
  pass-through rule for corners that crossed a thin body) form up to
  four manifold points, reduced by farthest-point sampling so a face
  contact keeps its corners spread — never clustered on one edge.
- Box–plane: all supporting corners touch (a unit cube reports 4
  contacts, not 1).
- Capsule–box: the exact segment-to-box closest pair plus segment
  endpoints inside the box; generic pairs use a contact point whose
  depth is the support-face midpoint and whose tangential position is
  anchored between the body centers, so a big floor's far support
  corner can never fabricate a metre-long lever arm.

### Solver: point impulses without the manifold ratchet

Contacts now carry true world points and impulses act at the point —
`v += J/m`, `L += r × J` — with angular effective mass, so off-center
hits spin bodies correctly and mass ratios are honoured exactly
(1 kg vs 100 kg exchanges measured momentum to machine precision).
Sequential per-corner impulses on a centered face, however, ratchet:
asymmetric shares feed back through friction and pitch/yaw a pushed or
resting cube over time. The velocity solve therefore reduces every
manifold to its **centroid lever arm** — a face contact acts under the
center of mass (zero spurious torque: a 0.5-restitution floor bounce
returns exactly vy = 2.0 with zero residual spin), while an edge
manifold's genuinely offset centroid still tips the body as it should.
Per-point penetration depths are retained for the positional pass.

### Non-linear position correction

The positional pass iterates (`positionIterations`, default 2),
re-measuring penetration from the mutated contact after every
correction and clamping each iteration's separation gain to
`maxPositionCorrection` (0.02 m; see the stacking stability fix below), split across manifold points so a
4-point face does not correct four times as hard as a single point.
Deep jump landings resolve firmly over a few passes instead of one
explosive pop, and the player capsule lands from a jump at rest height
with zero sink and zero residual velocity.

### Friction & sleeping

Anisotropic Coulomb friction (two tangent coefficients per body, blended
pair-wise) is unchanged in form but now acts at the manifold centroid,
giving the player capsule smooth wall/box-top sliding without sticking.
Resting bodies fall asleep: below 0.05 m/s and 3°/s *with a supporting
contact from the previous sub-step* (unsupported slow bodies in zero-G
never freeze) for 0.5 s — the timer decays rather than hard-resetting
on jitter bursts — the body is skipped by integration and treated as
immovable by the solver until a force, torque, impact or lost support
wakes it. The demo player is exempt (`allowSleep = false`).
`PhysicsStats` reports sleeping bodies and the sub-step count, and
candidate/contact counts are the peak across sub-steps so a mid-step
bounce is never invisible. Tests cover sub-step configuration and
clamping, the 120 m/s anti-tunneling A/B, sleeping stacks waking under
a dropped heavy ball, momentum exchange, penetration clamping and the
player jump landing: 350 checks, all passing.

## Phase 12 — Automated 3D Asset Import, Auto-Scale & 1:1 Hitboxes ✅

External models now enter the engine through one automated path:
`AssetManager::loadObj` → `MeshAsset` (with `MeshBounds`) →
`Scene::spawnImportedModel` → a renderable, physical entity whose
hitbox is derived from the same numbers as its visuals.

### OBJ import (`AssetManager::loadObj`)

A strict Wavefront OBJ importer (no third-party dependency) parses
`v`/`vt`/`vn` records and fan-triangulates polygon faces into the
registry's indexed vertex/index buffers. It accepts all four face
forms (`v`, `v/vt`, `v//vn`, `v/vt/vn`), resolves negative relative
indices, shares duplicate attribute tuples, generates flat Newell
normals for faces that carry none, and rejects malformed files
(bad numbers, out-of-range indices, no faces) without registering a
half-parsed mesh. Imported UVs land in the interleaved `Vertex`
format's new `texCoord` attribute (location 3), so geometry is
texture-ready; the built-in cube carries matching per-face UVs.

### AABB bounds & auto-scale normalization

Every registered mesh — imported or programmatic — gets a
`MeshBounds` computed once from its raw vertices: AABB `min`/`max`,
`center`, half-`extents`, full `size`, and
`autoScaleFactor = 1 / max(size)`. The longest raw dimension therefore
normalizes to 1.0 whether the source file was authored in
millimetres, inches or metres; raw vertices are never rescaled, so
the registry keeps one true copy of the file geometry.

### 1:1 hitbox spawning (`Scene::spawnImportedModel`)

Spawning wires the whole entity from those bounds:

- `TransformComponent.scale = userScale * bounds.autoScaleFactor`
- `MeshComponent` references the asset and adopts its raw AABB as
  local culling bounds
- `RigidBodyComponent` carries the requested mass
- `ColliderComponent::box(bounds.extents, bounds.center)` — half
  extents and center offset straight from the AABB

Physics honours the new `ColliderComponent::centerOffset` when it
derives world geometry (scaled, then rotated into world space) and
adds the parallel-axis term to automatic box inertia, so the
collision volume sits exactly on the rendered volume even when a
model's origin is far from its geometric center. Because mesh and
collider both consume the raw bounds through the same transform
scale, the rendered world AABB and the physics world box are equal
by construction at any user scale — verified numerically in tests,
including a landing test where an off-center imported crate comes to
rest with its *visual* bottom exactly on the floor. Phase 12 adds 23
checks (373 total, all passing).

## Stacking Stability Fix — No More Jitter, Bouncing or Chain-Launches ✅

After Phase 12, multi-level stacks of the demo cubes (0.5 m cubes,
restitution 0.38, each level offset 0.03 m) never settled: they rocked,
vibrated and eventually threw cubes at several m/s. Instrumented
tracing isolated three distinct solver defects, each fixed at its root:

- **Warm-start identity collision.** After the manifold-centroid
  reduction every contact of a pair shares one point, and warm starting
  matched stored impulses by that shared point — so *each* manifold
  point re-applied the *first* point's impulse share every sub-step,
  injecting energy and torque into the stack. Contacts now carry a
  stable pre-centroid `featurePoint` (the corner/endpoint that
  generated the point) and warm-start prepare/store match on it.
- **Restitution chain reaction.** A resting contact that warm-started
  from the previous sub-step still aimed at
  `restitution × approach speed` captured at first impact; solved
  against an immovable lower neighbour, that target pumped the upper
  body upward iteration after iteration (+0.10 → +0.31 m/s in one
  sub-step), and the wave amplified level by level. Restitution now
  fires only on a *new* (non-persistent) impact above
  `restitutionVelocityThreshold` (1 m/s) — below the threshold, or on
  any persistent contact, the restitution target is exactly zero.
- **No pressure-shift righting moment.** A leaning cube's contact
  patch was solved at the unweighted face centroid, so nothing pushed
  back against tilt. Box–box manifold points now carry true per-point
  penetration depths, and the velocity-solve centroid is
  penetration-weighted: the deeper (loaded) corner pulls the force
  line toward itself, producing the restoring moment a real contact
  patch produces. Per-point depths also drive the positional pass.

Supporting tuning, all in `[Physics]`:

- **Resting-contact damping** (`contactLinearDamping = 20`,
  `contactAngularDamping = 40`, 1/s): bodies resting on persistent,
  weight-bearing contacts at residual speeds (< 0.3 m/s, < 90°/s)
  bleed energy every sub-step and snap to exact zero below
  0.01 m/s / 1°/s. Force-driven bodies and the player
  (`allowSleep = false`) are never damped, and fresh impacts are
  untouched, so momentum conservation stays exact.
- **Relaxed, clamped position correction**: gain 0.8 → 0.35 per pass,
  per-pass clamp `maxPositionCorrection = 0.02` m, slop
  0.001 → 0.003 m — resting manifolds converge instead of popping
  apart and re-impacting.
- **Tighter sleep, harder wake discipline**: sleep thresholds
  0.05 m/s and 3°/s (after 0.5 s under them); penetration alone no
  longer wakes a sleeper — a real approach speed
  (> `wakeSpeedThreshold`, or deep penetration with approach) is
  required, so positional correction can't jitter a stack awake.

Verification: the demo's 12-cube offset stack (and 6–8 cube towers
spawned at rest) reach **absolute rest — asleep, velocities exactly
zero — in under a second**, with regression tests pinning the offset
stack, the sub-threshold no-bounce landing, and the new config keys:
386 checks, all passing in Debug, Release-shared and Release-static.

## Player–Rigid-Body Interaction Fix — No More Trampolines or Ejected Stacks ✅

The 80 kg player capsule (640:1 against the 0.125 kg demo cubes) fed
back through the resting-contact chain: dropped onto a sleeping cube
it sank ~0.2 m in, then rebounded at nearly its impact speed and
bounced forever; dropped onto the sleeping stack it ejected cubes at
~2× its impact speed with violent spin. Tracing one impact end to
end isolated the loop in the solver's mass-ratio behaviour:

- **Gauss-Seidel cannot cross a 640:1 chain in budget.** Each sweep
  through a floor-pinned cube transfers only
  ~invM_heavy/invM_light of the capsule's approach, so at
  4 iterations × 4 sub-steps the capsule tunneled through the cube
  before converging, while the accumulated impulse grew to the
  chain-consistent value (~35 N·s) across many sub-steps.
- **Warm-start redelivery then became the pump.** That impulse —
  legitimately accumulated while the chain absorbed the impact —
  was re-fired in full every following sub-step (+J/m long after
  the approach was consumed), and neither the velocity iterations
  (whose effective-mass term assumes a free partner — 8.0125 for
  the pair vs the floor-pinned 1/80 the chain actually presents)
  nor the saturated positional pass (capsule share ≈ 0.16% through
  the sandwiched cube) could remove or out-correct it.

The fix is **shock propagation** (support-depth-pinned solve
masses), computed per sub-step in `solveConstraints`:

- Every body's **support depth** is the shortest contact chain from
  an immovable body (static or asleep) up to it. Within a contact
  between different depths the shallower body is the support.
- The **positional** pass always pins the support
  (`Contact::solveInvMass*`): the deeper body takes the whole
  correction share, so a heavy body sunk into a light supported one
  climbs back out instead of both saturated corrections cancelling
  through the sandwiched body.
- The **velocity** pass pins the support only at extreme mass
  ratios (≥ 16:1 inverse-mass ratio; `Contact::velInvMass*`), where
  iteration convergence is hopeless. The impulse is then sized by —
  and delivered only to — the heavy body, so the reaction crosses
  the chain in one sweep per level. Comparable-mass contacts
  (stack cubes, free collisions) keep symmetric two-body impulses,
  which resting stacks rely on for torque balance and the momentum
  tests rely on for exact conservation.
- **Warm starting delivers under the same fiction** that produced
  the stored impulse (`applyWarmStart` respects `velInvMass*`): a
  pinned support never received the impulse while it accumulated
  (its own support contact kept it at rest), so it never receives
  the 80 kg-sized impulse on redelivery either. Delivery itself
  stays uncapped — with per-contact response now matching the real
  constraint environment, an overshooting delivery is measured and
  clamped away by the first solver sweep of the same sub-step.
  (Pair-response caps, measured-conductance delivery and per-body
  speed caps were each built and rejected: they either starved
  legitimate chain support or corrupted the accumulated-impulse
  bookkeeping momentum conservation depends on.)

Verification: an 80 kg capsule dropped 1.5–4 m onto a single
sleeping cube comes to rest **on top of it** (cube unsunk, asleep,
zero rebound — combined restitution is 0), and dropped onto the
sleeping 12-cube offset stack leaves every cube at its rest
position, asleep, with peak cube speed/spin of zero measurable
motion; standing on a sleeping cube deviates < 4 mm over 3 s and
jumping off imparts no velocity to it; walking pushes slide cubes
and topple stacks without explosions. Regression tests pin the
single-cube landing and the stack landing alongside the existing
suite: **393 checks, all passing in Debug, Release-shared and
Release-static, CTest 1/1.**

## Resting-Contact Fix — No More Creep, Perpetual Tumble or Spawn Pop ✅

Three related reports traced to one family of solver leaks around
resting contacts:

- **Perpetual tumbling / spawn chaos.** The positional pass used to
  correct penetration by translating centres of mass straight along
  the contact normal. For a rotating body that is an energy pump: a
  tumbling cube's corner digs in a little deeper every sub-step, and
  lifting the centre of mass vertically out of the penetration
  converts solver work into gravitational potential that gravity
  immediately converts back into spin. Measured on a single dropped
  demo cube: spawn spin 24°/s amplified to 400°/s+ and sustained
  indefinitely; in the full demo scene a stack cube was clocked at
  50 m/s (156 J from a scene whose largest drop stores ~25 J) and
  loose cubes were ejected off the floor. The correction is now a
  positional impulse *at the contact point*, distributed between
  translation and rotation through the same point Jacobian the
  velocity solver uses (`invM + n·((I⁻¹(r×n))×r)`), so it mostly
  un-rotates the dig-in that created the penetration instead of
  lifting the body. Face contacts (r×n ≈ 0 at the centroid) reduce
  to the old pure translation, so resting stacks are unaffected.
  The same drop now tumbles briefly, dies within ~2 s and sleeps;
  the demo replica (six falling cubes, 12-cube stack, player) ends
  with every body asleep and nothing ejected.

- **Standing creep.** A capsule standing across the seam between
  two stack cubes drifted sideways ~1 mm/s with steadily growing
  velocity: gravity re-seats it vertically every sub-step and the
  positional pass lifted it back out along the tilted edge-contact
  normal — a sag→lift ratchet. The contact point itself never
  slides (measured point slide ≈ 0; the spherical bottom effectively
  *rolls* in the seam), so velocity friction could never oppose it.
  Two-part fix: for persistent, support-like contacts in the static
  regime (relative tangential point velocity below a 0.05 m/s
  zero-velocity threshold), the positional pass now cancels the
  gravity-lateral part of the relative translation its own
  separation produced — the correction lifts straight out of the
  support, exactly what static friction enforces physically; and
  resting-contact damping now also covers non-sleeping bodies (the
  player) inside that same hard rest band, so the sub-mm/s
  micro-roll residue is damped to exact zero instead of
  accumulating. Driven motion is never bled: the band sits far
  below any commanded speed and force/torque-driven bodies are
  vetoed as before. Standing on a box — centred, off-centre, or
  across a stack seam — now shows literally zero drift and zero
  residual velocity over 10 s.

- **Spawn pop.** Overlapping spawns already separated
  positionally with zero velocity by construction (the velocity
  solve has no penetration bias term); this is now pinned by
  regression tests: a pair spawned with 10 cm overlap separates to
  a resting, sleeping pair with peak speed 0.000 m/s, and a stack
  spawned at exact rest spacing never exceeds 0.04 m/s and is
  fully asleep within half a second.

Verification: fifteen new regression checks (tumbling death,
kicked-cube decay, seam-stand zero drift, press-and-hop settle,
overlap spawn, exact-spawn stack, full demo replica) join the
suite: **408 checks, all passing in Debug, Release-shared and
Release-static, CTest 1/1, zero warnings.**

## Phase 13 — Raycasting & Mouse Picking ✅

World-space ray queries for picking, visibility probes and gameplay:

- **Core math** (`Geometry/Ray.h`, EngineGeometry): `Ray`
  (origin + normalized direction), a slab-method **ray vs AABB**
  test reporting entry distance and face normal (parallel axes and
  origin-inside-box handled exactly, no inverse-direction NaNs),
  a two-sided **Möller–Trumbore ray vs triangle** test reporting
  distance and barycentric coordinates, `rayThroughNdc`
  (unprojection through an inverse view-projection matrix) and
  `screenToNdc` (window pixels → Vulkan-convention NDC, y-down on
  both sides, so no flip is needed against the renderer's
  Y-flipped projection).
- **Camera rays**: `cameraRay(camera, ndc, aspectRatio)` builds the
  picking ray through any screen point with its origin snapped to
  the eye, so distances are measured from the camera.
- **Scene queries** (`Scene::raycast`, `raycastAll`,
  `raycastPrecise`): per entity, the first applicable
  representation is tested — exact per-triangle intersection when
  a `MeshResolver` resolves the entity's `MeshComponent` asset id
  (the mesh surface is then the truth, with the asset's
  `MeshBounds` as broadphase), an exact **oriented-box** test for
  `BoxColliderComponent`/OBB colliders done in collider local
  space (rotated boxes hit through their slanted faces, not their
  world AABB), or the world AABB of a visible mesh's local bounds.
  Sphere/capsule/plane/hull colliders are not raycast yet and fall
  through to their mesh. `RaycastHit` carries entity, distance,
  world position and world normal (oriented against the ray).
- **Mouse picking** (Application): left-click casts a precise ray
  through the cursor (or through the crosshair while the mouse is
  captured), logs the picked entity's name/id/distance/position/
  normal, and highlights it by swapping its material to a dedicated
  hot-orange `pickedHighlight`; the previous selection's material
  is restored, and clicking empty space deselects.

Verification: 41 new checks (NDC conversion, unprojection against
the real projection matrix, AABB face/inside/parallel cases,
triangle barycentrics and back faces, closest/all/max-distance
scene queries, rotated-OBB exactness vs the AABB approximation,
precise-vs-bounds discrimination on a single triangle, and a
camera-ray end-to-end pick) join the suite: **449 checks, all
passing in Debug, Release-shared and Release-static, CTest 1/1,
zero warnings.**

## Phase 14 — Spatial Partitioning (Dynamic AABB Tree / BVH) ✅

One acceleration structure now backs both physics broadphase and
raycast picking (`Scene/DynamicAABBTree.h`, EngineScene):

- **The tree**: a Box2D-style dynamic BVH over **fat AABBs** — every
  leaf stores its entity's world box expanded by a 0.1 margin plus a
  prediction along the last reported displacement (×2). Nodes live
  in an index-addressed pool with a free list, so proxy ids stay
  stable as the pool grows; each node stores its box, child/parent
  indices and subtree height. Insertion descends to the sibling
  minimizing the **surface-area heuristic (SAH)** cost increase, and
  the walk back to the root refits ancestors and applies SAH
  rotations wherever a subtree is unbalanced, keeping the tree at
  O(log N) expected query cost. New `AABB` helpers
  (`surfaceArea`, `contains`, `overlaps`, `combined`, EngineGeometry)
  carry the math.
- **Core operations**: `insertLeaf(tightBox, entity) → ProxyId`,
  `removeLeaf(proxy)`, and `moveLeaf(proxy, box, displacement)`,
  which is a no-op while the tight box *and* its displacement-
  predicted reach stay inside the fat box — small movements never
  touch the tree, only genuine escapes pay a remove+reinsert.
  `validate()` provides an O(N) structural self-check (parent/child
  links, stored heights, internal boxes equal to the union of their
  children, pool accounting) used throughout the tests.
- **Spatial queries**: `query(box, callback)` reports every leaf
  whose fat box overlaps (callback returns false to stop), and
  `raycast(ray, maxDistance, callback)` traverses only subtrees the
  ray pierces; the callback returns the new clip distance, so a
  found hit prunes everything farther away (return 0 to stop).
- **Physics integration**: `PhysicsSystem`'s broadphase keeps a
  persistent tree keyed by entity — bodies are inserted/moved each
  substep and pair candidates are generated by querying each body's
  **swept AABB** (the end box unioned with itself shifted back along
  the step's motion, the same coverage the old X-axis sweep's pad
  provided, now with full 3D rejection). Infinite planes stay out
  of the tree and pair against every body, exactly as before.
- **Picking integration**: `Scene` keeps a second tree over every
  pickable entity's bounds (box/OBB collider world AABB unioned
  with the visible mesh's world AABB — the same candidacy the exact
  tests use). Physics writes transforms directly, so the tree is
  synced at the top of every `raycast`/`raycastAll`/`raycastPrecise`
  (fat boxes make sync mostly a bounds pass over unchanged
  entities; destroyed entities are pruned), and the BVH then runs
  the unchanged exact tests — OBB slab, per-triangle — only on the
  candidates along the ray, with the closest-hit distance fed back
  as the traversal clip.

Verification: 43 new checks (fat-box containment guarantees,
insert/remove/move bookkeeping, query early-exit, monotonic-
insertion and 512-leaf grid balance bounds, a randomized
insert/move/remove stress validated against brute force over the
fat boxes every round, ray traversal set-equality, max-distance
and clip-tightening pruning, cluttered-scene picking parity with
destroy/move tracking, tree broadphase pair exactness including
resting stacks and plane pairing) join the suite: **492 checks,
all passing in Debug, Release-shared and Release-static,
CTest 1/1, zero warnings.**

## Phase 15 — Collision Layers & Masks ✅

Category-based collision filtering, configured per collider:

- **Layer & mask data** (`Physics/PhysicsComponents.h`):
  `ColliderComponent` gains `uint32_t layer` (the single layer the
  collider belongs to) and `uint32_t mask` (the layers it collides
  with), placed in the struct's tail padding so the dense 160-byte
  component size is unchanged. The `CollisionLayer` namespace names
  the common categories — `Default`, `Player`, `Enemy`,
  `Environment`, `Trigger`, plus `None`/`All` bookends — and any of
  the 32 bits can be used. Defaults (`Default` / `All`) reproduce
  the pre-layer behaviour exactly: everything collides with
  everything, so existing scenes are unaffected.
- **Filtering rule**: `shouldCollide(layerA, maskA, layerB, maskB)`
  is the standard mutual bitmask test —
  `(layerA & maskB) != 0 && (layerB & maskA) != 0` — so either side
  can veto a pairing and a one-way "I see you" never collides. A
  component-level overload `shouldCollide(a, b)` reads the fields
  directly.
- **Physics integration**: the test runs inside
  `PhysicsSystem::broadPhase()` — in the AABB-tree pair query and
  in the plane-pairing loop — so a filtered pair never becomes a
  candidate: no manifold generation, no CCD, no islands, no solver
  work, and `Stats::candidatePairs` reflects the filtered count.
  Values are read live from the components each step, so changing
  a layer or mask at runtime takes effect on the next step (the
  demo tags its player capsule `CollisionLayer::Player`, mask
  `All`, as a usage example).
- **Query integration**: `Scene::raycast`, `raycastAll` and
  `raycastPrecise` accept an optional `layerMask` (default `All`).
  An entity answers through its collider's layer; collider-less
  pickables (mesh-only entities) answer on the `Default` layer.
  The AABB tree itself stays layer-agnostic — layers live only in
  the collider component, so there is no second copy to go stale;
  filters are applied where candidates are consumed.

Verification: 26 new checks (named-bit sanity, the full mutual
truth table including symmetry over all layer/mask combinations,
factory defaults, overlapping static bodies producing zero
broadphase pairs until the mask re-includes their layer, a dropped
cube passing through a platform it is masked against — from either
direction — and resting on the floor while the unmasked control
lands on the platform, and raycast/raycastAll/raycastPrecise layer
filtering including mesh-only Default-layer answers) join the
suite: **518 checks, all passing in Debug, Release-shared and
Release-static, CTest 1/1, zero warnings.**

## Phase 16 — Physics Constraints & Joints ✅

`JointComponent` (Physics) now implements four constraint types
between two bodies — or a body and the world — solved by a proper
sequential-impulse joint solver in `PhysicsSystem`:

- **Joint types** (factories `JointComponent::distance / hinge /
  fixed / springDamper`, mirroring the collider factories):
  *Distance* holds the anchor separation at `restLength` (negative
  captures the initial separation once), or inside
  `[minDistance, maxDistance]` when bounds are set — a rope when
  only `maxDistance` is given, a strut when only `minDistance` is.
  *Hinge* is a true revolute joint: anchor points coincide and the
  only free relative motion is rotation about `hingeAxis`, with
  optional twist limits (`enableLimits`, `min/maxAngleDegrees`,
  measured from the relative orientation captured when the solver
  first sees the joint). *Fixed* welds the pair to the relative
  position and orientation captured at first sight. *SpringDamper*
  remains the force-based spring. `compliance` (0..1) softens the
  rigid types by scaling the error feedback.
- **Solver**: every rigid type is built from scalar impulse rows
  with full point Jacobians — anchor lever arms, world inverse
  inertias, effective-mass denominators — solved alongside contacts
  in each island's velocity iterations. Distance bounds and hinge
  limits are unilateral rows (accumulated impulse clamped to one
  sign); everything else is bilateral. Hinge adds two
  axis-alignment angular rows; Fixed adds three orientation rows
  against the captured relative orientation; both share the three
  anchor-coincidence rows. A positional pass projects anchor
  errors and axis/orientation misalignment each sub-step, and the
  Baumgarte bias is clamped to a 2 m/s correction speed so spawn
  errors can never launch a body (a 1 m error previously became a
  48 m/s target velocity).
- **Warm starting**: accumulated impulses, captured rest lengths
  and captured reference orientations persist per joint (keyed by
  the joint-owning entity) across sub-steps and steps, are
  re-applied through the current Jacobians before velocities are
  measured — the same protocol as contact warm starting — and are
  pruned when a joint disappears. Joint-linked bodies were already
  grouped into single solver islands and already woke each other;
  both are covered by tests, so multi-body structures sleep, wake
  and solve cohesively. (The pre-existing placeholder hinge, which
  behaved as a world tether, became a real revolute joint; its
  legacy test now uses an offset anchor and keeps the same
  swing-radius assertion.)

Verification: 17 new checks (factories/defaults, exact distance
radius through a gravity swing, captured rest length, rope
slack-then-taut, min-distance strut behaviour, hinge anchor
coincidence and axis purity, ±30° hinge limits crossed and held,
a welded pair holding zero relative transform through a drop,
landing and a 120°/s initial spin, island grouping and
joint-partner waking, compliance softness that still converges,
and a double pendulum whose total energy over 3 s at 8 sub-steps
drifts < 15 % of its 40 J potential range with no energy gain and
< 1 mm anchor gaps — the bounded first-order dissipation any
impulse solver shows, verified rather than assumed) join the
suite: **535 checks, all passing in Debug, Release-shared and
Release-static, CTest 1/1, zero warnings.**

## Phase 17 — Event Bus & Input System ✅

Producers and consumers no longer call each other directly:
`EventBus` (`src/Core/EventBus.h`) is a small header-only,
type-safe publish/subscribe hub. Any struct can be an event;
`subscribe<T>()` dispatches synchronously in subscription order,
`unsubscribe(id)` (or the RAII `EventBus::Subscription` from
`subscribeScoped<T>()`) detaches, and dispatch iterates a snapshot
so listeners may safely subscribe/unsubscribe while handling an
event. The engine emits three core events on it:

- **`CollisionEvent`** (entities, world contact point, normal,
  strongest contact impulse) — published by `PhysicsSystem` at the
  end of `update()` for contact pairs that appeared since the
  previous update: resting pairs fire once, bounce re-contacts and
  re-drops fire again, and nothing is ever published from inside
  the solver, so listeners may inspect or modify the scene.
- **`TriggerEvent`** (`triggerEntity`, `otherEntity`, `entered`) —
  colliders flagged `ColliderComponent::isTrigger` generate
  enter/exit crossings instead of contact response: bodies fall
  straight through, only pairs passing the Phase 15 layer/mask
  filter are reported, and the event names which side is the
  trigger. Event generation respects the end-of-update pair-set
  diff, so sub-step jitter never double-fires.
- **`EntityDestroyedEvent`** — published by `Scene::destroyEntity`
  *before* the entity is destroyed, while its components are still
  readable: the cleanup-hook point for per-entity state kept
  outside the registry.

`Input` grew from raw state polling into the full input system.
It still tracks held keys/buttons plus per-frame pressed/released
edges, cursor position/delta and scroll — now through shared
`inject*()` entry points that the GLFW callbacks and tools/tests
both feed — and adds named mappings on top: `bindAction` /
`bindActionButton` collect any number of physical controls under
one gameplay name (`isActionDown`, `wasActionPressed`,
`wasActionReleased`), and `bindAxis` turns a key pair into a
-1/0/+1 axis (`axisValue`). Gameplay code asks for "Impulse" or
"Horizontal", never for raw key codes. A default-constructed
headless `Input` (no window) exists for tests and replays.

Demo integration proves the loop end to end: `Application` owns
one bus wired into both `Scene` and `PhysicsSystem`, logs trigger
crossings and hard (> 25 N·s) collisions from subscribed hooks,
adds a static trigger pad beside the cube stacks that bodies and
the player fall/walk through, and binds the **F** key to an
"Impulse" action that launches the Phase 13 picked body up and
away from the camera.

Verification: 31 new checks (bus ordering, payloads, type
isolation, id/RAII/clear unsubscription, no-replay, safe
unsubscribe-during-dispatch; injected press/held/repeat/release
transitions, cursor baseline/delta/scroll accumulation and
per-frame clearing, multi-control actions, axis signs and
cancellation; collision-began exactly-once and re-fire after
re-drop, trigger enter/exit with fall-through and layer/mask
silence, destroyed-entity cleanup with live components) join the
suite: **566 checks, all passing in Debug, Release-shared and
Release-static, CTest 1/1, zero warnings.**

## Phase 18 — Scene Graph & Transform Hierarchy ✅

Entities now form a tree. `TransformComponent` keeps its authored
LOCAL placement; two new components encode the graph —
`ParentComponent` (the parent handle, on the child) and
`ChildrenComponent` (the child handle list, on the parent) — and a
computed `WorldTransformComponent` carries the propagated result:
world matrix plus decomposed world position/orientation/scale.

- **Hierarchy API** (`Scene::setParent / removeParent / parentOf /
  childrenOf / isDescendantOf`, mirrored on `Entity`): both sides
  of every link are maintained atomically, reparenting moves
  children between lists, and cycle-creating or self-parenting
  links are rejected without side effects. `keepWorldTransform`
  variants recompute the child's local transform so its world
  placement survives (re)parenting or detaching.
- **Destruction policy** (`Scene::setHierarchyDestroyMode`):
  *Orphan* (default) detaches children to the root with their
  world placement rewritten into their local transforms — nothing
  visually moves; *Cascade* destroys the whole subtree, each
  descendant publishing its own Phase 17 `EntityDestroyedEvent`.
  Destroying a child always unlinks it from its parent's list.
- **TransformSystem** recomputes world transforms by walking the
  tree from the roots: `world = parentWorld * local`. Orientation
  composes as quaternions (no gimbal lock at any depth), scale
  inherits multiplicatively, and rotated non-uniform scale composes
  into the matrix. The pass runs twice per fixed update in the
  demo (before physics so parent placement is current, after every
  transform writer so picking/rendering are fresh), and malformed
  cycles fall back to local matrices instead of stalling.
- **Physics integration**: the solver keeps working purely in
  world space. `PhysicsSystem::update` converts parented bodies
  local→world on entry (ancestors first) and writes results back
  world→local on exit, so contacts, joints, CCD, thrusters and
  gravity fields never see hierarchy — while colliders and bodies
  under moved/rotated rigs collide exactly at their world
  placement, and moving a parent carries resting child bodies.
  Outside `update()`, transforms always hold authored local values
  (spawn capture/reset semantics unchanged).
- **Rendering & picking**: `RenderSystem` draws with propagated
  world matrices (per-entity matrices resolved serially before the
  parallel draw evaluation), and raycasts — both the BVH
  broadphase and the exact mesh/collider tests — test world
  placement, falling back to local matrices before the first
  propagation pass. The demo parents a small beacon cube to the
  tumbling centre physics cube; it rides and spins with it.

Verification: 22 new checks (two-level rotation/scale composition
with exact matrix point mapping, quaternion yaw compounding,
eight-level depth, link/reparent/detach bookkeeping, cycle
rejection, keep-world reparenting, orphan world-preservation and
cascade event counts, offset-rig static colliders, a dynamic body
falling under a 90°-rotated rig with exact local write-back,
moving-parent carry, world-space picking) join the suite: **588
checks, all passing in Debug, Release-shared and Release-static,
CTest 1/1, zero warnings.**

## Phase 19 — Scene Serialization & Asset Persistence ✅

A scene is now a document. `Scene::saveToString / loadFromString`
and `Scene::saveToFile / loadFromFile` serialize the entire world
to JSON and back:

- **Format machinery** (`src/Core/Json.h`): a small self-contained
  RFC 8259 value model, parser and pretty-writer — no external
  dependency. Objects preserve insertion order, strings handle full
  escaping (including surrogate pairs), and numbers print in the
  shortest representation that round-trips — values that are
  exactly a float print in float form (`0.123`, not
  `0.12300000298023224`), so files stay readable *and* float fields
  survive bit-for-bit.
- **What's stored**: every entity with its tag, transform (TRS plus
  the authoritative quaternion and its flag), camera (full
  `CameraConfig` plus live pose), mesh (asset and material by
  stable name, visibility, local bounds), spin, rigid body
  (velocities, momentum, orientation, both inertia tensors, mass
  and inverse mass, material, gravity/sleep flags), collider (all
  six shapes' dimensions, hull point cloud, layer/mask, isTrigger),
  gravity field, thruster, and joints (type, anchors, limits,
  compliance, spring parameters). Entities are referenced by
  document index — EnTT handles recycle and are never persisted.
- **Reconstruction**: loading validates the document
  (format tag + version) and leaves the scene untouched on
  malformed input; on success the registry is cleared wholesale
  (silently, no destroyed-events) and rebuilt in two passes —
  entities and their own components first, then hierarchy through
  `Scene::setParent` and joint partner remapping — so document
  order never matters. `ChildrenComponent` and
  `WorldTransformComponent` are derived state and are rebuilt by
  their systems, never stored. Transient solver accumulators
  (force/torque) restart at zero, as they do every step boundary.

Demo integration: at startup the application serializes the
freshly built sandbox scene, reloads it into a scratch scene, and
logs the verified round-trip (entity count and document size) —
the live scene and its physics spawn snapshots are never touched.

Verification: 23 new checks (bit-exact field comparisons across
transforms, bodies, all collider shapes, layers/masks/triggers,
hull clouds, hierarchy relinking, all joint types with remapped
partners, camera pose + config, mesh references; a reloaded scene
that lands two jointed bodies at exactly their 2 m separation;
file round-trip through a temp path; malformed JSON, wrong
versions and missing files rejected with the scene untouched)
join the suite: **611 checks, all passing in Debug, Release-shared
and Release-static, CTest 1/1, zero warnings.**

## Phase 20 — Asynchronous Job-Driven Asset Manager & Resource Pipeline ✅

The startup-only registry grew a full async pipeline. Synchronous
registration (programmatic meshes, shaders, materials — the Phase 5
API) is unchanged; file-backed assets now load without ever
blocking a frame:

- **Registry & handles**: stable dense ids per kind (meshes,
  shaders, materials, and now textures — name-keyed `TextureAsset`
  RGBA8 images with VRAM-budgeted registration), the same ids
  Phase 19 scene files reference by name. Every async asset carries
  a lifecycle state — `Unloaded`, `Loading`, `Loaded`, `Failed` —
  queryable per kind (`meshState`/`textureState`/`assetError`).
- **Job integration**: `declareMeshSource` / `declareTextureSource`
  bind a name to a file; `requestLoad` flips it to `Loading` and
  dispatches file I/O + parsing (the existing OBJ importer, a new
  dependency-free binary-PPM reader) onto the work-stealing
  `JobSystem`. Workers touch only their load record; completions
  travel through lock-free MPMC rings (mutex overflow for bursts)
  and `processCompletedLoads()` — the main thread's frame sync
  boundary — publishes them into the registry with budget checks,
  so GPU-facing state mutates only on the thread that renders.
  `pendingUploadCount` exposes finished-but-unpublished loads.
  Without a JobSystem, loads run inline but still respect the
  boundary. Records are shared_ptr-owned and the completion sink
  outlives the manager, so late worker completions are always safe.
- **Fallbacks**: built-in error assets (magenta-tinted cube,
  magenta/black checker texture) back `meshOrFallback` /
  `textureOrFallback` — unknown names, first loads still in flight
  and failed loads all degrade to visible, debuggable placeholders
  instead of nulls; reloads serve stale data until the swap lands.
- **Hot reload**: `reloadAsset` re-reads a source in place — the
  registry entry keeps its stable id, VRAM reservations are
  re-balanced, data swaps at the next sync boundary — and
  `pollFileChanges()` triggers exactly that for every `Loaded`
  asset whose file mtime moved. State-change events
  (`setAssetEventCallback`) fire `Loading` on the requesting thread
  and `Loaded`/`Failed` on the processing thread.

Demo integration: the application hands its `JobSystem` to the
asset manager, declares and queues every `*.obj`/`*.ppm` found in
`./assets` at startup (names from file stems, matching
`MeshComponent` references), drains up to 8 completions per frame
in the update callback, polls file mtimes at 1 Hz, and logs
terminal load events.

Verification: 18 new checks (handle validation, fallback identity
and pixels, synchronous texture registration, deterministic
in-flight observation via blocked workers — state stays `Loading`
after workers finish until the sync boundary runs — OBJ/PPM
content fidelity, failed-load errors, duplicate/undeclared
rejections, mtime hot-reload with id stability and watcher
idempotence, inline mode with bounded processing budgets, and 16
assets submitted concurrently from 4 threads registering exactly
once) join the suite: **629 checks, all passing in Debug,
Release-shared and Release-static, CTest 1/1, zero warnings.**

## Phase 21 — Modular Layout, Spatial Culling & Advanced Lighting ✅

A structural phase: the codebase moved to a production public/private
layout, visibility became a two-stage spatial pipeline, and shadow
quality plus per-object dynamic lights landed without touching the
performance-critical cores (lock-free rings, work-stealing jobs,
paginated pools are byte-for-byte the same code):

- **Public header tree**: every public header moved from `src/` to
  `include/Engine/<Subsystem>/` (`Core`, `Platform`, `Geometry`,
  `Graphics`, `Assets`, `Scene/Systems`, `Physics`), keeping the
  existing module directory names and every `#include "Scene/Scene.h"`
  -style string — the only build change is the include root in
  `engine_module()`. `src/` holds implementations only. One
  deliberate trade is documented here rather than hidden: the
  namespace stays flat `engine::`. Subsystem separation is already
  enforced physically — the Phase 8 DLL-module link layering makes
  cycles link errors (e.g. `EngineScene` may use `EngineGraphics`,
  never the reverse) — while a namespace rename would churn all 661
  checks and every call site for zero functional gain; the
  include-path hierarchy carries the module identity instead.
- **Two-stage culling** (before any command buffer is recorded, so
  culled geometry costs zero GPU cycles): frustum tests now reject
  with a cheap bounding-sphere sweep before the exact p-vertex AABB
  test, and `Scene::queryFrustum` gathers visible entities in one
  pruned `DynamicAABBTree` traversal (the same BVH the raycasts
  use). A conservative screen-space occlusion pass (default off,
  `[Render] occlusionCulling`) drops meshes whose NDC rect sits
  fully inside a static box/OBB occluder's rect and strictly behind
  its far depth; it never culls partially visible geometry and is
  reported separately (`occludedCount`), never folded into the
  frustum `culledCount`. Occluder entities exempt themselves.
- **Cascaded shadow maps**: the single fixed light frustum became
  4 cascades fitted per-frame to the camera's frustum slices
  (practical log/uniform split, λ = 0.7; bounding-sphere fit with
  texel snapping against shimmer), one 2048² depth target each,
  sampled as a `sampler2D` array; the shadow pass selects its
  cascade through an extended push-constant word at byte offset
  112 (range now 116 of the guaranteed 128 bytes). The fragment
  shader picks the cascade by view depth and runs the same 3×3 PCF
  kernel per slice — near shadows stay crisp, far coverage is kept.
- **Dynamic lights**: `PointLightComponent` / `SpotLightComponent`
  are gathered by the new `LightingSystem` each frame — world-aware
  positions (hierarchy included), nearest-16 selection to the
  camera, packed vec4 arrays in the UBO (1456 bytes, asserted
  against the GLSL block in tests) with a smooth windowed
  inverse-square attenuation mirrored exactly on the CPU for tests
  and tooling, and a cosine-eased spot cone (inner/outer half-angles;
  entity orientation aims the spot, −Z forward like the camera).
  Demo scene: a warm point light over the sandbox and a cool
  downward spot on the floor.

Verification: 32 new checks (split-scheme ordering and blend,
cascade matrix validity and slice containment, BVH `queryFrustum`
exactness, occlusion unit cases + end-to-end `prepareFrame`
accounting with occlusion on/off, light packing order, 16-light
nearest cap, disabled-light skipping, attenuation/spot closed-form
mirrors, UBO byte-budget) join the suite: **661 checks, all passing
in Debug, Release-shared and Release-static, CTest 1/1, zero
warnings.**

## Phase 22 — Metallic-Roughness PBR, Image-Based Lighting & Cinematic Post ✅

The renderer grew from a fixed-function-style Blinn-Phong sandbox
into a physically based, HDR pipeline:

- **Cook-Torrance PBR**: GGX/Trowbridge-Reitz distribution, Smith
  geometry (Schlick-GGX), Schlick Fresnel — evaluated for the
  cascaded directional key light and the nearest-16 point/spot
  lights from Phase 21, all in linear HDR. Materials are
  metallic-roughness (`MaterialDesc::pbr`): scalar metal/rough
  factors plus optional texture maps (albedo [sRGB-decoded in
  shader], metallic-roughness [glTF G/B packing], tangent-space
  normal [derivative cotangent frame — no vertex tangents needed],
  ambient occlusion), resolved per draw into the same 112-byte
  push payload (indices into a 32-slot material texture array,
  -1 = untextured). The CPU mirror in `Graphics/Pbr.h` is the
  testable ground truth for the shader formulas.
- **IBL & skybox**: a procedural HDR sky (gradient + sun disc that
  matches the key light) is generated deterministically on the CPU
  (Hammersley quasi-random sampling throughout — bit-identical
  every run), convolved into a cosine-weighted irradiance cube
  (stored as E/π) and a 5-level GGX-prefiltered reflection cube,
  and integrated into the standard split-sum BRDF LUT. The
  fragment shader combines them as
  `prefiltered * (F * LUT.x + LUT.y)` plus irradiance diffuse, so
  metals tint their reflections and rough surfaces blur them. The
  same environment cube renders as an inside-out skybox at the
  far plane (`z = w`, depth-write off).
- **Cinematic post chain**: the scene now renders into an
  RGBA16F HDR offscreen target (one per frame in flight, rebuilt
  on resize) instead of the swapchain. A 5-level bloom chain
  (soft-knee bright-pass + 3×3 tent downsample, additive tent
  upsample) extracts the glow; a fullscreen composite applies
  exposure, folds bloom in, compresses with the ACES filmic curve
  (Narkowicz fit) and encodes sRGB — the HUD draws after the
  composite, on the presented image. Everything is tunable from
  `[Render]` (`exposure`, `bloomEnabled/Strength/Threshold`,
  `skyboxEnabled`, `environmentIntensity`). Scene pipelines were
  retargeted to the HDR pass via `GraphicsPipeline::
  setSceneRenderPass` (called by the Renderer before materials
  register); the UBO grew one vec4 (1472 bytes, still byte-matched
  to GLSL). Material textures mirror from the asset registry into
  the GPU array on a version counter — hot reloads re-upload
  automatically.

Verification: 42 new checks (GGX/Smith/Fresnel closed forms and
behavioral metal/dielectric cases, ACES/sRGB/bright-pass exact
values, cube-mapping round-trips, constant-environment
convolutions reproducing the constant exactly, LUT bounds +
determinism, sky sun peak, prefilter blur variance, material →
draw-item PBR/texture mapping, registry version bumps, INI
parsing) join the suite: **703 checks, all passing in Debug,
Release-shared and Release-static, CTest 1/1, zero warnings.**
Additionally, every shader now compiles and link-checks with a
real glslangValidator (built from the KhronosGroup/glslang 15.4
sources; the sandbox SDK validator is a stub), including the
dynamically-indexed material texture array.

## Phase 23 — Native C++ Game Modules & Hot-Reload Scripting ✅

Gameplay code no longer has to live inside the engine binary. A
game is now a native shared library the engine loads, drives, and
hot-swaps while running — with the world state safely on the engine
side of the boundary:

- **Stable module ABI** (`Core/GameModule.h`): plugins implement
  `IGameModule` (`onLoad` / `onUnload` / `onUpdate` /
  `onFixedUpdate`, plus a `moduleName`) and expose two `extern "C"`
  factories, `CreateGameModule` / `DestroyGameModule`, stamped by
  the `ENGINE_IMPLEMENT_GAME_MODULE` macro — so objects are always
  created and destroyed inside the plugin, never across heaps.
  `ENGINE_PLUGIN_API` (`Core/PluginApi.h`) resolves to
  `dllexport`/`dllimport` under `ENGINE_BUILDING_PLUGIN` on Windows
  and default visibility on ELF. The ABI header itself includes no
  EnTT/GLM and no engine library linkage: a plugin needs only the
  headers.
- **EngineContext**: every module call receives the controlled
  surface — `Scene` (the ECS registry lives in engine memory),
  `Input`, `AssetManager`, `JobSystem`, frame/timing data and a
  `userData` slot. Because the registry outlives the module, a
  reload swaps logic, never world state.
- **Scratch-copy loading** (`Platform/DynamicLibrary`,
  `Core/ModuleManager`): the manager copies the `.dll`/`.so` to a
  uniquely named scratch file next to it and loads *that*, so the
  compiler can rebuild the original while the engine runs (no file
  locking on Windows). Scratch names come from a process-wide
  monotonic counter — two managers can never collide on the path
  one of them has mapped. The move-only `DynamicLibrary` wraps
  `LoadLibraryW` / `dlopen(RTLD_NOW | RTLD_LOCAL)` with clean
  symbol resolution.
- **Failure-proof reload pipeline**: a candidate generation is
  copied, loaded, symbol-checked, constructed and `onLoad`ed
  *before* the running generation is retired; a missing file, a
  library without factories, a factory returning null, or a
  throwing `onLoad` just logs and keeps the old module (or no
  module) running. A module whose `onUpdate`/`onFixedUpdate`
  throws is retired cleanly mid-run. `pollForChanges()` (mtime)
  drives hot reload from the app's update loop, and every load is
  paired with exactly one unload.
- **Wiring**: `[Game] modulePath` / `moduleHotReload` in
  `config/engine.ini`; the `Application` loads the module at
  startup, feeds `onFixedUpdate` from the fixed-step loop (before
  physics, so modules can drive bodies), `onUpdate` from the frame
  loop, and polls for rebuilds every 0.5 s.

Verified headlessly with three mock plugin binaries built as real
`MODULE` libraries (a state-file mock observed across the boundary,
a factory-less library, and a throwing `onLoad`): raw loader symbol
resolution, scratch isolation and rotation, exact lifecycle pairing,
scene/entity survival across reloads, bad-rebuild fallback,
throwing-init recovery, and delta/frame-index delivery — **727
passing checks** (24 new), zero warnings in Debug, Release-shared
and Release-static, CTest 1/1, and all 11 shaders still compile
under a real glslangValidator 15.4.

## Phase 24 — Engine-Branded Modules & Newtonian Space Flight ✅

The Phase 23 plugin system grew a security/naming policy and its
first real game: a 6-DOF Newtonian space-flight sandbox delivered
entirely as a hot-reloadable module.

- **Engine-branded naming** (`engine_add_game_module` in CMake):
  every game module ships as `EngineRuntimeCore_<signature>` where
  the signature is the first 10 hex chars of the MD5 of the target
  name (e.g. the flight module is `EngineRuntimeCore_31ba0de483.so`).
  `ModuleManager::isEngineBrandedModuleName` enforces the same
  convention at load — a valid binary under a generic name like
  `game_logic.so` is rejected.
- **Handshake validation** (`GetEngineModuleInfo`): modules built
  with `ENGINE_IMPLEMENT_GAME_MODULE` now also export a descriptor
  carrying the engine ABI token (`0x45474D4F` "EGMO"), the exact ABI
  version, and its struct size. The manager validates all three
  *before any factory runs*, so arbitrary DLLs, forged descriptors,
  and modules built against a stale ABI are refused with distinct
  errors. (Two adversarial fixtures — a forged-token module and a
  legacy-ABI module — are part of the test suite.)
- **`SpaceFlightModule`** (`modules/SpaceFlightModule.cpp`, model in
  `modules/SpaceFlightModel.h`): flies a 1200 kg spacecraft with
  true Newtonian physics — WASD/Space/Ctrl translational thrusters,
  Q/E roll, mouse pitch/yaw — computed as body-frame forces/torques
  and accumulated into the ship's `RigidBodyComponent`; the engine
  physics integrates them (zero drag, angular momentum conserved;
  the only counter-torque is an RCS flight-assist damper, itself
  modeled thruster work). Editing `FlightParams` (mass-normalized
  thrust, torque authority, damping), rebuilding the module and
  letting the 0.5 s poll swap it in changes the flight feel
  mid-flight with the ship's state untouched.
- **Sandbox**: on load the module procedurally registers a jittered-
  icosahedron asteroid mesh and a dart ship mesh with the
  AssetManager, spawns 56 seeded asteroids (static sphere colliders
  in the physics BVH — sized via transform scale, the engine's
  collider convention), and parents three thruster-nozzle point
  lights to the ship. Nozzle light intensity pulses with throttle
  over a ~7 Hz flicker, and HDR-bright nozzle materials feed the
  Phase 22 bloom chain; the Phase 22 IBL skybox is the backdrop.
  All spawning is find-or-create, so reloads adopt the world
  instead of duplicating it.

Verification is fully headless: the real plugin binary is loaded
through ModuleManager against a live Scene + PhysicsSystem with
synthesized input (`Input::injectKeyState`) — a 0.5 s W burn
reaches the expected ~10 m/s (24 kN / 1200 kg), engines-off coast
conserves velocity, Q roll spins the ship up, lights pulse, and a
mid-flight hot reload preserves velocity bit-exactly with zero
duplicated entities. **759 checks (32 new) pass in Debug,
Release-shared and Release-static with zero warnings, CTest 1/1.**

## Phase 25 — Native Component Reflection & Unity/UE-Style Scripting ✅

Game logic moves from monolithic module frame callbacks to
per-entity script components, and component fields become visible
to reflection tooling — the foundation for editor inspectors.

- **Zero-overhead reflection** (`Scene/Reflection.h`,
  `Scene/ComponentReflection.h`): `ENGINE_REFLECT_BEGIN(Type)` /
  `ENGINE_REFLECT_PROPERTY(field, "Display Name")` /
  `ENGINE_REFLECT_END()` generate an ADL `reflectType()` overload
  per type (works in any namespace), yielding a static `TypeInfo` —
  name plus a `PropertyInfo` table (name, display name,
  `PropertyType` {Float, Int32, UInt32, Bool, Vec3}, true `offsetof`
  byte offset). No RTTI, no registration ceremony, no runtime cost
  beyond the static tables. `readProperty`/`writeProperty` memcpy
  through the offset, so tools (and tests) edit real fields
  generically. Built-ins (Transform, RigidBody, Point/SpotLight,
  Spin) are reflected in `ComponentReflection.h`.
- **ReflectionRegistry**: a name → TypeInfo directory for tooling
  that does not know the C++ type at compile time. Built-ins
  register lazily on first `instance()`; registration replaces
  same-name entries (hot-reload safe) and `unregisterType` removes
  by table-pointer identity, so a retiring module generation can
  never erase its replacement's table. Names are `std::string_view`
  so comparisons stay content-based across binary boundaries.
- **Native scripts** (`Scene/NativeScript.h`): `INativeScript`
  exposes the Unity/UE lifecycle — `onAwake`, `onStart`,
  `onUpdate(float)`, `onFixedUpdate(float)`, `onDestroy` — and
  `NativeScriptComponent` (script-class name + instance) attaches
  one to any entity. `ScriptRegistry` instantiates scripts by name
  from factories registered with `REGISTER_SCRIPT(ClassName)`;
  factories are plain function pointers, never `std::function`,
  because a callable created inside a game module must never be
  destroyed or invoked by the host after that module unloads.
  `unregisterScript` is pointer-checked for the same reason.
  `NativeScriptSystem` drives the lifecycle from the host frame
  loop (awake/start on first tick, update/fixed-update per frame,
  destroy + delete on entity destruction via one EnTT `on_destroy`
  hook) and prefers `EngineContext::scripts`, so statically linked
  modules register into the host's registry.
- **SpaceFlightModule refactored**: all flight behaviour now lives
  in `SpaceFlightScript`, an `INativeScript` attached to the ship;
  the module only builds the sandbox in `onLoad` (its frame
  callbacks are no-ops) and destroys the live script instance in
  `onUnload`, before its code unmaps. `FlightParams` is reflected
  (7 properties), so an editor editing `mainThrust` retunes the
  burn — the tests double thrust purely through reflection writes.
- **Module-boundary storage contract**: a Scene now primes every
  built-in component storage from engine code at construction. An
  EnTT storage carries the vtable and shared_ptr control block of
  whichever binary's instantiation created it; priming guarantees
  module code only ever finds host-owned storages, so scenes safely
  outlive unloaded modules. (Found by crash bisection: glibc does
  not reliably unmap dlclosed libraries, which had masked
  plugin-created storages since Phase 24.)

Verification: 28 new headless checks cover reflection tables
(exact `offsetof`, typed reads/writes through offsets), registry
replace/unregister semantics, script registration and
instantiation by name, lifecycle order (awake → start → update →
fixed → destroy), dormant unregistered names, reflection-edited
thrust changing the burn, script-driven flight matching the
Phase 24 numbers, and hot-reload adoption (fresh instance,
velocity preserved, the new generation flying the same ship).
**787 checks pass in Debug, Release-shared and Release-static
with zero warnings, CTest 1/1** — and additionally in a
hidden-visibility build (simulating Windows DLL rules) and under
AddressSanitizer.

## Phase 26 — Custom Vulkan UI Renderer & Primitives Pipeline ✅

Screen-space UI gets its own analytic-shader pipeline: rounded
rectangles, borders and solid colors are evaluated per-pixel from
a signed distance field, and — Step 2 — SDF text, textured images
and a scissor stack turn it into a real immediate-mode UI layer,
still flushed as a handful of indexed draws per frame. Step 3
adds interactive widgets (buttons, sliders, checkboxes) with
hit-testing/focus management, plus a reflection-driven Component
Inspector built entirely from those widgets.

- **UIVertex & analytic shaders** (`shaders/ui.vert`,
  `shaders/ui.frag`, compiled to SPIR-V by glslangValidator like
  every engine shader): position (pixels, top-left origin), uv
  (0..1 across the quad), fill color, corner radius, border width,
  border color. The fragment shader recovers the rect's exact
  pixel size from the uv derivatives and evaluates one rounded-box
  SDF into fill, border ring and anti-aliased coverage, so radius
  and border stay in true pixel units at any rect size.
- **UIBatch** (pure CPU, headless-testable): immediate-mode
  accumulator — `begin()` per frame, `drawRect()` /
  `drawRoundedRect()` (radius + border via `UIRectStyle`) append
  one quad (4 vertices + 6 indices) each, and `bounds()` tracks
  the union of all queued rects. The batch is capacity-capped
  (65536 vertices / 98304 indices) and truncates whole quads
  rather than overflowing; degenerate rects emit nothing.
- **SDF text** (`Graphics/SdfFont.h`): the built-in 5x7 bitmap
  font data is baked by an exact signed-distance transform into
  one 384x192 R8 atlas page (128 = glyph edge, 6-texel spread),
  with save/load round-tripping through a small "ESDF" blob for
  future asset caching. `drawText()` lays out real glyph quads
  (whole-cell quads carrying the cell's atlas rect, pen advance,
  newline handling, '?' fallback; `measureText()` reports the
  widest line), and the fragment shader thresholds the field with
  a derivative-sized band — crisp at any scale from one texture.
  Without an atlas, `drawText()` degrades to accounting only.
- **Texture descriptors**: `drawTexture(id, ...)` samples a
  caller-registered descriptor set (`UIRenderer::registerTexture`
  hands out never-reused ids; binding 0 = combined image sampler)
  for icons/thumbnails/render targets, tinted and SDF-clipped, so
  images get rounded corners for free. Consecutive quads sharing
  texture + scissor coalesce into one flush segment, so state
  changes cost exactly one extra draw each.
- **Scissor stack**: `pushScissor()`/`popScissor()` nest clip
  rects (each push intersects with its parent; the root is the
  viewport; popping the root is a no-op). The flush programs
  `vkCmdSetScissor` per segment and restores the full-frame
  scissor afterwards for the HUD pass — scroll views and nested
  panels clip correctly.
- **UIRenderer**: dedicated pipeline (one sampler descriptor,
  dynamic viewport/scissor, no depth test, alpha blending) over
  persistently mapped dynamic vertex/index buffers plus the
  uploaded font atlas texture; `record()` uploads the frame's
  batch and flushes it segment by segment, then clears it. The
  vertex-input contract lives in exactly one place
  (`UIRenderer::vertexInput()`), shared by pipeline creation and
  the tests.
- **UIContext widgets** (`Graphics/UIContext.h`): immediate-mode
  interaction state over the batch — hovered / active / focused
  widget ids (FNV-1a of panel seed + label) carried across
  frames. Hit-testing respects the batch's nested scissor stack,
  so clipped pixels neither hover nor press; presses capture the
  mouse until release (sliders follow the pointer off-row),
  pressing a widget steals focus, and pressing empty space
  clears it. `button()` (normal/hover/active states, click on
  completed press+release), `slider()` (track jump, drag,
  clamping, 1%-of-range keyboard steps while focused),
  `checkbox()` and three-axis `vec3Sliders()` all draw with the
  SDF batch and share one theme; panels provide the row layout.
- **Component Inspector** (`Graphics/UIInspector.h`): renders any
  Phase 25 `TypeInfo` as an interactive panel with zero per-type
  code — floats become sliders, bools become checkbox toggles,
  vec3s become per-axis sliders, ints render as read-only
  readouts — and writes edits back through the reflected offsets
  in place. Header-only reflection keeps it in EngineGraphics
  with no EngineScene link dependency. The app live-inspects the
  player ship's `RigidBodyComponent` whenever the flight module
  has spawned it (inert while the mouse is flight-captured).
- **Integration**: `Renderer` owns the UIRenderer next to the HUD
  and records it inside the present pass right after the 3D
  composite, under the HUD text; the Application issues a
  translucent rounded backdrop panel plus an accent rule behind
  the FPS HUD plus an SDF control hint along the bottom edge,
  every frame through `Renderer::uiBatch()`, and feeds real
  mouse/arrow-key state into its `UIContext` for the inspector.

Verification: 74 new headless checks cover vertex accumulation
and corner/uv layout, index offsets across quads, batch-bounds
unions (including negative origins), radius/border clamping,
degenerate-input rejection, the vertex-input layout (binding
stride + all eight attribute offsets/formats), capacity
truncation staying index-consistent, the SDF atlas bake
(interior/ramp/empty-cell texel audit, lowercase folding, '?'
fallback), save/load round-trip + corrupt-blob rejection, glyph
layout (pen advance, newline, cell-halo quads, measureText),
texture segment coalescing, scissor nesting/intersection, and
the full widget lifecycle driven by synthetic mouse frames —
hover/press/drag/release semantics, clipped hit-testing, focus
stealing and clearing, slider track math with clamping and
keyboard steps, checkbox toggling, panel layout rows, and an
inspector that edits a reflected struct's float/bool/vec3
fields in place while read-only ints never write back.
**861 checks pass in Debug, Release-shared and Release-static
with zero warnings, CTest 1/1** — and additionally in a
hidden-visibility build (simulating Windows DLL rules) and under
AddressSanitizer.

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
cmake --build build   # shared engine modules + thin executable
# Modular static alternative: add -DENGINE_BUILD_SHARED=OFF
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
**WASD** move, **Space/E** up, **Q** down, **Shift** boost, **Esc**
releases the mouse (press again to quit).

## Project structure

```
.
├── CMakeLists.txt            # Build, dependencies, shader compilation, tests
├── CMakePresets.json         # debug / release presets
├── config/
│   └── engine.ini            # All engine tuning (window, render, logger, loop)
├── shaders/
│   ├── cube.vert             # MVP, inverse-transpose normals, world/camera + material lighting varyings
│   ├── cube.frag             # Cook-Torrance PBR + CSM + 16 dynamic lights + split-sum IBL
│   ├── shadow.vert           # Depth-only directional shadow pass
│   ├── skybox.vert/.frag     # Inside-out environment cubemap background
│   ├── post.vert             # Fullscreen triangle for the post chain
│   ├── bloomDown/bloomUp.frag # Multi-scale bloom (bright-pass, tent blur)
│   └── composite.frag        # Exposure + ACES tone mapping + sRGB output
├── tests/
│   ├── CoreTests.cpp         # Config/logger/physics/player/material tests (CTest)
│   └── TestGameModule.cpp    # (+Bad/Throw/Forged/Legacy) mock plugins
├── modules/                    # Game modules (built as EngineRuntimeCore_*)
│   ├── SpaceFlightModule.cpp # Newtonian 6-DOF flight game module (P24)
│   └── SpaceFlightModel.h    # Shared flight math (plugin + tests)
├── include/Engine/           # Public headers (Phase 21): subsystem tree,
│   ├── Core/                 #   installed-style; src/ holds .cpp only
│   ├── Platform/
│   ├── Geometry/
│   ├── Graphics/
│   ├── Assets/
│   ├── Scene/                # + Scene/Systems/
│   └── Physics/
└── src/
    ├── main.cpp              # Bootstrap: arguments -> config -> logger -> app
    ├── Core/
    │   ├── Application.cpp   # Composition root, wires loop callbacks
    │   ├── Camera.cpp        # First-person yaw/pitch camera (GLM)
    │   ├── EngineConfig.cpp  # INI loading, validation, fallback chain
    │   ├── Logger.cpp        # Levels, console/file/async sinks, macros
    │   ├── Time.cpp          # steady_clock frame timing, delta, FPS
    │   ├── GameLoop.cpp      # Fixed/variable loop, FPS cap, statistics
    │   └── ModuleManager.cpp # Native game-module hot reload (Phase 23)
    ├── Platform/
    │   ├── Window.cpp        # GLFW window, surface, resize/input callbacks
    │   ├── DynamicLibrary.cpp # LoadLibrary/dlopen RAII wrapper (Phase 23)
    │   └── Input.cpp         # Key/mouse state, transitions, mouse delta
    ├── Assets/
    │   └── AssetManager.cpp  # Mesh/shader/material registry, VRAM-budgeted
    ├── Scene/
    │   ├── Scene.cpp         # RAII ECS world (entt::registry), Entity handle
    │   ├── Components.cpp    # Tag/Transform/Camera/Mesh/Spin components
    │   └── Systems/
    │       ├── CameraSystem.cpp  # Look + physical-player camera follow
    │       ├── PhysicsSystem.cpp # Rigid-body solver (fixed update)
    │       ├── PlayerSystem.cpp  # Physical capsule player controller
    │       ├── RotationSystem.cpp# Spin integration (fixed update)
    │       ├── LightingSystem.cpp# Point/spot gather + shadow cascades
    │       └── RenderSystem.cpp  # Scene -> view/projection + draw items
    ├── Geometry/
    │   ├── Bounds.cpp        # AABB + world-space transformation
    │   └── Cube.cpp          # Uniform-material cube (24 verts, 36 indices)
    └── Graphics/
        ├── VulkanContext.cpp   # Instance, devices, queues, command pool
        ├── VulkanHelpers.cpp   # Buffers/images, one-off transfers, file IO
        ├── Buffer.cpp          # RAII VkBuffer + staging uploads
        ├── Swapchain.cpp       # Swapchain, depth, render pass, framebuffers
        ├── GraphicsPipeline.cpp# Shader modules, pipeline state, UBO layout
        ├── EnvironmentMap.cpp  # Procedural sky, irradiance/prefilter convolution, BRDF LUT
        ├── Frustum.cpp         # View frustum extraction + AABB/sphere tests
        ├── RenderQueue.cpp     # CPU render command stream + parallel sort
        ├── PipelineState.cpp   # PSO descriptions, hashing, state cache
        ├── HudFont.cpp         # 5x7 bitmap font + HUD text layout (pure CPU)
        ├── HudRenderer.cpp     # Screen-space FPS HUD overlay pipeline
        └── Renderer.cpp        # Frames-in-flight, command recording, present
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
- **Phase 7 — Production physics ✅:** ECS rigid-body simulation with
  solver islands, warm starting, CCD, joints, thrusters, radial gravity,
  and terrestrial/zero-G demo scenes
- **Phase 8 — DLL modules, player & materials ✅:** layered shared-library
  engine modules with explicit export/import macros, a physical capsule
  player with exact capsule collision (landing, pushing, sliding), and a
  uniform solid-material pipeline (normals, diffuse shading, push
  constants), plus Phase 9's distinct palette and directional lighting,
  and Phase 10's shadow mapping, depth/winding contract tests, and 1:1
  quaternion rotation sync
- **Phase 11 — Sub-stepped physics ✅:** internal physics sub-stepping,
  multi-point contact manifolds with centroid-reduced sequential
  impulses, non-linear position correction, and resting-body sleep
- **Phase 12 — Asset import pipeline ✅:** dependency-free OBJ import
  into the asset registry, AABB/MeshBounds auto-scale normalization,
  and one-call ECS spawning with a 1:1 offset box hitbox
- **Stacking stability fix ✅:** warm-start feature identity,
  persistent-contact restitution suppression, penetration-weighted
  manifold centroids, resting-contact damping, relaxed/clamped
  position correction and hardened sleep/wake thresholds — stacks
  settle to absolute rest in under a second
- Later phases build the real engine core on top of this foundation —
  editors and visual tooling only arrive once the core is complete.

## Phase 27 — Editor Shell, Entity Outliner & Right-Click Context Menus ✅

The widget layer from Phase 26 Step 3 grows into a working editor
shell: a floating right-click context menu system in `UIContext`,
a docked Scene Hierarchy panel (`UIOutliner`, EngineScene) listing
every ECS entity, and a unified layout in `Application` where the
Outliner's selection drives the reflection-based Component
Inspector around the 3D viewport.

- **Floating popups** (`Graphics/UIContext`): `openPopup(id, x, y)`
  raises a single active menu (opening one replaces any other —
  the stacking discipline), `beginPopup`/`menuItem`/`menuSeparator`/
  `endPopup` render it ImGui-style on the frame after the click
  that opened it. Menu items fire on the press edge and dismiss
  the menu; disabled items render dimmed and swallow nothing.
  Dismissal is decided centrally in `beginFrame`: Esc closes, and
  a left press outside the popup closes it *and is consumed*
  (`m_pressConsumed` reaches `pressedRect` and every widget claim
  site) so the button/slider underneath never sees the click.
  The popup's hit height converges from the previous frame's
  measured content, like immediate-mode GUIs do.
- **UIInput extensions**: right-button state, an Esc edge and the
  mouse-wheel delta are appended after the Step 3 fields, so all
  existing aggregate initializations keep their meaning.
  `rightPressedRect` (edge-only, scissor-aware like every hit
  test) is the dispatch primitive for context clicks.
- **UIOutliner** (`Scene/UIOutliner`, in EngineScene beside the
  registry it drives): flattens the hierarchy each frame (roots
  sorted by name for determinism, children in scene order,
  depth-capped and cycle-guarded), rows scissor-Clipped to the
  panel, wheel scrolling clamped to the content height.
  Left-click selects (feeding the Inspector); right-click selects
  *and* retargets the context menu. **Duplicate** copies the data
  components (Tag gains a `" copy"` suffix), adopts the source's
  parent, never copies children, and re-creates any
  `NativeScriptComponent` fresh so a script instance is never
  shared; **Create Child** parents a new entity under the target;
  **Delete** delegates to `Scene::destroyEntity`, so the scene's
  `HierarchyDestroyMode` decides the children's fate (orphaned to
  roots by default, destroyed in Cascade mode). A selection that
  goes stale (deleted directly or via an ancestor) is cleared on
  the next build.
- **Unified editor layout** (`Application`): the inspector dock
  (top-right) reflects the Outliner's selection over every
  reflected built-in (`TransformComponent`, `RigidBodyComponent`,
  `SpinComponent`, `PointLightComponent`, `SpotLightComponent`),
  stacked by measured panel height; with nothing selected it
  keeps the Step 3 fallback — the player ship's live
  RigidBodyComponent. The hierarchy panel docks left below the
  HUD backdrop and builds last, so its context menu always
  paints above the dock. Right button, Esc and wheel input are
  wired from the platform `Input` alongside the existing keys.
- **Verification**: `testPhase27Editor()` adds 36 checks —
  right-click dispatch (edge-only), menu item firing, Esc
  dismissal, outside-click consumption (guarded button stays
  inert, next honest press clicks), popup replace semantics,
  disabled items, and the full outliner workflow over real
  scenes (sorted selection, Duplicate component fidelity and
  parent adoption, Create Child linkage, retargeted Delete,
  orphan-vs-cascade destroy modes, wheel clamp). **897 passed,
  0 failed** in Debug, Release (shared and static),
  hidden-visibility (Windows-symbol simulation) and ASan
  builds, warning-free, CTest 1/1.

## Phase 28 — 3D Raycasting & Transform Gizmos ✅

The editor becomes spatial: clicking in the viewport selects
entities through the active camera, and the selection grows
procedural 3D gizmos — translation arrows, rotation rings, scale
handles — that edit the entity's `TransformComponent` live under
the mouse.

- **Screen-to-world raycasting**: the picking chain is
  `pickEntityAt` (`Scene/Raycast`) — viewport pixel →
  `screenToNdc` → `cameraRay` (unprojection through the active
  camera's view/projection, distances measured from the eye) →
  `Scene::raycast`, which walks the BVH broadphase and tests
  box/OBB colliders exactly in local space with mesh-bounds
  fallback. The nearest hit wins; a transform-only entity is
  transparent to the ray. This also retires the orphaned Phase 13
  picking stub: selection is now the single editor selection
  (outliner + inspector + gizmo all read it), and the Phase 17
  "Impulse" action (F) applies to whatever the editor has
  selected.
- **TransformGizmo** (`Scene/TransformGizmo`, EngineScene):
  immediate-mode interaction over `UIContext`. Handles are
  picked in screen space (cursor distance to the projected
  handle geometry, 12 px threshold, deterministic axis
  priority), drags are exact 3D constructions — the closest
  point between the mouse ray and the axis line for
  translate/scale, ray vs ring-plane angles for rotate. Axis
  constraint is absolute: a translate freezes the other two
  position components bit-for-bit, a rotate touches one Euler
  angle (and re-syncs the orientation quaternion so
  `modelMatrix` honors it), a scale clamps into
  [0.01, 100] so a drag can never invert an entity. Drags lock
  their target at press time; switching tools (toolbar or keys
  1/2/3), opening a menu, capturing the mouse, or deleting the
  target mid-drag all cancel cleanly.
- **Reflection-instant edits**: drag results are written
  straight into `TransformComponent` — the exact fields the
  reflection-driven Inspector re-reads every frame — so the
  dock, the gizmo and the rendered entity move as one.
- **Procedural rendering**: true 3D handle geometry (axis
  segments, 64-sample rings, length proportional to camera
  distance) projected through the same view-projection and
  emitted as dotted SDF lines/tip glyphs into the UI overlay
  batch, so the gizmo always renders on top of the scene.
- **Editor glue** (`Application`): the gizmo updates after the
  outliner (context menu still paints last-but-one over docks);
  viewport picking fires only for presses the UI left
  unconsumed — outside the HUD, toolbar, inspector and
  outliner rects, with no menu open at frame start and no
  widget mid-drag — and clicking empty space deselects.
- **Verification**: `testPhase28Gizmos()` adds 40 checks —
  NDC mapping corners, ray/AABB slab cases (front hit, miss,
  behind, inside origin), projection analytics + a
  project→pick round-trip, axis-parameter math (incl. the
  parallel fallback), nearest-hit picking through occluders,
  and full drag state machines per tool (hover, grab,
  consume, axis-frozen deltas, reflection read-back, scale
  clamping, blocked input, mid-drag tool switch and target
  deletion, inert targets). **937 passed, 0 failed** in
  Debug, Release (shared and static), hidden-visibility and
  ASan builds, warning-free, CTest 1/1.

## Phase 29 — Asset Browser, File Management & Drag-and-Drop ✅

The editor gains its content pipeline: a docked Asset Browser
over the project folder, an immediate-mode drag-and-drop system
in `UIContext`, and drop wiring that spawns meshes into the
scene and assigns meshes/textures/shaders onto the inspected
entity.

- **Drag & drop** (`Graphics/UIContext`): `beginDragSource`
  arms a *pending* drag on press (plain clicks keep working);
  traveling 6 px with the button held promotes it to a live
  drag with a labeled ghost following the cursor, drawn last
  in `endFrame`. `dropTarget(rect, type)` delivers exactly
  once, on the release frame, to the first target whose type
  contract matches the payload and whose rect holds the
  pointer; `dropTargetHovered` drives target highlighting.
  Esc cancels mid-drag (targets go dark, the release delivers
  nowhere), and a second source can never hijack a live drag.
- **Asset Browser** (`Scene/UIAssetBrowser`): scans the
  current folder (folders first, then files, name-sorted,
  dotfiles skipped) and classifies by extension — Mesh
  (.obj/.gltf/.glb), Texture (.ppm/.png/.jpg/.tga/...),
  Shader (.spv/.vert/.frag/.glsl), Script, Module, Scene —
  rendered as minimalist colored kind chips. Click selects;
  double-click navigates folders (with a ".." row back up) or
  activates files. Payload data is always the root-relative
  path, and `resolveWithinRoot` canonicalizes and refuses
  anything escaping the root ("..", absolute paths).
- **Viewport drops** spawn content: an OBJ payload resolves
  under the asset root, imports through `AssetManager::loadObj`
  (idempotent by name), and lands via
  `Scene::spawnImportedModel` — auto-scaled, with a 1:1
  bounds collider — at the ray∩ground-plane point under the
  cursor (`dropPointOnGround`, falling back to a point in
  front of the camera), then becomes the selection.
  Double-clicking a mesh file spawns it through the screen
  centre. Recognized-but-not-yet-importable formats (glTF)
  are refused with a warning, never silently.
- **Inspector drops** assign: Mesh → the entity's
  `MeshComponent` asset (created if missing) with matching
  local bounds; Texture → declares + async-loads the texture
  and derives a new material (`drop_<texture>`) carrying it
  as albedo, so the registry's immutable materials are never
  mutated in place; Shader (.vert.spv/.frag.spv) → the same
  derived-material route with the shader slot swapped.
  Payloads with no inspector meaning (scripts) are never
  accepted by any target — type contracts are the validation.
- **Sample content**: `assets/` ships a starter project
  (OBJ cube, PPM checker, GLSL sample, script) and is copied
  next to the executable on every build; the browser opens
  on it. The browser dock sits along the bottom edge and is
  part of the picking occluder set, so drags never fall
  through to viewport selection.
- **Verification**: `testPhase29Assets()` adds 33 checks —
  classification tables, scan ordering, root containment,
  ground-point math, the full drag state machine (pending,
  promotion, single delivery, type contracts, off-target
  release, Esc cancel, hijack refusal), browser
  navigation/activation against a real temp tree, and the
  import→spawn chain. **970 passed, 0 failed** in Debug,
  Release (shared and static), hidden-visibility and ASan
  builds, warning-free, CTest 1/1.

## Phase 30 — Scene Serialization & Custom Binary Format (.gbq) ✅

The Phase 19 JSON scene format gains a fast binary twin: the
**GBQ1 container** — a protected, index-addressed file for
whole-scene persistence with bit-exact floats and no text
parsing on load, plus editor Save/Load commands.

- **Container format** (`Scene/SceneBinarySerialization`):
  a 32-byte header (magic `GBQ1`, format version, flags,
  section count, TOC offset) followed immediately by a
  **table of contents** — one 32-byte entry per section
  (type code, section version, absolute offset, size) so any
  section is located in O(1) without scanning the file.
  Sections are 8-byte aligned, fixed-layout little-endian
  records; strings are u32-length-prefixed byte runs, floats
  travel as raw IEEE bits. `inspectGbqFile()` reads only the
  header + TOC (version, entity count from META, section
  list) for tooling that must peek without loading.
- **Coverage**: META (destroy mode + entity count), ENTY
  (parent indices), TAGC, TRNF (incl. quaternion orientation
  mode), CAMR (full `CameraConfig` + live position/yaw/pitch),
  MESH (asset/material/bounds), SPIN, RGBD (full dynamics
  state incl. inertia tensors and sleep flags), CLDR (all six
  collider shapes, hull points, trigger + layer/mask), GRVF,
  JONT, THRS — plus **PLGT/SLGT lights and NSCR native script
  names**, which the JSON format never stored. Scripts persist
  by name only; the script system recreates the live instance,
  which is runtime state. Transient physics forces are not
  stored (same rule as JSON). Entities are referenced by file
  index only — recycled EnTT handles are never persisted.
  Unknown sections are skipped (forward compatibility); a
  known section at an unexpected version, a bad magic, any
  truncation, or an out-of-range entity/joint/parent index
  rejects the whole load.
- **Loader contract**: the file is staged into plain structs
  and fully validated *before* the scene is touched — malformed
  input leaves the workspace byte-for-byte unchanged. On
  success the registry is cleared silently (no destroy events)
  and rebuilt in file order; hierarchy is re-linked through
  `setParent`, and joint `other` links are remapped in a
  second pass once every handle exists. Camera components are
  reconstructed through `Camera::setPosition/setOrientation`,
  exactly like the JSON path.
- **API**: `serializeScene(scene, path)` /
  `deserializeScene(scene, path)` free functions, and
  `Scene::saveToGbqFile` / `Scene::loadFromGbqFile` members
  mirroring `saveToFile`/`loadFromFile`. The JSON serializer
  is untouched; both formats describe the same scene.
- **Editor integration**: a **Save / Load** toolbar segment
  beside the gizmo tool switch and **Ctrl+S / Ctrl+O**
  shortcuts round-trip `assets/quicksave.gbq`. Load drops the
  editor selection first so the inspector and gizmo never
  chase a recycled handle, and logs the outcome either way.
- **Verification**: `testPhase30Gbq()` adds 30 checks — TOC
  structure, binary-smaller-than-JSON, bit-for-bit round-trip
  of transforms/quaternions/camera/rigid-body/hull/lights/
  script name, nested hierarchy rebuild, joint link remapping
  in both directions, transitive save→load→save→load stability,
  JSON parity, and five malformed-input rejections that must
  leave the scene untouched. **1000 passed, 0 failed** in
  Debug, Release (shared and static), hidden-visibility and
  ASan builds, warning-free, CTest 1/1.

## Phase 31 — Play / Stop Mode (Runtime Simulation) ✅

The editor gains the classic edit/play split: the scene opens
frozen in **Edit** mode, **Play** runs the live simulation
against a throwaway state, and **Stop** restores the authored
scene exactly as it was.

- **Snapshot** (`Scene/PlaySession`): `enterPlay()` captures
  the entire scene as **in-memory GBQ bytes**
  (`Scene::saveToGbqBytes()`, the Phase 30 serialization core
  with no file in between — a test verifies the snapshot is
  byte-identical to the `.gbq` file form). All component
  states, hierarchies, rigid bodies, joint links and native
  script bindings are secured before any runtime code runs.
  `saveToGbqBytes`/`loadFromGbqBytes` (plus
  `serializeSceneToBytes`/`deserializeSceneFromBytes` free
  functions) are the new in-memory API; the file functions
  are now thin wrappers over them.
- **Runtime gating**: in Edit mode the fixed step propagates
  transforms only — game modules, `NativeScriptSystem`
  (`INativeScript`), `PlayerSystem`, rigid-body physics and
  spin stay frozen. Play mode runs the full pipeline (module
  and script updates also ungate in the variable update);
  module hot-reload polling stays live in Edit so code
  changes land while authoring. The camera fly controls work
  in both modes.
- **Editor lockdown in Play**: the transform gizmo is never
  updated (nothing draws or drags) and raycast picking is
  blocked through the same gate; the Inspector and Outliner
  (whose widgets and context menus edit the scene) are not
  built; the Asset Browser is not built, so no drops can
  spawn; tool keys, toolbar authoring buttons and Ctrl+S/O
  are ignored (the buttons still draw). Stop is always
  reachable.
- **Controls**: a **Play / Stop** toolbar segment beside
  Save/Load (accent underline + red Stop tint while playing,
  PLAY MODE hint line) and **Ctrl+P** to toggle. Entering
  Play clears the editor selection and re-captures physics
  spawn states; the C-key respawn is a runtime-only action.
- **Restoration**: `exitPlay()` feeds the snapshot back
  through the validating GBQ loader — the runtime may move,
  spawn and destroy entities freely; Stop rebuilds the
  authored scene bit-for-bit (new EnTT handles, so the
  selection is cleared rather than left dangling). Guards:
  entering twice is refused, Stop while editing is a no-op,
  and a failed restore (impossible for a self-produced
  snapshot, but defended) keeps the session in Play rather
  than presenting a mutated scene as edit state. Snapshot
  memory is released on Stop.
- **Verification**: `testPhase31PlayMode()` adds 22 checks —
  state-machine guards, snapshot↔file byte identity, a live
  150-step physics run inside Play (body falls and lands),
  runtime destruction/spawn/edit mutations, bit-exact
  restoration of transforms, hierarchy, joint links, lights'
  hosts, script bindings and camera, a second-cycle
  re-snapshot, and the empty-scene round trip. **1022
  passed, 0 failed** in Debug, Release (shared and static),
  hidden-visibility and ASan builds, warning-free, CTest
  1/1.

## Phase 32 — Advanced Materials & PBR: Directional Light Entity, Spot Shadows, Live Material Editing ✅

Phase 22's Cook-Torrance pipeline (GGX/Smith/Schlick for the
cascaded key light and 16 dynamic lights, metallic-roughness
maps incl. normal/AO, split-sum IBL) gains its missing
author-facing pieces: the sun becomes an ECS entity, one spot
light casts real-time shadows, and materials become editable
live in the Inspector.

- **DirectionalLightComponent** (new ECS component): `color`,
  `intensity`, `direction` (toward the light), `enabled`,
  `castShadow`, `cascadeCount` (1–4). `LightingSystem::gather`
  lets the brightest enabled sun drive the key light; with no
  sun entity the legacy fixed sun (direction 0.35/0.8/0.45,
  warm tint, radiance 1) answers identically, so old scenes
  don't change. Cascade splits/matrices past the count pin to
  the last slice. The component is reflected (Inspector-ready),
  registered in the reflection registry, primed in Scene
  storage, and serialized in `.gbq` (new DIRL section).
- **Spot shadows**: `SpotLightComponent::castShadow` opts a
  spot into the one budgeted 1024² shadow map. Gather picks
  the nearest shadow-casting spot *after* the nearest-16 sort
  (a shadow spot outside the light budget casts none) and
  computes its perspective light-space matrix
  (`spotShadowViewProjection`, FOV = outer cone, Vulkan clip
  conventions). The Renderer renders it through the existing
  depth pipeline (push index 4) into a dedicated target bound
  at descriptor 7; `cube.frag` reprojects fragments
  (perspective divide + 3×3 PCF) only for the owning light.
  Point lights stay shadowless by design (cube shadows = six
  passes per light).
- **UBO discipline**: `directionalColor` (rgb tint, w =
  cascade-shadow toggle), `spotShadowViewProj` and
  `spotShadowParams` are APPENDED to the uniform block — every
  pre-existing offset is byte-identical in C++ and all three
  shaders (1568 bytes now; the layout test pins the offsets).
- **Live material editing**: `AssetManager::updateMaterial`
  replaces a registered material's description in place (id
  and descriptor slot stable, new `materialRegistryVersion`
  counter bumped; unknown names refused). RenderSystem
  rebuilds draw items every frame, so edits land in the very
  next frame. The editor's Inspector shows a **Material
  panel** for mesh entities (base color, metallic, roughness,
  occlusion strength sliders) committing through that API,
  next to the new DirectionalLight panel and the SpotLight
  Cast Shadow toggle. The demo scene gains a `Sun` entity
  reproducing the legacy look, and its floor spot now casts
  shadows.
- **Format evolution**: DIRL is a new section (old readers
  skip it); SLGT grows to section version 2 (trailing
  castShadow byte) while v1 files still load with the flag
  defaulted off — the first exercised use of the GBQ
  per-section versioning. Play-mode snapshots (Phase 31)
  inherit both.
- **Verification**: `testPhase32Pbr()` adds 25 checks —
  legacy-vs-entity sun selection and cascade pinning, spot
  shadow ownership/indexing and light-space projection,
  spot VP math, material update/version/slot stability,
  draw-item PBR + four-texture binding, live re-edit flow,
  GBQ directional/spot round-trip, and a byte-downgraded
  SLGT v1 fixture that still loads. **1047 passed, 0 failed**
  in Debug, Release (shared and static), hidden-visibility
  and ASan builds, warning-free, CTest 1/1; all shaders
  compile with glslangValidator 15.4.

## Phase 33 — Standalone Packaging & Game Export ✅

The editor can now ship what it authors: **Export Game**
distills the live scene and every asset it references into one
compressed, checksummed deployment bundle, and a new
editor-free `game-player` binary boots that bundle straight
into the game.

- **`ExportProject()`** (`Scene/ProjectExport`, in
  EngineScene): walks the scene's Mesh/NativeScript
  components, resolves every referenced mesh, material,
  texture and shader-pair in the AssetManager, and fails
  *atomically* — listing every unregistered reference by
  name, writing nothing — rather than shipping an incomplete
  game. The bundle (new `GBQB` v1 container) carries the
  scene as GBQ bytes plus mesh geometry (counts + raw
  `Vertex`/index bytes), RGBA8 texture pixels, SPIR-V words,
  material descriptions (JSON, inverse of the loader) and
  native-script *name records* — scripts are compiled code,
  so the bundle records which classes the build must
  provide. Payloads are LZ77-compressed (flag-byte LZSS,
  12-bit window, no external zlib dependency — the sandbox
  has none) and CRC-32-checksummed; a JSON manifest inside
  the bundle (name/kind/sizes/offset/CRC per entry) is
  mirrored to `<name>.manifest.json` next to a generated
  `game.ini` (`[Game] bundle`, `[Window]` defaults).
  Exports are deterministic: same scene, byte-identical
  bundle (Scene first, then kind/name-sorted entries,
  8-byte-aligned blobs).
- **`game-player`** (`src/Standalone/StandaloneMain.cpp`):
  the ship build's entry point — links every engine
  library *except* EngineApp, so no outliner/inspector,
  gizmo, asset browser or editor system is even present in
  the binary. `loadBundleIntoRuntime()` (shared with the
  tests) registers bundle assets into a fresh AssetManager
  and rebuilds the scene; missing native script classes
  log an error and boot degraded, exactly like a missing
  game module. `--headless <frames>` is the CI smoke
  test: load, run N fixed steps (Transform → Physics →
  Rotation → Transform), verify every transform stayed
  finite, print a summary, exit 0/1 — no window, no GPU.
  Windowed mode mirrors the editor's bring-up minus the
  editor (window/input/Vulkan/pipelines/GameLoop, game
  modules with hot reload, native scripts, CameraSystem),
  extracts bundle SPIR-V into the shader directory when
  the install doesn't provide it, and falls back to the
  built-in cube + sandbox palette for assets the bundle
  doesn't override. `[Game] bundle` is now a first-class
  EngineConfig key, so `--config game.ini` alone boots an
  exported game.
- **Editor**: an **Export** segment joins the toolbar
  after Play/Stop, exporting the live scene to
  `assets/export/game.gbq` (+ manifest + `game.ini`); it
  is inert in Play mode, so bundles always ship the
  authored world.
- **Verification**: `testPhase33Export()` adds 37 checks —
  CRC-32 known vector, export completeness by entry kind,
  scene/mesh/texture payload round-trips, compression of
  flat textures, tamper → CRC refusal (extract *and*
  boot), garbage/missing file refusal, atomic missing-
  reference failure, byte-identical re-export,
  `loadBundleIntoRuntime` asset/script resolution, and 90
  simulated steps bringing the exported crate to rest on
  the floor. `game-player --bundle <fixture> --headless
  120` exits 0 (also under ASan); tampered/missing
  bundles exit 1. **1084 passed, 0 failed** in Debug,
  Release (shared and static), hidden-visibility and ASan
  builds, warning-free, CTest 1/1.

## Maintenance — Windows scene-teardown segfault fix ✅

A MinGW x64 crash in `testPhase24SpaceFlight` teardown
(`std::_Sp_counted_base::_M_release` from `Scene::~Scene`)
traced to undefined teardown ordering, invisible on Linux
for a subtle reason: glibc refuses to actually unmap a
module whose TUs include EnTT headers (the EnTT meta
locator's `__cxa_atexit` entanglement marks it NODELETE —
verified empirically: segments stay mapped after
`dlclose`), while Windows `FreeLibrary` unmaps for real,
exposing any host-side teardown that reaches module-owned
code late. The concrete engine defects, both reproduced
on Linux:

- **`~Scene()` leaked live native-script instances.**
  `~Scene() = default` let EnTT destroy pool storages
  directly, which never publishes the `on_destroy`
  destruction sink — so the NativeScriptSystem hook that
  `onDestroy()`s and deletes module-created script
  instances never ran at scene teardown (probe: instance
  alive after `~Scene`, no `onDestroy`). `~Scene` now
  destroys every entity first via `registry.clear()`
  (whose `pop_all` publishes the sinks) and is out-of-line.
- **Instance deletion could be sequenced after module
  unload.** Deleting a script instance calls module code
  through its vtable; the editor never shut scripts down
  at exit, and its member order retires the module before
  the scene dies. `Application` and the standalone player
  now call `NativeScriptSystem::shutdown` up front in
  their destructors, and the module contract (module
  `onUnload` retires instances → host shutdown → `~Scene`
  as a loaded-module-only safety net; modules always
  retire before their scene is destroyed) is documented
  on `IGameModule` and `Scene`.

`testSceneTeardown()` adds 12 checks: the leak fix,
no double-destroy after explicit shutdown, and both
contract teardown orders against the real flight module.
**1096 passed, 0 failed** in Debug, Release (shared and
static), hidden-visibility and ASan builds, warning-free,
CTest 1/1.

## Maintenance — MinGW link fix: `SdfFontAtlas::glyphFor` export ✅

`engine-core-tests` failed to link on Windows/MinGW with
`undefined reference to engine::SdfFontAtlas::glyphFor(char)
const`: the definition existed and compiled into
EngineGraphics, but the declaration carried no
`ENGINE_GRAPHICS_API` annotation — the lone un-annotated
out-of-line member among everything the test binary imports
(the header's own free functions and every comparable type —
`AABB`, `Cube`, `Camera`, `Scene`… — are all annotated).
ELF default visibility masked it on Linux; the annotation
is now on the declaration (function-level, matching the
`ProjectExport` precedent for STL-membered data structs and
avoiding any new MSVC C4251 surface). Verified in the
sandbox by compiling the TU with `-fvisibility=hidden`:
pre-fix the symbol is withheld from the dynamic table and
a consumer reproduces the exact undefined-reference error;
post-fix it exports and the consumer links and runs.
1096 passed / 0 failed across all five build configs,
CTest 1/1.

## Maintenance — Bounded editor viewport, TrueType UI font, free-cursor editor ✅

A user screenshot showed the editor rendering the 3D scene
full-window underneath translucent floating panels, blocky
all-caps bitmap text, and a cursor that booted straight into
game-style mouse capture. All three are fixed:

- **Docked layout, bounded viewport** (`Graphics/EditorLayout.*`):
  one pure-CPU `EditorLayout` per frame, computed from the live
  framebuffer size — Toolbar on top, Scene Hierarchy left,
  Inspector right, Asset Browser bottom, and the 3D scene
  strictly inside the central Viewport rect. The renderer takes
  that rect (`Renderer::setSceneViewport`) and applies it as
  the Vulkan viewport/scissor for the scene pass *and* the
  HDR composite; camera aspect, mouse picking rays, gizmo
  projection/dragging, and drag-and-drop targets all consume
  the same rect. In Play mode the docks collapse and the
  viewport takes their space. Layout is recomputed every
  frame, so resizing reflows immediately (pinned by
  `testEditorViewportAndFont` at 1280×720, 1920×1080, 800×500,
  and 0×0).
- **Real TrueType font** (`Graphics/SdfFont.*`, vendored
  `stb_truetype` v1.26): the UI atlas is now baked from an
  actual sans-serif — `ENGINE_UI_FONT` env override, then a
  system font (Segoe UI / Helvetica / DejaVu Sans…), then the
  bundled `assets/fonts/DejaVuSans.ttf` — as a 48 px SDF page
  with per-glyph advances/bearings, a 95×95 kerning table, and
  baseline-aligned, proportional, mixed-case text (the legacy
  5×7 bake remains only as the last-resort fallback atlas).
  Malformed font data is rejected before stb sees it (stb does
  no bounds checking of its own), the ESDF cache format is
  v2, and the negative-control/GCC-false-positive wrinkles are
  covered by tests.
- **The editor never captures the mouse**: `captureMouseOnStart`
  now defaults to `false`; capture exists only as a deliberate
  Play-mode click *inside the viewport rect* (RMB look,
  WASD fly). Clicks on any dock never capture, Esc releases
  instead of quitting, and focus loss hands the cursor back.
  The standalone player is untouched.

1112 passed / 0 failed across all five build configs, CTest 1/1,
zero warnings.

## Maintenance — Composite alignment, text solidity, selection outline ✅

Four editor defects from a user screenshot, one of them a genuine
rendering bug that invalidated the previous maintenance pass's
"no offset math needed" assumption:

- **Rendered world displaced from picking/gizmo**: the composite
  pass narrowed the Vulkan *viewport* to the scene rect while
  `post.vert` derives UVs from NDC — so the composite stretched
  the whole HDR image across the viewport panel while the scene
  pass had written the world only into the panel's own texels.
  The world rendered squashed and offset inside the panel while
  picking rays, gizmo projection and the camera all assumed a
  1:1 panel mapping. The composite now keeps a full-extent
  viewport and confines itself with the scissor only: panel
  pixel (x, y) shows scene texel (x, y), the mapping every other
  system already used. A CPU round-trip test (project a cube's
  centre through the docked rect, pick at that pixel, expect the
  cube) pins the contract.
- **Text fading/clipping**: the SDF shader now thresholds in
  *screen-space* distance (`(field-0.5)/fwidth(field)`, AA over
  one pixel) instead of a ±fwidth band that left thin strokes
  permanently grey at editor text sizes; the font atlas carries
  a mip chain (blit-generated, feature-gated) so the gradient
  estimate stays clean at 4x minification; `UIRenderer` staging
  buffers are per-frame-slot, ending the CPU/GPU race that
  corrupted glyphs at random; and the Inspector's component
  panels now nest inside the dock panel, so its scissor — not
  each panel's own rect wherever it landed — is the clipping
  authority.
- **Flush viewport**: docks tile edge-to-edge (no gutters, no
  drawn frame around the viewport); the viewport reclaims the
  margin pixels and meets its neighbours surface-to-surface.
  Dock panels opt out of the rounded/bordered house style via a
  negative-style sentinel in `beginPanel`.
- **Selection outline + deselection**: the selected entity gets
  an amber wire box around its exact pick volume (mesh bounds ∪
  box/OBB collider, fallback box for bounds-less entities),
  projected through the same `projectToScreen` path as the
  gizmo. Empty-space viewport clicks already deselected, but
  the displaced rendering made them land on geometry; with the
  composite fixed, deselect works as written and is test-pinned.

1122 passed / 0 failed across all five build configs, CTest 1/1,
zero warnings.

## Maintenance — Editor usability: local-frame gizmo, inspector ranges/scrolling, context menus, dark theme ✅

Nine reported editor defects, fixed as one pass over the gizmo,
inspector, hierarchy/viewport menus and theme:

- **Smeared SDF text**: the batch, vertex layout and shaders
  audited clean (now pinned by CPU integrity tests: per-string
  quad/index accounting, segment partitioning, attribute
  offsets == `offsetof`, glyph rects never overlapping). The
  real lever was the atlas: neighbouring glyphs sat 2 texels
  apart, so mip footprints at editor text sizes averaged a
  neighbour's ink into each glyph's halo — the white streaks.
  Shelf packing now keeps an 8-texel moat (pinned for TrueType
  atlases).
- **Inspector ranges**: Transform fields get editor ranges —
  Position ±5000, Rotation ±3600°, Scale 0.001..1000 — instead
  of the generic ±10 component clamp; other types keep the
  caller's range. Holding Shift turns any slider drag into a
  slow relative drag (0.05% of range per pixel, no jump on
  grab) and shrinks focused arrow steps 20×.
- **Gizmo cross-talk & grab jumps**: handles now live in the
  target's *local* frame (directions from its world matrix,
  scale stripped), drags are incremental frame-to-frame deltas
  applied to the live transform, and translations write back
  through the parent's inverse world matrix. After a 90°
  rotation the X handle still drives exactly local X — never
  Y/Z — and the grab frame always measures a zero delta, so
  nothing snaps on frame one. Unrotated drags telescope to the
  exact analytic totals the old absolute math produced.
- **Inspector scrolling**: the wheel over the Inspector dock
  shifts the panel stack (3 rows per tick), clipped by the
  dock's nested scissor; the offset clamps against last frame's
  measured content height and resets on selection change.
- **Context menus**: the Hierarchy menu gains Rename, Reset
  Transform and Focus next to Duplicate/Create Child/Delete;
  a right *click* (not drag — camera look is untouched) in the
  3D viewport picks the entity and raises the same menu.
  Rename edits the tag in a new UIContext textField fed by a
  GLFW char callback (`Input::onCharEvent` → UTF-8 → UIInput);
  Enter commits, Esc restores, and editor shortcuts go inert
  while typing. Focus frames the selection box with the active
  camera, orientation preserved.
- **Dark theme**: charcoal surfaces (#252526 panels, #1e1e1e
  tracks), #3f3f46 borders, #007acc/#0e639c blue fills, orange
  focus borders, active gizmo handles and selection outline;
  the Hierarchy selection is #094771.
- **Save/Export feedback**: both were wired but only logged;
  they (and Load + every context action) now post a 4-second
  status-bar notification with the file/result, and the editor
  tracks the active scene path.

1154 passed / 0 failed across all five build configs (Debug,
Release shared/static, hidden-visibility, ASan), CTest 1/1,
zero warnings, standalone player headless run clean.

## Maintenance — Culling winding, editor fly camera, true deep-dark theme, SDF text audit ✅

Four critical rendering/UI fixes:

- **Inside-out meshes (backface culling)**: the material,
  shadow and skybox rasterizers declared
  `VK_FRONT_FACE_CLOCKWISE` on the theory that the
  projection's Y-flip reverses winding. It does — and the
  positive-height viewport reverses it again, so front faces
  arrive counter-clockwise in framebuffer space and CLOCKWISE
  made back-face culling discard exactly the visible faces
  (cubes looked hollow, walls see-through). All pipelines now
  share `kEngineFrontFace = CounterClockwise`
  (PipelineState.h); the shadow pass still renders back faces
  into the depth map and the skybox still renders the inside
  of the sky cube. A regression test projects the real cube
  through the real camera projection and asserts exactly the
  two camera-facing triangles classify front-facing.
- **Editor fly camera**: holding the right mouse button in
  the 3D viewport now flies as well as looks — WASD along the
  camera basis, Q/E on the world vertical, Shift sprints
  (`Camera::processMovement`, previously Play-mode only).
  The Camera's exponential look/velocity smoothing carries
  both, so releasing the button glides to a stop. Inert while
  a rename text field owns the keyboard.
- **True deep-dark theme**: #121212 chrome base, a dedicated
  #202020 toolbar/status fill, #181818 tracks and text
  fields, panels stay #252526, borders and hover lift are
  #333333, and the near-white slider thumb is de-glared to
  mid grey. Text and the blue/orange accents are unchanged.
- **SDF text audit**: the whole pipeline — TrueType bake,
  UIBatch quad/UV generation, atlas upload, shader sampling —
  was audited (vertex layout/stride, buffer upload, mip
  chain, scissor segments) and is now locked by an
  end-to-end regression test that software-renders "Hello"
  through the real batch and atlas exactly as ui.frag
  samples it: glyph rects ordered and in-page, the field
  below the edge value within 3 texels outside every rect
  (no neighbour bleed), all five glyphs inked, no stray
  coverage in empty canvas.

1172 passed / 0 failed across all five build configs (Debug,
Release shared/static, hidden-visibility, ASan), CTest 1/1,
zero warnings, standalone player headless run clean.

## Maintenance — Dock partition lock, deep-black theme, larger UI text, SDF editor-scale lock ✅

Four editor layout/theme/font fixes:

- **Dock bleed / overlap**: `computeEditorLayout` already
  partitioned the window, but nothing proved it at every
  size. The suite now property-tests nine window sizes from
  1920×1080 down to 64×64 (plus Play mode and a degenerate
  0×0): all six docks are pairwise disjoint, contained in
  the window, and the Hierarchy/Inspector/Asset
  Browser/Viewport tile the middle band with zero gaps and
  zero overlap. A second test binds clipping to the layout:
  geometry emitted inside a dock carries exactly the dock's
  scissor segment (nested scissors only ever intersect), so
  panel children can never paint outside their dock.
- **Deep-black theme**: the palette dropped from the
  #202020–#252526 band to near-black — panels #121214,
  chrome base #0f0f11, toolbars/status #161618, tracks and
  fields #18181c, borders/hover #2a2a2e — with text pinned
  to #e0e0e0 so it pops against the dark surfaces. The
  blue/orange accents are unchanged.
- **UI font size**: the global text scale `kTextScale`
  rises 1.5 → 1.65 (+10%). Widget text stays vertically
  centred by the atlas line box, and a regression check
  pins that one line at the new scale still fits the fixed
  26 px rows, so no bounding boxes move.
- **SDF text**: third end-to-end audit (bake → quad/UV
  generation → vertex/index emission → mip chain → shader
  threshold), now locked at the real operating point: the
  suite software-renders "Hello" at `kTextScale` through
  the trilinear mip chain with ui.frag's exact fwidth AA
  and requires per-glyph ink with a thin AA fringe, and a
  second test pins emitted glyph quads attribute-exact
  (uvRects verbatim from the atlas, ordered corners,
  exact index tuples — no stride shift or cross-quad
  corruption is possible without failing the suite).

1194 passed / 0 failed across all five build configs
(Debug, Release shared/static, hidden-visibility, ASan),
CTest 1/1, zero warnings, standalone player headless run
clean.

## Maintenance — Bitmap text replaces SDF, swapchain gamma fix (washed-out UI root cause) ✅

Two rendering changes, one of them the actual cure for the
"UI is still light grey" reports that survived three palette
reworks:

- **Washed-out theme was a gamma bug, not a palette bug**:
  the swapchain preferred `VK_FORMAT_B8G8R8A8_SRGB` while
  the composite shader already encodes linear→sRGB itself
  (ACES + `linearToSrgb`). Every store to the swapchain was
  therefore encoded twice — near-black theme values lifted
  ~4× on screen (#121214 displayed as light grey, the
  tone-mapped scene went pastel), and no palette change
  could ever fix it. `Swapchain::chooseSurfaceFormat` now
  prefers `B8G8R8A8_UNORM` (then `R8G8B8A8_UNORM`), so the
  composite's encoding and the UI's authored hex colours
  reach the display exactly once-encoded. The deep-black
  palette (#0f0f11/#121214/#161618/#18181c/#2a2a2e, text
  #e0e0e0) now displays as specified; no code path calls
  `setTheme`, so nothing can reset it.
- **SDF text is gone; bitmap coverage text replaces it**:
  the screenshot showed the SDF failure mode precisely —
  "Floor" as "Foor", "Position" as "Posit on": at editor
  sizes, trilinear filtering of a nonlinear distance field
  averages thin-stroke interiors below the 0.5 threshold
  and the strokes vanish. `SdfFont` is replaced wholesale
  by `BitmapFont`: stb_truetype rasterizes coverage
  (`stbtt_GetCodepointBitmap`) at a 20 px target em into an
  R8 atlas (metrics/kerning/baseline layout unchanged, blob
  format is now GBMF v1, the 5x7 fallback bakes coverage
  too), `ui.frag` samples coverage as alpha with a 0.1
  cutout (no distance field, no fwidth threshold), and
  `UIBatch::drawText` snaps glyph quad origins to whole
  pixels so stems land on pixel boundaries. Coverage is
  linear, so mip filtering preserves ink instead of
  erasing it. Tests were updated to the bitmap contract
  (solid-coverage glyph rects, zero-coverage 2-texel gap,
  coverage-based legibility renders at scale 3 and at
  `kTextScale`, snapped-position expectations).

1194 passed / 0 failed across all five build configs
(Debug, Release shared/static, hidden-visibility, ASan),
CTest 1/1, zero warnings, standalone player headless run
clean.

## Maintenance — Larger crisper bitmap text, pointer-locked viewport fly-look ✅

- **Text size & clarity**: the global UI text scale rises
  1.65 → 1.8 and the bitmap bake rises 20 px → 26 px em
  (`kBitmapFontBakePixels`), keeping the bake comfortably
  above the ~18 px on-screen em so coverage minifies
  slightly rather than magnifying. Glyph quad origins are
  snapped with `std::round` (half away from zero, exact for
  the negative origins of scrolled/clipped panels) instead
  of the previous `floor(x + 0.5f)`. A 26 px-row still fits
  a full line at the new scale (verified in the suite), so
  no widget bounding boxes moved.
- **Pointer lock for the viewport fly-look**: Edit mode used
  to forbid cursor capture outright, so an RMB look-drag
  died at the window border. `handleMouseCapture` now owns
  a fly-capture state: an RMB press inside the viewport
  engages capture (`Input::setMouseCaptured` →
  `glfwSetInputMode(GLFW_CURSOR_DISABLED)`, cursor hidden
  and locked, look driven by raw incremental deltas — the
  capture toggle already zeroes the delta baseline, so
  there is no first-frame jump), and releasing the button
  restores `GLFW_CURSOR_NORMAL` and a live UI. Play mode's
  click-to-capture is unchanged and never inherits the fly
  state. The viewport right-click context menu keeps
  working: its press bookkeeping is no longer gated on
  `isMouseCaptured()` (the fly capture engages on the press
  frame; the existing <6 px click-vs-drag test is what
  separates a menu click from a fly drag). A headless Input
  test pins the capture delta contract.

1202 passed / 0 failed across all five build configs
(Debug, Release shared/static, hidden-visibility, ASan),
CTest 1/1, zero warnings, standalone player headless run
clean.

## Maintenance — Viewport/scissor confinement: scene, gizmos, wireframes clipped to the panel ✅

- **Gap closed**: the HDR scene pass and the composite were
  already confined to the docked "Viewport – Scene" rect
  (scene pass: `VkViewport` + `VkRect2D` from
  `effectiveSceneViewport`; composite: scissor-only, preserving
  the 1:1 panel-pixel ↔ scene-texel mapping), but the
  transform gizmo and the amber selection outline are
  **UI-overlay draws** — outside the scene pass, so its scissor
  never covered them. Any handle tip, rotate ring, or outline
  edge projecting past the panel edge painted wire pixels
  straight onto the Hierarchy/Inspector docks.
- **Gizmo + wireframe confinement**: `TransformGizmo::update`
  now wraps its whole rendering block, and
  `drawSelectionOutline` its edge/dot submission, in
  `UIBatch::pushScissor(viewport…)` / `popScissor()` — the same
  mechanism the text fields and popup menus already use, which
  `UIRenderer::record` turns into a per-segment `VkRect2D`.
  Projected geometry is left untouched; the GPU hard-clips it
  at the panel boundary. Play mode (full-window viewport) is a
  no-op clip.
- **Resolver hardening**: the confinement clamp is now the
  pure, testable `engine::resolveSceneViewport(rect, extent)`.
  A fully off-screen or clamped-to-zero request collapses to a
  zero-size rect (scene shows nothing) instead of the previous
  silent fallback to the **full framebuffer**, which splashed
  the world across every dock. The scene-pass viewport takes a
  harmless 1 px floor so a zero-size scissor never implies a
  possibly-invalid 0-size `VkViewport`.
- **Diagnostics**: `Renderer::setSceneViewport` (now out of
  line) warns once per distinct non-empty request that
  resolution had to alter — layout/framebuffer disagreement is
  surfaced instead of rendered wrong quietly. The editor's
  normal steady rect stays silent.
- **Pins**: 9 new headless checks — resolver passthrough /
  empty-request / negative-offset re-entry / edge overhang cut
  / off-screen collapse / zero framebuffer; outline and gizmo
  segments all record under exactly the panel scissor with the
  scissor stack balanced, including an oversize entity whose
  vertices genuinely leave the panel while every quad stays
  clipped.

1211 passed / 0 failed across all five build configs
(Debug, Release shared/static, hidden-visibility, ASan),
CTest 1/1, zero warnings, standalone player headless run
clean.
