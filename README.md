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
- Swapchain (sRGB format preferred, mailbox present mode with FIFO fallback),
  depth buffer, render pass, and framebuffers
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

### Run it

You should see a tumbling cube with six colored faces (red, green, blue,
yellow, magenta, cyan) on a dark background. Press **Esc** or close the window
to exit.

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
watch stderr for `[Vulkan validation]` diagnostics while developing.

## Project structure

```
.
├── CMakeLists.txt            # Build, dependencies, shader compilation
├── CMakePresets.json         # debug / release presets
├── shaders/
│   ├── cube.vert             # MVP transform (UBO) + color passthrough
│   └── cube.frag             # Flat per-face color output
└── src/
    ├── main.cpp              # Entry point (exception guard only)
    ├── Core/
    │   ├── Application.h/.cpp  # Composition root, game loop, camera + rotation
    ├── Platform/
    │   └── Window.h/.cpp       # GLFW window, surface, resize/input polling
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

- **Phase 1 — First render (current):** window, Vulkan bootstrap, rotating colored cube
- Later phases build the real engine core on top of this foundation (asset
  management, scene/entity systems, materials, lighting) — editors and visual
  tooling only arrive once the core is complete.
