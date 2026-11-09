cmake_minimum_required(VERSION 3.24)

project(3DEngine
    VERSION 0.1.0
    DESCRIPTION "Modern general-purpose 3D game engine (Vulkan)"
    LANGUAGES CXX
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
# Engine executable (Phase 1: first render)
# ---------------------------------------------------------------------------
add_executable(3d-engine
    src/main.cpp
    src/Core/Application.cpp
    src/Core/Camera.cpp
    src/Core/Jobs/JobSystem.cpp
    src/Core/Memory/PoolAllocator.cpp
    src/Assets/AssetManager.cpp
    src/Graphics/PipelineState.cpp
    src/Scene/Scene.cpp
    src/Scene/Components.cpp
    src/Scene/Systems/CameraSystem.cpp
    src/Scene/Systems/PhysicsSystem.cpp
    src/Scene/Systems/RotationSystem.cpp
    src/Scene/Systems/RenderSystem.cpp
    src/Core/EngineConfig.cpp
    src/Core/Logger.cpp
    src/Core/Time.cpp
    src/Core/GameLoop.cpp
    src/Platform/Input.cpp
    src/Platform/Window.cpp
    src/Geometry/Bounds.cpp
    src/Geometry/Cube.cpp
    src/Graphics/Frustum.cpp
    src/Graphics/HudFont.cpp
    src/Graphics/HudRenderer.cpp
    src/Graphics/RenderQueue.cpp
    src/Graphics/VulkanContext.cpp
    src/Graphics/VulkanHelpers.cpp
    src/Graphics/Swapchain.cpp
    src/Graphics/GraphicsPipeline.cpp
    src/Graphics/Buffer.cpp
    src/Graphics/Renderer.cpp
)

target_include_directories(3d-engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(3d-engine
    PRIVATE
        Vulkan::Vulkan
        glfw
        glm::glm
        EnTT::EnTT
        Threads::Threads
)

# GLM must match Vulkan conventions: radians everywhere and a [0, 1] depth
# range (instead of OpenGL's [-1, 1]).
target_compile_definitions(3d-engine
    PRIVATE
        GLM_FORCE_RADIANS
        GLM_FORCE_DEPTH_ZERO_TO_ONE
)

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
    ${SHADER_SOURCE_DIR}/hud.vert
    ${SHADER_SOURCE_DIR}/hud.frag
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
target_compile_definitions(3d-engine
    PRIVATE
        ENGINE_SHADER_DIR="${SHADER_OUTPUT_DIR_SLASHES}"
)

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

string(REPLACE "\\" "/" ENGINE_SOURCE_DIR_SLASHES ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(3d-engine
    PRIVATE
        ENGINE_DEFAULT_CONFIG_PATH="${ENGINE_SOURCE_DIR_SLASHES}/config/engine.ini"
)

# ---------------------------------------------------------------------------
# Tests (core infrastructure that needs no GPU/window)
# ---------------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
    add_executable(engine-core-tests
        tests/CoreTests.cpp
        src/Core/Camera.cpp
        src/Core/EngineConfig.cpp
        src/Core/Jobs/JobSystem.cpp
        src/Core/Memory/PoolAllocator.cpp
        src/Core/Logger.cpp
        src/Core/Time.cpp
        src/Core/GameLoop.cpp
        src/Platform/Input.cpp
        src/Platform/Window.cpp
        src/Geometry/Bounds.cpp
        src/Geometry/Cube.cpp
        src/Graphics/Frustum.cpp
    src/Graphics/HudFont.cpp
    src/Graphics/HudRenderer.cpp
    src/Graphics/RenderQueue.cpp
        src/Assets/AssetManager.cpp
    src/Graphics/PipelineState.cpp
    src/Scene/Scene.cpp
        src/Scene/Components.cpp
        src/Scene/Systems/CameraSystem.cpp
        src/Scene/Systems/PhysicsSystem.cpp
        src/Scene/Systems/RotationSystem.cpp
        src/Scene/Systems/RenderSystem.cpp
        src/Graphics/VulkanContext.cpp
        src/Graphics/VulkanHelpers.cpp
        src/Graphics/Swapchain.cpp
        src/Graphics/GraphicsPipeline.cpp
        src/Graphics/Buffer.cpp
        src/Graphics/Renderer.cpp
    )
    target_include_directories(engine-core-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(engine-core-tests PRIVATE GLM_FORCE_RADIANS GLM_FORCE_DEPTH_ZERO_TO_ONE)
    target_link_libraries(engine-core-tests
        PRIVATE Threads::Threads Vulkan::Vulkan glfw glm::glm EnTT::EnTT)
    if(MSVC)
        target_compile_options(engine-core-tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(engine-core-tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME engine-core-tests COMMAND engine-core-tests)
endif()
