cmake_minimum_required(VERSION 3.24)

project(3DEngine
    VERSION 0.1.0
    DESCRIPTION "Modern general-purpose 3D game engine (Vulkan)"
    LANGUAGES CXX C
)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Dependencies
#
# Vulkan comes from the Vulkan SDK installed on the developer machine.
# GLFW and GLM are fetched as source so a fresh clone builds with no
# manual third-party setup.
# ---------------------------------------------------------------------------
find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)

include(FetchContent)

FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE
)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
    GIT_SHALLOW    TRUE
)

FetchContent_Declare(
    enet
    GIT_REPOSITORY https://github.com/lsalzman/enet.git
    GIT_TAG        v1.3.18
    GIT_SHALLOW    TRUE
)

FetchContent_Declare(
    entt
    GIT_REPOSITORY https://github.com/skypjack/entt.git
    GIT_TAG        v3.14.0
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(glfw glm entt)

# EnTT is a third-party header library: consume its headers as SYSTEM so
# warnings inside EnTT don't break this project's zero-warning policy
# (our own code is still fully warned). The real target behind the
# EnTT::EnTT alias carries the include directories.
if(TARGET EnTT)
    get_target_property(ENTT_INCLUDE_DIRS EnTT INTERFACE_INCLUDE_DIRECTORIES)
    if(ENTT_INCLUDE_DIRS)
        # Added (not moved): compilers ignore -I for directories that are
        # also passed with -isystem, so EnTT headers become system headers.
        set_target_properties(EnTT PROPERTIES
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${ENTT_INCLUDE_DIRS}")
    endif()
endif()

# ---------------------------------------------------------------------------
# Engine modules (shared libraries / DLLs)
#
# The engine builds as layered shared libraries so iterating on one module
# does not relink (or recompile) the whole engine:
#
#   EngineFoundation  logging, time, memory pool, job system
#   EnginePlatform    GLFW window + input            (links Foundation)
#   EngineCore        camera, game loop, config      (links Platform)
#   EngineGeometry    bounds + cube geometry         (links Foundation)
#   EngineGraphics    Vulkan rendering stack         (links Platform/Geometry)
#   EngineAssets      asset/material registry        (links Graphics)
#   EngineScene       ECS scene + all systems        (links Core/Graphics/Assets)
#   EngineApp         Application composition root   (links Scene)
#
# Public symbols are annotated with per-module *_API macros (Core/Export.h:
# __declspec(dllexport/dllimport) on Windows, visibility attributes on
# ELF/Mach-O). Each module target privately defines its
# ENGINE_BUILDING_<MODULE> macro, and WINDOWS_EXPORT_ALL_SYMBOLS is enabled
# as a safety net for templates/auxiliary symbols on DLL builds.
# ENGINE_BUILD_SHARED=OFF remains available for modular static builds.
# ---------------------------------------------------------------------------
option(ENGINE_BUILD_SHARED "Build engine modules as shared libraries (DLLs on Windows)" ON)
if(ENGINE_BUILD_SHARED)
    set(ENGINE_LIBRARY_TYPE SHARED)
else()
    set(ENGINE_LIBRARY_TYPE STATIC)
endif()

function(engine_module target building_macro)
    add_library(${target} ${ENGINE_LIBRARY_TYPE} ${ARGN})
    add_library(Engine::${target} ALIAS ${target})
    target_include_directories(${target} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include/Engine)
    target_compile_definitions(${target}
        PUBLIC GLM_FORCE_RADIANS GLM_FORCE_DEPTH_ZERO_TO_ONE
        PRIVATE ${building_macro}=1)
    if(NOT ENGINE_BUILD_SHARED)
        # Static consumers must not see __declspec(dllimport) on Windows.
        target_compile_definitions(${target} PUBLIC ENGINE_STATIC=1)
    endif()
    set_target_properties(${target} PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS ON
        CXX_VISIBILITY_PRESET default
        VISIBILITY_INLINES_HIDDEN OFF
        # Game modules (Phase 24) link engine code into a shared
        # library of their own, so archives must be PIC in static
        # builds too.
        POSITION_INDEPENDENT_CODE ON)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

function(engine_copy_runtime_dlls target)
    if(WIN32 AND ENGINE_BUILD_SHARED)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_RUNTIME_DLLS:${target}> $<TARGET_FILE_DIR:${target}>
            COMMAND_EXPAND_LISTS
            COMMENT "Copying engine module DLLs next to ${target}")
    endif()
endfunction()

engine_module(EngineFoundation ENGINE_BUILDING_FOUNDATION
    src/Core/Logger.cpp
    src/Core/Time.cpp
    src/Core/Memory/PoolAllocator.cpp
    src/Core/Jobs/JobSystem.cpp
)
target_link_libraries(EngineFoundation PUBLIC Threads::Threads glm::glm)

engine_module(EnginePlatform ENGINE_BUILDING_PLATFORM
    src/Platform/Window.cpp
    src/Platform/Input.cpp
    src/Platform/DynamicLibrary.cpp
)
target_link_libraries(EnginePlatform PUBLIC EngineFoundation glfw Vulkan::Vulkan glm::glm ${CMAKE_DL_LIBS})

engine_module(EngineCore ENGINE_BUILDING_CORE
    src/Core/ModuleManager.cpp
    src/Core/Camera.cpp
    src/Core/GameLoop.cpp
    src/Core/EngineConfig.cpp
)
target_link_libraries(EngineCore PUBLIC EngineFoundation EnginePlatform glm::glm EnTT::EnTT)

engine_module(EngineGeometry ENGINE_BUILDING_GEOMETRY
    src/Geometry/Bounds.cpp
    src/Geometry/Cube.cpp
    src/Geometry/Ray.cpp
)
target_link_libraries(EngineGeometry PUBLIC EngineFoundation glm::glm Vulkan::Vulkan)

engine_module(EngineGraphics ENGINE_BUILDING_GRAPHICS
    src/Graphics/EnvironmentMap.cpp
    src/Graphics/Frustum.cpp
    src/Graphics/RenderQueue.cpp
    src/Graphics/ShadowMap.cpp
    src/Graphics/PipelineState.cpp
    src/Graphics/VulkanContext.cpp
    src/Graphics/VulkanHelpers.cpp
    src/Graphics/Swapchain.cpp
    src/Graphics/GraphicsPipeline.cpp
    src/Graphics/Buffer.cpp
    src/Graphics/HudFont.cpp
    src/Graphics/HudRenderer.cpp
    src/Graphics/DynamicFont.cpp
    src/Graphics/EditorLayout.cpp
    src/Graphics/UIContext.cpp
    src/Graphics/UIInspector.cpp
    src/Graphics/UIRenderer.cpp
    src/Graphics/Renderer.cpp
    src/Graphics/TextEditor.cpp
    src/Graphics/DslCompletion.cpp
    src/Graphics/ConsoleLog.cpp
)
target_link_libraries(EngineGraphics
    PUBLIC EngineFoundation EnginePlatform EngineGeometry Vulkan::Vulkan glfw glm::glm)

engine_module(EngineAssets ENGINE_BUILDING_ASSETS
    src/Assets/AssetManager.cpp
)
target_link_libraries(EngineAssets PUBLIC EngineFoundation EngineGeometry EngineGraphics glm::glm)

engine_module(EngineScene ENGINE_BUILDING_SCENE
    src/Scene/Scene.cpp
    src/Scene/Reflection.cpp
    src/Scene/NativeScript.cpp
    src/Scene/UIOutliner.cpp
    src/Scene/TransformGizmo.cpp
    src/Scene/UIAssetBrowser.cpp
    src/Scene/SceneSerialization.cpp
    src/Scene/SceneBinarySerialization.cpp
    src/Scene/ProjectExport.cpp
    src/Scene/PlaySession.cpp
    src/Scene/Components.cpp
    src/Scene/Raycast.cpp
    src/Scene/DynamicAABBTree.cpp
    src/Scene/Systems/CameraSystem.cpp
    src/Scene/Systems/LightingSystem.cpp
    src/Scene/Systems/PhysicsSystem.cpp
    src/Physics/MovementSystem.cpp
    src/Scene/Systems/PlayerSystem.cpp
    src/Scene/Systems/RotationSystem.cpp
    src/Scene/Systems/RenderSystem.cpp
    src/Scene/Systems/TransformSystem.cpp
    src/Scene/Systems/AnimationSystem.cpp
    src/Scripting/DSLParser.cpp
    src/Scripting/ScriptSystem.cpp
    src/Scripting/StalkerAISystem.cpp
)
target_link_libraries(EngineScene
    PUBLIC EngineFoundation EngineCore EnginePlatform EngineGeometry EngineGraphics EngineAssets
           EnTT::EnTT glm::glm Threads::Threads)

# ---------------------------------------------------------------------------
# Networking (Source-style multiplayer): ENet transport, server/client
# runtime, snapshot delta compression, prediction. ENet is vendored as
# plain C sources (silenced: third-party code is not held to the
# engine's warning bar) so its build quirks stay out of the module.
# ---------------------------------------------------------------------------
FetchContent_GetProperties(enet)
if(NOT enet_POPULATED)
    FetchContent_Populate(enet)
endif()
set(ENGINE_ENET_SOURCES
    ${enet_SOURCE_DIR}/callbacks.c
    ${enet_SOURCE_DIR}/compress.c
    ${enet_SOURCE_DIR}/host.c
    ${enet_SOURCE_DIR}/list.c
    ${enet_SOURCE_DIR}/packet.c
    ${enet_SOURCE_DIR}/peer.c
    ${enet_SOURCE_DIR}/protocol.c
)
if(WIN32)
    list(APPEND ENGINE_ENET_SOURCES ${enet_SOURCE_DIR}/win32.c)
else()
    list(APPEND ENGINE_ENET_SOURCES ${enet_SOURCE_DIR}/unix.c)
endif()
if(MSVC)
    set_source_files_properties(${ENGINE_ENET_SOURCES} PROPERTIES COMPILE_OPTIONS "/w")
else()
    set_source_files_properties(${ENGINE_ENET_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
endif()

engine_module(EngineNet ENGINE_BUILDING_NET
    src/Net/NetProtocol.cpp
    src/Net/ServerConfig.cpp
    src/Net/NetTransport.cpp
    src/Net/NetServer.cpp
    src/Net/NetClient.cpp
    src/Net/DedicatedServer.cpp
    ${ENGINE_ENET_SOURCES}
)
target_include_directories(EngineNet PRIVATE ${enet_SOURCE_DIR}/include)
target_link_libraries(EngineNet PUBLIC EngineScene)
if(WIN32)
    target_link_libraries(EngineNet PRIVATE ws2_32 winmm)
endif()

engine_module(EngineApp ENGINE_BUILDING_APP
    src/Core/Application.cpp
)
target_link_libraries(EngineApp
    PUBLIC EngineFoundation EngineCore EnginePlatform EngineGeometry EngineGraphics EngineAssets EngineScene
           EngineNet
           Vulkan::Vulkan glfw glm::glm EnTT::EnTT Threads::Threads)

# ---------------------------------------------------------------------------
# Game modules (Phases 23-24): native shared libraries implementing
# IGameModule. Branding policy: every module ships as
# EngineRuntimeCore_<signature>, signature = first 10 hex chars of the
# MD5 of the target name. ModuleManager enforces the same convention
# plus the GetEngineModuleInfo handshake (magic token + ABI version).
# ---------------------------------------------------------------------------
function(engine_add_game_module target)
    add_library(${target} MODULE ${ARGN})
    string(MD5 module_signature "${target}")
    string(SUBSTRING "${module_signature}" 0 10 module_signature)
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME "EngineRuntimeCore_${module_signature}"
        PREFIX "")
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/Engine)
    target_compile_definitions(${target} PRIVATE ENGINE_BUILDING_PLUGIN)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# Newtonian 6-DOF space flight + procedural asteroid sandbox (Phase 24).
# Unlike the ABI-only test fixtures, a real game module links the
# engine module libraries whose APIs it drives (Scene/Assets/Input);
# in shared builds those resolve to the already-loaded engine DLLs.
engine_add_game_module(SpaceFlightModule modules/SpaceFlightModule.cpp)
target_include_directories(SpaceFlightModule PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/modules)
target_link_libraries(SpaceFlightModule
    PRIVATE EngineScene EngineAssets EngineCore EnginePlatform EngineFoundation
            Vulkan::Vulkan glm::glm EnTT::EnTT)

# ---------------------------------------------------------------------------
# Engine executable (thin host: main() + EngineApp shared library)
# ---------------------------------------------------------------------------
add_executable(3d-engine src/main.cpp)
target_link_libraries(3d-engine PRIVATE EngineApp)
engine_copy_runtime_dlls(3d-engine)
if(MSVC)
    target_compile_options(3d-engine PRIVATE /W4 /permissive-)
else()
    target_compile_options(3d-engine PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# Shader compilation: GLSL -> SPIR-V (requires glslc or glslangValidator
# from the Vulkan SDK)
# ---------------------------------------------------------------------------
find_program(GLSLC_EXECUTABLE NAMES glslc HINTS "$ENV{VULKAN_SDK}/bin")
find_program(GLSLANG_EXECUTABLE NAMES glslangValidator HINTS "$ENV{VULKAN_SDK}/bin")

if(NOT GLSLC_EXECUTABLE AND NOT GLSLANG_EXECUTABLE)
    message(FATAL_ERROR
        "No shader compiler found. Install the Vulkan SDK "
        "(https://vulkan.lunarg.com) so that glslc or glslangValidator is on "
        "your PATH or pointed to by the VULKAN_SDK environment variable.")
endif()

set(SHADER_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADER_OUTPUT_DIR ${CMAKE_BINARY_DIR}/shaders)

set(SHADER_SOURCES
    ${SHADER_SOURCE_DIR}/cube.vert
    ${SHADER_SOURCE_DIR}/cube.frag
    ${SHADER_SOURCE_DIR}/shadow.vert
    ${SHADER_SOURCE_DIR}/skybox.vert
    ${SHADER_SOURCE_DIR}/skybox.frag
    ${SHADER_SOURCE_DIR}/post.vert
    ${SHADER_SOURCE_DIR}/bloomDown.frag
    ${SHADER_SOURCE_DIR}/bloomUp.frag
    ${SHADER_SOURCE_DIR}/composite.frag
    ${SHADER_SOURCE_DIR}/hud.vert
    ${SHADER_SOURCE_DIR}/hud.frag
    ${SHADER_SOURCE_DIR}/ui.vert
    ${SHADER_SOURCE_DIR}/ui.frag
)

set(SHADER_OUTPUTS "")
foreach(SHADER_SOURCE IN LISTS SHADER_SOURCES)
    get_filename_component(SHADER_NAME ${SHADER_SOURCE} NAME)
    set(SHADER_OUTPUT ${SHADER_OUTPUT_DIR}/${SHADER_NAME}.spv)

    if(GLSLC_EXECUTABLE)
        add_custom_command(
            OUTPUT  ${SHADER_OUTPUT}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_OUTPUT_DIR}
            COMMAND ${GLSLC_EXECUTABLE} ${SHADER_SOURCE} -o ${SHADER_OUTPUT}
            DEPENDS ${SHADER_SOURCE}
            COMMENT "Compiling shader ${SHADER_NAME} -> SPIR-V (glslc)"
            VERBATIM
        )
    else()
        add_custom_command(
            OUTPUT  ${SHADER_OUTPUT}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_OUTPUT_DIR}
            COMMAND ${GLSLANG_EXECUTABLE} -V ${SHADER_SOURCE} -o ${SHADER_OUTPUT}
            DEPENDS ${SHADER_SOURCE}
            COMMENT "Compiling shader ${SHADER_NAME} -> SPIR-V (glslangValidator)"
            VERBATIM
        )
    endif()

    list(APPEND SHADER_OUTPUTS ${SHADER_OUTPUT})
