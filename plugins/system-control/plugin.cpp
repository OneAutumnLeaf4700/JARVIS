#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "jarvis_plugin_abi.h"
#include "plugin_internal.h"

extern char** environ;

namespace {

// Runs argv[0] with the given arguments via posix_spawnp (no shell — argv is passed straight
// through, so nothing in `argv` can be interpreted as shell syntax regardless of its content).
// Returns true and the command's own stdout+stderr are inherited straight through to JARVIS's
// own — this plugin doesn't capture output, only exit status, since "volume set" and "shutdown"
// only need to report success/failure, not relay text back.
bool runCommand(const std::vector<std::string>& argv) {
    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const std::string& arg : argv) {
        cargv.push_back(const_cast<char*>(arg.c_str()));
    }
    cargv.push_back(nullptr);

    pid_t pid = 0;
    const int spawnResult = posix_spawnp(&pid, cargv[0], nullptr, nullptr, cargv.data(), environ);
    if (spawnResult != 0) {
        return false;
    }

    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
        return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

char* makeResult(const std::string& text) {
    char* result = static_cast<char*>(std::malloc(text.size() + 1));
    std::memcpy(result, text.c_str(), text.size() + 1);
    return result;
}

char* volumeExecute(const char* payload) {
    const std::string text = payload != nullptr ? payload : "";
    const std::optional<int> level = parseVolumeArgument(text);

    if (!level.has_value()) {
        std::istringstream stream(text);
        std::string verb;
        stream >> verb;
        if (verb == "get") {
            if (runCommand({"pactl", "get-sink-volume", "@DEFAULT_SINK@"})) {
                return makeResult("Current volume printed via pactl above.");
            }
            return makeResult("Could not read volume — is 'pactl' installed and a sink present?");
        }
        return makeResult("Usage: volume get | volume set <0-100>");
    }

    if (runCommand(buildVolumeArgv(*level))) {
        return makeResult("Volume set to " + std::to_string(*level) + "%.");
    }
    return makeResult("Failed to set volume — is 'pactl' installed and a sink present?");
}

}  // namespace

extern "C" int jarvis_plugin_abi_version() {
    return JARVIS_PLUGIN_ABI_VERSION;
}

extern "C" int jarvis_plugin_register(void* host_context, const JarvisPluginHost* host) {
    return host->registerCapability(
        host_context, "volume",
        "Gets or sets system output volume (0-100). Usage: volume get | volume set <0-100>",
        JARVIS_POWER_TIER_T2_SYSTEM_AFFECTING, &volumeExecute);
}
