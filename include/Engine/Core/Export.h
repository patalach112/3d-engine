#pragma once

// ---------------------------------------------------------------------------
// Engine module export/import macros.
//
// The engine is built as a set of shared libraries (DLLs on Windows, shared
// objects elsewhere). Public classes/functions in each module are annotated
// with that module's *_API macro. The module being built defines its
// ENGINE_BUILDING_<MODULE> macro privately through CMake; consumers see the
// import form. On Windows CMake also enables WINDOWS_EXPORT_ALL_SYMBOLS for
// the module targets as a safety net for templates and auxiliary symbols,
// while these annotations make the intended public ABI explicit.
// ---------------------------------------------------------------------------

#if defined(ENGINE_STATIC)
#define ENGINE_DLL_EXPORT
#define ENGINE_DLL_IMPORT
#define ENGINE_DLL_LOCAL
#elif defined(_WIN32) || defined(__CYGWIN__)
#define ENGINE_DLL_EXPORT __declspec(dllexport)
#define ENGINE_DLL_IMPORT __declspec(dllimport)
#define ENGINE_DLL_LOCAL
#elif defined(__GNUC__) || defined(__clang__)
#define ENGINE_DLL_EXPORT __attribute__((visibility("default")))
#define ENGINE_DLL_IMPORT __attribute__((visibility("default")))
#define ENGINE_DLL_LOCAL __attribute__((visibility("hidden")))
#else
#define ENGINE_DLL_EXPORT
#define ENGINE_DLL_IMPORT
#define ENGINE_DLL_LOCAL
#endif

#ifdef ENGINE_BUILDING_FOUNDATION
#define ENGINE_FOUNDATION_API ENGINE_DLL_EXPORT
#else
#define ENGINE_FOUNDATION_API ENGINE_DLL_IMPORT
#endif

#ifdef ENGINE_BUILDING_PLATFORM
#define ENGINE_PLATFORM_API ENGINE_DLL_EXPORT
#else
#define ENGINE_PLATFORM_API ENGINE_DLL_IMPORT
#endif

#ifdef ENGINE_BUILDING_CORE
#define ENGINE_CORE_API ENGINE_DLL_EXPORT
#else
#define ENGINE_CORE_API ENGINE_DLL_IMPORT
#endif

#ifdef ENGINE_BUILDING_GEOMETRY
#define ENGINE_GEOMETRY_API ENGINE_DLL_EXPORT
#else
#define ENGINE_GEOMETRY_API ENGINE_DLL_IMPORT
#endif

#ifdef ENGINE_BUILDING_GRAPHICS
#define ENGINE_GRAPHICS_API ENGINE_DLL_EXPORT
#else
#define ENGINE_GRAPHICS_API ENGINE_DLL_IMPORT
#endif

#ifdef ENGINE_BUILDING_ASSETS
#define ENGINE_ASSETS_API ENGINE_DLL_EXPORT
#else
#define ENGINE_ASSETS_API ENGINE_DLL_IMPORT
#endif

#ifdef ENGINE_BUILDING_SCENE
#define ENGINE_SCENE_API ENGINE_DLL_EXPORT
#else
#define ENGINE_SCENE_API ENGINE_DLL_IMPORT
#endif

#ifdef ENGINE_BUILDING_APP
#define ENGINE_APP_API ENGINE_DLL_EXPORT
#else
#define ENGINE_APP_API ENGINE_DLL_IMPORT
#endif

#ifdef ENGINE_BUILDING_NET
#define ENGINE_NET_API ENGINE_DLL_EXPORT
#else
#define ENGINE_NET_API ENGINE_DLL_IMPORT
#endif
