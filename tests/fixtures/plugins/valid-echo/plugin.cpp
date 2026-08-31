#include <cstdlib>
#include <cstring>

#include "jarvis_plugin_abi.h"

namespace {

char* echoExecute(const char* payload) {
    const std::size_t length = std::strlen(payload);
    char* result = static_cast<char*>(std::malloc(length + 1));
    std::memcpy(result, payload, length + 1);
    return result;
}

}  // namespace

extern "C" int jarvis_plugin_abi_version() {
    return JARVIS_PLUGIN_ABI_VERSION;
}

extern "C" int jarvis_plugin_register(void* host_context, const JarvisPluginHost* host) {
    return host->registerCapability(
        host_context, "fixture-echo", "Test fixture: echoes payload back.",
        JARVIS_POWER_TIER_T0_READ_ONLY, &echoExecute);
}
