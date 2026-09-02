#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "jarvis_plugin_abi.h"
#include "plugin_internal.h"

extern char** environ;

namespace {

// INV-7: every cross-boundary call needs a deadline and an honest fallback. Spawning pactl /
// systemctl is a boundary call just like the AI gRPC call elsewhere in this codebase (which uses
// a 5s deadline) — a wedged child process (e.g. pactl blocking on a dead PulseAudio/PipeWire
// socket) must not hang the CLI loop or a gRPC handler thread forever. 5 seconds mirrors that
// existing reference deadline; there's nothing pactl/systemctl-specific requiring a different
// value.
constexpr std::chrono::milliseconds kChildDeadline{5000};
constexpr std::chrono::milliseconds kPollInterval{20};

// Waits for `pid` to exit without blocking past `deadline`. On timeout, kills the child (so it
// can't keep running unbounded) and reaps it to avoid leaving a zombie, then reports failure —
// callers treat this exactly like any other "command failed" outcome, never a crash/hang.
bool waitForChildWithDeadline(pid_t pid, std::chrono::steady_clock::time_point deadline,
                               int* statusOut) {
    while (true) {
        int status = 0;
        const pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            *statusOut = status;
            return true;
        }
        if (result == -1) {
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, statusOut, 0);
            return false;
        }
        std::this_thread::sleep_for(kPollInterval);
    }
}

// Runs argv[0] with the given arguments via posix_spawnp (no shell — argv is passed straight
// through, so nothing in `argv` can be interpreted as shell syntax regardless of its content).
// Returns true and the command's own stdout+stderr are inherited straight through to JARVIS's
// own — this plugin doesn't capture output, only exit status, since "volume set" and "shutdown"
// only need to report success/failure, not relay text back. Bounded by kChildDeadline (INV-7).
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

    const auto deadline = std::chrono::steady_clock::now() + kChildDeadline;
    int status = 0;
    if (!waitForChildWithDeadline(pid, deadline, &status)) {
        return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// Same execution model as runCommand, but redirects the child's stdout into a pipe and returns
// what it wrote — needed for "volume get", whose whole point is to relay pactl's own output back
// through execute()'s return value rather than leave it stuck on this process's inherited stdout
// (INV-1: business logic returns data, it doesn't do surface-specific I/O). Bounded end-to-end
// (both the read loop and the final wait) by a single shared deadline (INV-7).
std::optional<std::string> runCommandCapturingOutput(const std::vector<std::string>& argv) {
    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const std::string& arg : argv) {
        cargv.push_back(const_cast<char*>(arg.c_str()));
    }
    cargv.push_back(nullptr);

    int pipeFds[2];
    // O_CLOEXEC: in a multithreaded gRPC server, a concurrent spawn from another in-flight
    // capability call must not inherit this pipe's write end across its own exec — that would
    // delay this call's EOF until the unrelated child also exits.
    if (pipe2(pipeFds, O_CLOEXEC) != 0) {
        return std::nullopt;
    }

    posix_spawn_file_actions_t fileActions;
    posix_spawn_file_actions_init(&fileActions);
    posix_spawn_file_actions_adddup2(&fileActions, pipeFds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&fileActions, pipeFds[0]);
    posix_spawn_file_actions_addclose(&fileActions, pipeFds[1]);

    pid_t pid = 0;
    const int spawnResult =
        posix_spawnp(&pid, cargv[0], &fileActions, nullptr, cargv.data(), environ);
    posix_spawn_file_actions_destroy(&fileActions);
    close(pipeFds[1]);

    if (spawnResult != 0) {
        close(pipeFds[0]);
        return std::nullopt;
    }

    const auto deadline = std::chrono::steady_clock::now() + kChildDeadline;
    std::string output;
    char buffer[256];
    bool timedOut = false;

    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            timedOut = true;
            break;
        }
        const int remainingMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());

        struct pollfd pfd;
        pfd.fd = pipeFds[0];
        pfd.events = POLLIN;
        pfd.revents = 0;
        const int pollResult = poll(&pfd, 1, remainingMs);

        if (pollResult == -1) {
            if (errno == EINTR) {
                continue;
            }
            timedOut = true;
            break;
        }
        if (pollResult == 0) {
            timedOut = true;
            break;
        }

        if (pfd.revents & POLLIN) {
            ssize_t bytesRead = 0;
            do {
                bytesRead = read(pipeFds[0], buffer, sizeof(buffer));
            } while (bytesRead == -1 && errno == EINTR);

            if (bytesRead > 0) {
                output.append(buffer, static_cast<size_t>(bytesRead));
                continue;
            }
            if (bytesRead == 0) {
                break;  // EOF — child closed its stdout.
            }
            // Real read error (not EINTR) — nothing more usable to read.
            break;
        }

        // POLLHUP/POLLERR with no POLLIN pending: child closed the pipe with nothing left to read.
        break;
    }
    close(pipeFds[0]);

    if (timedOut) {
        kill(pid, SIGKILL);
        int status = 0;
        waitpid(pid, &status, 0);
        return std::nullopt;
    }

    int status = 0;
    if (!waitForChildWithDeadline(pid, deadline, &status)) {
        return std::nullopt;
    }
    if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
        return std::nullopt;
    }

    while (!output.empty() && (output.back() == '\n' || output.back() == '\r' ||
                                output.back() == ' ' || output.back() == '\t')) {
        output.pop_back();
    }
    return output;
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
            std::optional<std::string> output =
                runCommandCapturingOutput({"pactl", "get-sink-volume", "@DEFAULT_SINK@"});
            if (output.has_value() && !output->empty()) {
                return makeResult(*output);
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

char* shutdownExecute(const char* /*payload*/) {
    // Reaching this function at all means ConsentGate already verified the caller's payload
    // contained the standalone "confirm" token (Task 1) — the plugin does not re-check payload
    // content itself (INV-9: the capability never invents its own guardrail, it trusts the
    // gate that ran before it).
    if (runCommand(buildShutdownArgv())) {
        return makeResult("Shutting down.");
    }
    return makeResult("Failed to shut down — is 'systemctl' available and permitted for this user?");
}

}  // namespace

extern "C" int jarvis_plugin_abi_version() {
    return JARVIS_PLUGIN_ABI_VERSION;
}

extern "C" int jarvis_plugin_register(void* host_context, const JarvisPluginHost* host) {
    const int volumeOk = host->registerCapability(
        host_context, "volume",
        "Gets or sets system output volume (0-100). Usage: volume get | volume set <0-100>",
        JARVIS_POWER_TIER_T2_SYSTEM_AFFECTING, &volumeExecute);
    const int shutdownOk = host->registerCapability(
        host_context, "shutdown",
        "Powers off the machine. Requires the word 'confirm' in the command every time "
        "(T3 — never covered by a grant). Usage: shutdown confirm",
        JARVIS_POWER_TIER_T3_DESTRUCTIVE, &shutdownExecute);
    return volumeOk && shutdownOk;
}
