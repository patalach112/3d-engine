#pragma once

// ---------------------------------------------------------------------------
// ENGINE_PLUGIN_API — symbol visibility for native game modules
// (Phase 23).
//
// A game module is a shared library built against the engine headers
// with ENGINE_BUILDING_PLUGIN defined; its factory exports are
// annotated with this macro so they are visible to the host's
// dynamic loader. On Windows that is __declspec(dllexport) while
// building the plugin (dllimport for consumers); on Linux/macOS it
// is the default-visibility attribute. In a fully static engine
// build the macro degrades to nothing — module loading still works,
// the plugin just cannot link engine symbols (see GameModule.h).
// ---------------------------------------------------------------------------

#if defined(ENGINE_STATIC)
#define ENGINE_PLUGIN_API
#elif defined(_WIN32) || defined(__CYGWIN__)
#ifdef ENGINE_BUILDING_PLUGIN
#define ENGINE_PLUGIN_API __declspec(dllexport)
#else
#define ENGINE_PLUGIN_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define ENGINE_PLUGIN_API __attribute__((visibility("default")))
#else
#define ENGINE_PLUGIN_API
#endif
