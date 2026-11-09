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

FetchContent_MakeAvailable(glfw glm)

# ---------------------------------------------------------------------------
# Engine executable (Phase 1: first render)
# ---------------------------------------------------------------------------
add_executable(3d-engine
    src/main.cpp
    src/Core/Application.cpp
    src/Platform/Window.cpp
    src/Geometry/Cube.cpp
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