endforeach()

add_custom_target(engine_shaders ALL DEPENDS ${SHADER_OUTPUTS})
add_dependencies(3d-engine engine_shaders)

# The renderer locates compiled SPIR-V through this absolute path, so the
# executable can be launched from any working directory.
string(REPLACE "\\" "/" SHADER_OUTPUT_DIR_SLASHES ${SHADER_OUTPUT_DIR})
string(REPLACE "\\" "/" ENGINE_FONT_DIR_SLASHES ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(EngineGraphics
    PRIVATE
        ENGINE_SHADER_DIR="${SHADER_OUTPUT_DIR_SLASHES}"
        # Bundled fallback UI font (used when no system sans-serif --
        # Segoe UI / Liberation / Noto / DejaVu -- can be found).
        ENGINE_DEFAULT_FONT_PATH="${ENGINE_FONT_DIR_SLASHES}/assets/fonts/DejaVuSans.ttf"
)

# ---------------------------------------------------------------------------
# Standalone game player (Phase 33): the ship build's entry point.
# Boots a GBQB bundle written by the editor's Export Game straight
# into the game — deliberately links no EngineApp (the editor shell
# never ships). It resolves bundle shaders through the same
# ENGINE_SHADER_DIR the pipeline reads, extracting any the install
# does not already provide.
# ---------------------------------------------------------------------------
add_executable(game-player src/Standalone/StandaloneMain.cpp)
target_link_libraries(game-player
    PRIVATE EngineScene EngineAssets EngineGraphics EngineGeometry EngineCore
            EnginePlatform EngineFoundation EngineNet Threads::Threads Vulkan::Vulkan glfw
            glm::glm EnTT::EnTT)
engine_copy_runtime_dlls(game-player)
add_dependencies(game-player engine_shaders)
target_compile_definitions(game-player
    PRIVATE
        ENGINE_SHADER_DIR="${SHADER_OUTPUT_DIR_SLASHES}"
)
if(MSVC)
    target_compile_options(game-player PRIVATE /W4 /permissive-)
else()
    target_compile_options(game-player PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# Dedicated server (vibe_server): the headless, server-authoritative
# runtime. No window, no Vulkan — just the fixed 128 Hz simulation,
# physics, server .dsla scripts and the ENet transport. Boots from a
# Source-style server.cfg (editor Server tab writes it) plus an
# optional GBQ scene. `game-player --server` runs the same code.
# ---------------------------------------------------------------------------
add_executable(vibe_server src/Server/ServerMain.cpp)
target_link_libraries(vibe_server
    PRIVATE EngineNet EngineScene EngineCore EnginePlatform EngineFoundation
            Threads::Threads glm::glm EnTT::EnTT)
engine_copy_runtime_dlls(vibe_server)
if(MSVC)
    target_compile_options(vibe_server PRIVATE /W4 /permissive-)
else()
    target_compile_options(vibe_server PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# Backrooms scripting demo: headless hybrid DSL + native showcase.
# Builds the Scenario 18 scene (player / almond water / safe room /
# stalker), runs the DSL ScriptSystem and the native
# StalkerAISystem in the same frame loop and prints a trace. No
# window, no Vulkan — safe for CI and for the sandbox.
# ---------------------------------------------------------------------------
add_executable(scripting-demo examples/ScriptingDemo.cpp)
target_link_libraries(scripting-demo
    PRIVATE EngineScene EngineCore EnginePlatform EngineFoundation
            Threads::Threads glm::glm EnTT::EnTT)
engine_copy_runtime_dlls(scripting-demo)
if(MSVC)
    target_compile_options(scripting-demo PRIVATE /W4 /permissive-)
else()
    target_compile_options(scripting-demo PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# Engine configuration (config/engine.ini)
#
# Copied next to the executable so edits take effect without rebuilding,
# and the source-tree location is baked in as a development fallback.
# ---------------------------------------------------------------------------
add_custom_command(TARGET 3d-engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/config/engine.ini
            $<TARGET_FILE_DIR:3d-engine>/config/engine.ini
    COMMENT "Copying engine configuration"
    VERBATIM
)

# Sample project assets (Phase 29): mirrored next to the executable
# so the editor's Asset Browser opens on real content.
add_custom_command(TARGET 3d-engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/assets
            $<TARGET_FILE_DIR:3d-engine>/assets
    COMMENT "Copying sample assets"
    VERBATIM
)

string(REPLACE "\\" "/" ENGINE_SOURCE_DIR_SLASHES ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(EngineCore
    PRIVATE
        ENGINE_DEFAULT_CONFIG_PATH="${ENGINE_SOURCE_DIR_SLASHES}/config/engine.ini"
)

# ---------------------------------------------------------------------------
# Tests (core infrastructure that needs no GPU/window)
# ---------------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
    add_executable(engine-core-tests tests/CoreTests.cpp)
    # The test host links every shared module directly: if a public symbol
    # were missing from a DLL's export surface, this target would fail to
    # link, so the suite doubles as the dynamic-module export test.
    target_link_libraries(engine-core-tests
        PRIVATE EngineApp EngineScene EngineAssets EngineGraphics EngineGeometry EngineCore
                EnginePlatform EngineFoundation EngineNet Threads::Threads Vulkan::Vulkan glfw glm::glm EnTT::EnTT)

    # Phase 23/24: mock game modules for the hot-reload and branding
    # tests. They are standalone shared libraries (ABI header only, no
    # engine link) shipping under engine-branded names, loaded by path
    # through ModuleManager exactly like real game code.
    foreach(plugin TestGameModule TestBadGameModule TestThrowGameModule
                   TestForgedGameModule TestLegacyGameModule)
        engine_add_game_module(${plugin} tests/${plugin}.cpp)
    endforeach()
    add_dependencies(engine-core-tests
        TestGameModule TestBadGameModule TestThrowGameModule
        TestForgedGameModule TestLegacyGameModule SpaceFlightModule)
    target_include_directories(engine-core-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/modules)
    target_compile_definitions(engine-core-tests PRIVATE
        ENGINE_TEST_PLUGIN_PATH="$<TARGET_FILE:TestGameModule>"
        ENGINE_TEST_BAD_PLUGIN_PATH="$<TARGET_FILE:TestBadGameModule>"
        ENGINE_TEST_THROW_PLUGIN_PATH="$<TARGET_FILE:TestThrowGameModule>"
        ENGINE_TEST_FORGED_PLUGIN_PATH="$<TARGET_FILE:TestForgedGameModule>"
        ENGINE_TEST_LEGACY_PLUGIN_PATH="$<TARGET_FILE:TestLegacyGameModule>"
        ENGINE_TEST_SPACEFLIGHT_PLUGIN_PATH="$<TARGET_FILE:SpaceFlightModule>")
    engine_copy_runtime_dlls(engine-core-tests)
    if(MSVC)
        target_compile_options(engine-core-tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(engine-core-tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME engine-core-tests COMMAND engine-core-tests)
endif()
