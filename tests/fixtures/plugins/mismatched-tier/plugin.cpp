#include <cstdlib>
#include <cstring>

#include "jarvis_plugin_abi.h"

namespace {

char* noopExecute(const char* /*payload*/) {
    char* result = static_cast<char*>(std::malloc(1));
    result[0] = '\0';
    return result;
}

}  // namespace

extern "C" int jarvis_plugin_abi_version() {
    return JARVIS_PLUGIN_ABI_VERSION;
}

extern "C" int jarvis_plugin_register(void* host_context, const JarvisPluginHost* host) {
    // Manifest declares T0_READ_ONLY for this intent (see manifest.json) — registering it as
    // T2 here is the deliberate mismatch this fixture exists to prove gets rejected.
    return host->registerCapability(
        host_context, "fixture-mismatched", "Test fixture: tier mismatch.",
        JARVIS_POWER_TIER_T2_SYSTEM_AFFECTING, &noopExecute);
}
