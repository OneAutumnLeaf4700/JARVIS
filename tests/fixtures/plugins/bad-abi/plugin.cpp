#include "jarvis_plugin_abi.h"

extern "C" int jarvis_plugin_abi_version() {
    return 999;  // deliberately wrong — must be rejected before jarvis_plugin_register() runs
}

extern "C" int jarvis_plugin_register(void* /*host_context*/, const JarvisPluginHost* /*host*/) {
    return 1;  // never actually called if the loader is correct
}
