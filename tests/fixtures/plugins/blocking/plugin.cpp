#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#include "jarvis_plugin_abi.h"

namespace {

bool fileExists(const std::string& path) {
    std::ifstream file(path);
    return file.good();
}

// payload is "readyFile|releaseFile". Touches readyFile the moment execution starts (so a test
// waiting on it observes the call is genuinely in flight — invocationCount is already
// incremented by the host trampoline before this function is even called), then blocks polling
// for releaseFile so the test can prove PluginLoader::unloadPlugin refuses while this call is
// still running.
char* blockUntilReleased(const char* payload) {
    const std::string combined = payload ? payload : "";
    const std::size_t sep = combined.find('|');
    const std::string readyFile = combined.substr(0, sep);
    const std::string releaseFile = sep == std::string::npos ? "" : combined.substr(sep + 1);

    { std::ofstream ready(readyFile); }

    while (!fileExists(releaseFile)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    const char kResult[] = "unblocked";
    char* result = static_cast<char*>(std::malloc(sizeof(kResult)));
    std::memcpy(result, kResult, sizeof(kResult));
    return result;
}

}  // namespace

extern "C" int jarvis_plugin_abi_version() {
    return JARVIS_PLUGIN_ABI_VERSION;
}

extern "C" int jarvis_plugin_register(void* host_context, const JarvisPluginHost* host) {
    return host->registerCapability(
        host_context, "fixture-blocking",
        "Test fixture: blocks until a release-file signal appears.",
        JARVIS_POWER_TIER_T0_READ_ONLY, &blockUntilReleased);
}
