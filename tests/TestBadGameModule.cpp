// A shared library that loads fine but exports no game-module
// factories: ModuleManager must reject it gracefully (Phase 23).
#include "Core/PluginApi.h"

extern "C" ENGINE_PLUGIN_API int BadGameModuleMarker() { return 23; }
