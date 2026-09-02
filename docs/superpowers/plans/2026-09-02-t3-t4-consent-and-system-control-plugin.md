# T3/T4 Consent Enforcement + System Control Plugin Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `ConsentGate`'s unconditional T3/T4 denial with real per-call explicit-confirmation
enforcement, then ship `system-control` — a second dynamic plugin (volume: T2, shutdown: T3) that
proves both the existing T2 grant gate and the new T3 confirmation gate on a real capability.

**Architecture:** T3/T4 consent is a **per-call payload convention**, never a persisted grant —
this is what INV-9 means by "explicit confirmation regardless of prior permissions." `ConsentGate`
gains a `payload` parameter and checks for a standalone `confirm` token in it; no new proto field,
no session state, no surface-specific code (CLI/gRPC/voice all just pass payload text through
unchanged, keeping INV-3 intact). The `system-control` plugin follows the exact `system-info`
pattern (manifest.json + plugin.cpp against the pure-C ABI) with `volume` (T2, gated by the
existing grant flow) and `shutdown` (T3, gated by the new confirm-token flow) as its two
capabilities.

**Tech Stack:** C++17, GoogleTest, the existing `jarvis_plugin_abi.h` ABI, POSIX `execvp`/`fork`
(no shell string ever built — avoids command injection), Linux `pactl` (volume) and `systemctl`
(shutdown).

**Spec:** No separate spec doc — this plan is scoped directly against
`CLAUDE.md` (INV-9, INV-10, INV-1, INV-3) and `docs/roadmap.md` / `docs/features.md`'s Phase 4
"System control plugin" line item.

## Global Constraints

- INV-9: T3/T4 actions require explicit confirmation **on every call**, never inherited from a
  prior grant. `ConsentGate` — not the plugin — enforces this.
- INV-1: capabilities return data, never do I/O to a specific surface. The plugin's `execute()`
  returns a result string; it does not print or prompt.
- INV-10: no library punched through the spine. The plugin talks to the OS via `execvp` with an
  explicit argv array — never `system()`/`popen()` (shell-injection risk), and never a process
  library, since this is exactly leaf-capability territory the ABI already scopes for.
- INV-3: no surface (`core/main.cpp`, gRPC, voice) gets new plugin-specific logic. They keep
  passing payload text through unchanged.
- INV-2: proto is untouched by this plan — the confirm-token convention lives entirely in payload
  text, not a new field.
- Every plugin `.so` is built and staged into `${CMAKE_BINARY_DIR}/plugins/<id>/` exactly like
  `system-info`, and added to `jarvis`/`jarvis_grpc_server`/`jarvis_tests`'s `add_dependencies`.
- Scope: this plan covers `volume` and `shutdown` only. "Open apps" and "lock screen" (also listed
  under the same Phase 4 roadmap bullet) are a separate subsystem — arbitrary-process-launch needs
  its own allowlist design and its own plan; do not fold it in here. Task 5 updates
  `docs/features.md`/`docs/roadmap.md` to reflect this split explicitly, so the deferred scope
  isn't silently dropped.

---

### Task 1: `ConsentGate` — real T3/T4 confirmation enforcement

**Files:**
- Modify: `core/consent_gate.h`
- Modify: `core/consent_gate.cpp`
- Modify: `tests/consent_gate_test.cpp`

**Interfaces:**
- Produces: `ConsentResult ConsentGate::check(const Capability& capability, const std::string& payload) const` — signature change from the current `check(const Capability&)`. Every caller in the codebase (Task 2) must be updated in the same commit or the build breaks.

- [ ] **Step 1: Write the failing tests**

Replace the full contents of `tests/consent_gate_test.cpp` with:

```cpp
#include <cstdio>
#include <gtest/gtest.h>

#include "capability.h"
#include "consent_gate.h"
#include "plugin_config.h"

namespace {

Capability makeCapability(const std::string& name, PowerTier tier) {
    return Capability{
        name, CommandType::ECHO, "test capability", tier,
        [](const std::string& payload, ExecutionContext&) { return payload; }
    };
}

}  // namespace

TEST(ConsentGateTest, T0AlwaysAllowedRegardlessOfConfig) {
    PluginConfig config = PluginConfig::load("does_not_exist.cfg", "does_not_exist.cfg");
    ConsentGate gate(config);

    ConsentResult result = gate.check(makeCapability("echo", PowerTier::T0_READ_ONLY), "");

    EXPECT_TRUE(result.allowed);
    EXPECT_TRUE(result.reason.empty());
}

TEST(ConsentGateTest, T1AlwaysAllowedRegardlessOfConfig) {
    PluginConfig config = PluginConfig::load("does_not_exist.cfg", "does_not_exist.cfg");
    ConsentGate gate(config);

    ConsentResult result =
        gate.check(makeCapability("some_stateful_thing", PowerTier::T1_STATEFUL_LOCAL), "");

    EXPECT_TRUE(result.allowed);
}

TEST(ConsentGateTest, T2DeniedWithoutGrant) {
    PluginConfig config = PluginConfig::load("does_not_exist.cfg", "does_not_exist.cfg");
    ConsentGate gate(config);

    ConsentResult result =
        gate.check(makeCapability("volume_control", PowerTier::T2_SYSTEM_AFFECTING), "");

    EXPECT_FALSE(result.allowed);
    EXPECT_NE(result.reason.find("volume_control"), std::string::npos);
    EXPECT_NE(result.reason.find("--grant"), std::string::npos);
}

// Shared fixture for tests that write grant files. Each test instance gets a filename unique
// to the actual running test (via GTest's current_test_info(), not __FUNCTION__), cleaned up
// unconditionally in TearDown() so a failed assertion can't leak the file into later runs.
class ConsentGateFileTest : public ::testing::Test {
 protected:
    std::string grants_file_;

    void SetUp() override {
        const auto* test_info = ::testing::UnitTest::GetInstance()->current_test_info();
        grants_file_ = "temp_grants_" + std::string(test_info->test_suite_name()) + "_" +
                        std::string(test_info->name()) + ".cfg";
    }

    void TearDown() override {
        std::remove(grants_file_.c_str());
    }
};

TEST_F(ConsentGateFileTest, T2AllowedWithGrant) {
    PluginConfig config = PluginConfig::load("does_not_exist.cfg", grants_file_);
    config.grant("volume_control");
    ConsentGate gate(config);

    ConsentResult result =
        gate.check(makeCapability("volume_control", PowerTier::T2_SYSTEM_AFFECTING), "");

    EXPECT_TRUE(result.allowed);
    EXPECT_TRUE(result.reason.empty());
}

TEST_F(ConsentGateFileTest, T3DeniedWithoutConfirmTokenEvenIfSomehowGranted) {
    PluginConfig config = PluginConfig::load("does_not_exist.cfg", grants_file_);
    config.grant("delete_files");  // granting is meaningless for T3 — gate must ignore it
    ConsentGate gate(config);

    ConsentResult result = gate.check(makeCapability("delete_files", PowerTier::T3_DESTRUCTIVE), "");

    EXPECT_FALSE(result.allowed);
    EXPECT_NE(result.reason.find("confirm"), std::string::npos);
}

TEST_F(ConsentGateFileTest, T3AllowedWithConfirmTokenEvenWithoutGrant) {
    PluginConfig config = PluginConfig::load("does_not_exist.cfg", grants_file_);
    ConsentGate gate(config);

    ConsentResult result =
        gate.check(makeCapability("delete_files", PowerTier::T3_DESTRUCTIVE), "confirm");

    EXPECT_TRUE(result.allowed);
    EXPECT_TRUE(result.reason.empty());
}

TEST_F(ConsentGateFileTest, T3ConfirmTokenMustBeAWholeWord) {
    PluginConfig config = PluginConfig::load("does_not_exist.cfg", grants_file_);
    ConsentGate gate(config);

    // "reconfirmation" contains "confirm" as a substring but is not the standalone token.
    ConsentResult result =
        gate.check(makeCapability("delete_files", PowerTier::T3_DESTRUCTIVE), "reconfirmation");

    EXPECT_FALSE(result.allowed);
}

TEST_F(ConsentGateFileTest, T4DeniedWithoutConfirmToken) {
    PluginConfig config = PluginConfig::load("does_not_exist.cfg", grants_file_);
    config.grant("call_external_api");
    ConsentGate gate(config);

    ConsentResult result = gate.check(makeCapability("call_external_api", PowerTier::T4_EXTERNAL), "");

    EXPECT_FALSE(result.allowed);
    EXPECT_NE(result.reason.find("confirm"), std::string::npos);
}

TEST_F(ConsentGateFileTest, T4AllowedWithConfirmToken) {
    PluginConfig config = PluginConfig::load("does_not_exist.cfg", grants_file_);
    ConsentGate gate(config);

    ConsentResult result =
        gate.check(makeCapability("call_external_api", PowerTier::T4_EXTERNAL), "please confirm");

    EXPECT_TRUE(result.allowed);
}
```

- [ ] **Step 2: Run tests to verify they fail to compile**

Run: `cmake -S . -B build && cmake --build build --target jarvis_tests`
Expected: FAIL — `gate.check(...)` now passes 2 args against the still-1-arg `ConsentGate::check`.

- [ ] **Step 3: Update `core/consent_gate.h`**

```cpp
#pragma once

#include <string>

#include "capability.h"
#include "plugin_config.h"

// Outcome of a consent check: allowed or not, with a human-readable reason when denied (empty
// when allowed).
struct ConsentResult {
    bool allowed;
    std::string reason;
};

// The single place a capability's declared PowerTier is turned into an allow/deny decision.
// Capabilities never check their own consent (INV-9) — this is that check, called from
// CapabilityRegistry::dispatch() only. `payload` is the capability's raw invocation payload:
// T3/T4 confirmation reads it for a standalone "confirm" token (INV-9 — explicit, per-call,
// never inherited from a persisted grant); T0-T2 ignore it.
class ConsentGate {
 public:
    explicit ConsentGate(const PluginConfig& config);
    ConsentResult check(const Capability& capability, const std::string& payload) const;

 private:
    const PluginConfig& config_;
};
```

- [ ] **Step 4: Update `core/consent_gate.cpp`**

```cpp
#include "consent_gate.h"

#include <sstream>

namespace {

// T3/T4 confirmation is a per-call payload convention, never persisted (INV-9: "explicit
// confirmation regardless of prior permissions"). The word "confirm" must appear as its own
// whitespace-delimited token anywhere in the payload — a substring match would let something
// like "reconfirmation" slip through by accident.
bool payloadConfirms(const std::string& payload) {
    std::istringstream stream(payload);
    std::string token;
    while (stream >> token) {
        if (token == "confirm") {
            return true;
        }
    }
    return false;
}

}  // namespace

ConsentGate::ConsentGate(const PluginConfig& config) : config_(config) {}

ConsentResult ConsentGate::check(const Capability& capability, const std::string& payload) const {
    switch (capability.powerTier) {
        case PowerTier::T0_READ_ONLY:
        case PowerTier::T1_STATEFUL_LOCAL:
            return {true, ""};

        case PowerTier::T2_SYSTEM_AFFECTING:
            if (config_.isGranted(capability.name)) {
                return {true, ""};
            }
            return {false, "Capability '" + capability.name +
                                "' requires consent. Grant it with: jarvis --grant " + capability.name};

        case PowerTier::T3_DESTRUCTIVE:
        case PowerTier::T4_EXTERNAL:
            if (payloadConfirms(payload)) {
                return {true, ""};
            }
            return {false, "'" + capability.name +
                                "' is power tier T3/T4 (destructive/external) and requires "
                                "explicit confirmation on every call — grants never cover it. "
                                "Re-run with 'confirm' added to the command, e.g. '" +
                                capability.name + " confirm'."};
    }
    return {false, "Unknown power tier."};
}
```

- [ ] **Step 5: Run tests to verify they compile and pass**

Run: `cmake --build build --target jarvis_tests && ./build/jarvis_tests --gtest_filter='ConsentGate*'`
Expected: PASS, all `ConsentGateTest.*` and `ConsentGateFileTest.*` cases green.

- [ ] **Step 6: Commit**

```bash
git add core/consent_gate.h core/consent_gate.cpp tests/consent_gate_test.cpp
git commit -m "feat(core): enforce T3/T4 consent via per-call confirm token (INV-9)"
```

---

### Task 2: Wire the new `check()` signature through `CapabilityRegistry` and `main.cpp`

**Files:**
- Modify: `core/capability_registry.cpp:60-64` and `core/capability_registry.cpp:82-86` (both `dispatch()` overloads)
- Modify: `core/main.cpp:33-38` (the `runGrantFlow` T3/T4 branch)

**Interfaces:**
- Consumes: `ConsentGate::check(const Capability&, const std::string&)` from Task 1.

- [ ] **Step 1: Update both `dispatch()` overloads in `core/capability_registry.cpp`**

In `dispatch(CommandType intent, ...)`, change:

```cpp
        ConsentGate gate(*pluginConfig_);
        ConsentResult consent = gate.check(*capability);
```

to:

```cpp
        ConsentGate gate(*pluginConfig_);
        ConsentResult consent = gate.check(*capability, payload);
```

Apply the identical change to the second `dispatch(const std::string& intentName, ...)` overload
just below it.

- [ ] **Step 2: Run the full test suite to confirm the fix compiles and existing dispatch tests still pass**

Run: `cmake --build build --target jarvis_tests && ./build/jarvis_tests`
Expected: PASS — all suites, including `CapabilityRegistryPluginGateTest.*` and
`CapabilityRegistryGrantTest.*` (their capabilities are T0/T2, so payload content is irrelevant to
them and they were already passing arbitrary payloads like `"up"`/`"50"`).

- [ ] **Step 3: Update the T3/T4 branch in `core/main.cpp`'s `runGrantFlow`**

Replace:

```cpp
    if (capability->powerTier == PowerTier::T3_DESTRUCTIVE ||
        capability->powerTier == PowerTier::T4_EXTERNAL) {
        std::cout << "'" << capabilityName << "' is power tier T3/T4 — enforcement isn't "
                     "implemented yet, so it cannot be granted.\n";
        return 1;
    }
```

with:

```cpp
    if (capability->powerTier == PowerTier::T3_DESTRUCTIVE ||
        capability->powerTier == PowerTier::T4_EXTERNAL) {
        std::cout << "'" << capabilityName << "' is power tier T3/T4 — it doesn't use a "
                     "persisted grant. Confirm it per call instead by adding 'confirm' to the "
                     "command, e.g. '" << capabilityName << " confirm'.\n";
        return 0;
    }
```

(`return 0`, not `1` — this is informational, matching the T0/T1 branch immediately below it, not
an error.)

- [ ] **Step 4: Manually verify the CLI message**

Run: `cmake --build build --target jarvis && ./build/jarvis --grant shutdown`
Expected output: `'shutdown' is power tier T3/T4 — it doesn't use a persisted grant. Confirm it
per call instead by adding 'confirm' to the command, e.g. 'shutdown confirm'.` (`shutdown` won't
exist as a capability until Task 4 — until then this is expected to print `Unknown capability:
shutdown` instead; re-run this exact check after Task 4 lands as its own verification step.)

- [ ] **Step 5: Commit**

```bash
git add core/capability_registry.cpp core/main.cpp
git commit -m "feat(core): thread payload into ConsentGate::check across dispatch and --grant"
```

---

### Task 3: `system-control` plugin scaffold — `volume` (T2)

**Files:**
- Create: `plugins/system-control/manifest.json`
- Create: `plugins/system-control/plugin_internal.h`
- Create: `plugins/system-control/plugin.cpp`
- Create: `tests/system_control_plugin_test.cpp`
- Modify: `CMakeLists.txt`
- Modify: `config/capabilities.cfg`

**Interfaces:**
- Produces: two pure, testable functions declared in `plugin_internal.h` — `std::optional<int>
  parseVolumeArgument(const std::string& payload)` and `std::vector<std::string>
  buildVolumeArgv(int level)` — consumed by both `plugin.cpp` (real execution) and
  `tests/system_control_plugin_test.cpp` (logic verification without touching the OS).

`plugin_internal.h` is a project-internal header (not part of the public ABI in
`plugin_sdk/jarvis_plugin_abi.h`) that only this plugin's `.cpp` and its own test file include —
it keeps the OS-command-construction logic unit-testable without linking a live `pactl`/`amixer`
process into the test binary. This mirrors `core/`'s existing pattern of factoring pure logic out
from I/O, applied at the plugin level for the first time (`system-info` had no branching logic
worth separating; this plugin does).

- [ ] **Step 1: Write the manifest**

`plugins/system-control/manifest.json`:

```json
{
  "id": "system-control",
  "version": "1.0.0",
  "abi_version": 1,
  "library": "libsystem_control_plugin.so",
  "capabilities": [
    {
      "intent": "volume",
      "description": "Gets or sets system output volume (0-100). Usage: volume get | volume set <0-100>",
      "power_tier": "T2_SYSTEM_AFFECTING"
    }
  ]
}
```

(The `shutdown` capability entry is added to this same manifest in Task 4 — one manifest per
plugin `.so`, matching how `system-info`'s single-capability manifest works today.)

- [ ] **Step 2: Write the failing test for the pure argument-parsing logic**

`tests/system_control_plugin_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include "../plugins/system-control/plugin_internal.h"

TEST(ParseVolumeArgumentTest, GetReturnsNullopt) {
    EXPECT_EQ(parseVolumeArgument("get"), std::nullopt);
}

TEST(ParseVolumeArgumentTest, SetWithValidLevelReturnsThatLevel) {
    EXPECT_EQ(parseVolumeArgument("set 42"), std::optional<int>(42));
}

TEST(ParseVolumeArgumentTest, SetWithZeroIsValid) {
    EXPECT_EQ(parseVolumeArgument("set 0"), std::optional<int>(0));
}

TEST(ParseVolumeArgumentTest, SetWithOneHundredIsValid) {
    EXPECT_EQ(parseVolumeArgument("set 100"), std::optional<int>(100));
}

TEST(ParseVolumeArgumentTest, SetAboveOneHundredIsRejected) {
    EXPECT_EQ(parseVolumeArgument("set 101"), std::nullopt);
}

TEST(ParseVolumeArgumentTest, SetBelowZeroIsRejected) {
    EXPECT_EQ(parseVolumeArgument("set -1"), std::nullopt);
}

TEST(ParseVolumeArgumentTest, SetWithNonNumericLevelIsRejected) {
    EXPECT_EQ(parseVolumeArgument("set loud"), std::nullopt);
}

TEST(ParseVolumeArgumentTest, EmptyPayloadIsRejected) {
    EXPECT_EQ(parseVolumeArgument(""), std::nullopt);
}

TEST(ParseVolumeArgumentTest, UnknownVerbIsRejected) {
    EXPECT_EQ(parseVolumeArgument("mute"), std::nullopt);
}

TEST(BuildVolumeArgvTest, ProducesPactlSetSinkVolumeCommand) {
    std::vector<std::string> argv = buildVolumeArgv(50);

    ASSERT_EQ(argv.size(), 4u);
    EXPECT_EQ(argv[0], "pactl");
    EXPECT_EQ(argv[1], "set-sink-volume");
    EXPECT_EQ(argv[2], "@DEFAULT_SINK@");
    EXPECT_EQ(argv[3], "50%");
}
```

- [ ] **Step 3: Run to verify it fails**

Run: `cmake -S . -B build && cmake --build build --target jarvis_tests`
Expected: FAIL — `plugin_internal.h` does not exist yet.

- [ ] **Step 4: Write `plugins/system-control/plugin_internal.h`**

```cpp
#pragma once

#include <optional>
#include <string>
#include <vector>

// Parses a "volume" capability payload. "get" -> std::nullopt (query the current level, don't
// set it). "set <0-100>" -> that level. Anything else (bad verb, out-of-range, non-numeric,
// empty) -> std::nullopt, treated by the caller as "not a set" and reported as an error there —
// kept pure and side-effect-free so it's unit-testable without a live audio backend.
std::optional<int> parseVolumeArgument(const std::string& payload);

// Builds the argv for `pactl set-sink-volume @DEFAULT_SINK@ <level>%`. Returned as a vector of
// separate argv entries (never a single shell string) so plugin.cpp can pass it straight to
// execvp — no shell is ever invoked, so there is no command-injection surface here regardless
// of what `level` is.
std::vector<std::string> buildVolumeArgv(int level);

// Builds the argv for `systemctl poweroff`. No arguments — kept as a function (not a literal)
// so shutdown's argv construction lives in the same tested, single place as volume's.
std::vector<std::string> buildShutdownArgv();
```

- [ ] **Step 5: Write the implementation half of `plugin_internal.h` — create `plugins/system-control/plugin_internal.cpp`**

```cpp
#include "plugin_internal.h"

#include <cctype>
#include <sstream>

std::optional<int> parseVolumeArgument(const std::string& payload) {
    std::istringstream stream(payload);
    std::string verb;
    stream >> verb;

    if (verb == "get") {
        return std::nullopt;
    }
    if (verb != "set") {
        return std::nullopt;
    }

    std::string levelText;
    if (!(stream >> levelText) || levelText.empty()) {
        return std::nullopt;
    }
    for (char c : levelText) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return std::nullopt;
        }
    }

    const int level = std::stoi(levelText);
    if (level < 0 || level > 100) {
        return std::nullopt;
    }
    return level;
}

std::vector<std::string> buildVolumeArgv(int level) {
    return {"pactl", "set-sink-volume", "@DEFAULT_SINK@", std::to_string(level) + "%"};
}

std::vector<std::string> buildShutdownArgv() {
    return {"systemctl", "poweroff"};
}
```

Note: `parseVolumeArgument`'s all-digit check rejects a leading `-` outright, which is why
`SetBelowZeroIsRejected` passes — `"set -1"`'s level token `"-1"` fails the digit-only loop before
the range check ever runs. Confirmed by the Step 2 test.

- [ ] **Step 6: Run tests to verify they pass**

Run: `cmake --build build --target jarvis_tests && ./build/jarvis_tests --gtest_filter='ParseVolumeArgumentTest.*:BuildVolumeArgvTest.*'`
Expected: PASS, all 10 cases.

- [ ] **Step 7: Write `plugins/system-control/plugin.cpp`**

```cpp
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
```

(`shutdown` registration is added to this same file in Task 4 — `jarvis_plugin_register` calls
`registerCapability` once per capability, same as any multi-capability plugin would.)

- [ ] **Step 8: Wire the plugin and its test into `CMakeLists.txt`**

Add near the existing `system_info_plugin` target (after its block, around line 185):

```cmake
add_library(system_control_plugin SHARED
    plugins/system-control/plugin.cpp
    plugins/system-control/plugin_internal.cpp)
target_include_directories(system_control_plugin PRIVATE ${CMAKE_SOURCE_DIR}/plugin_sdk)
set_target_properties(system_control_plugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins/system-control)
add_custom_command(TARGET system_control_plugin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_SOURCE_DIR}/plugins/system-control/manifest.json
        ${CMAKE_BINARY_DIR}/plugins/system-control/manifest.json)

add_dependencies(jarvis system_control_plugin)
add_dependencies(jarvis_grpc_server system_control_plugin)
add_dependencies(jarvis_tests system_control_plugin)
```

Add `tests/system_control_plugin_test.cpp` and
`plugins/system-control/plugin_internal.cpp` to `jarvis_tests`'s source list (next to
`tests/plugin_loader_test.cpp` at line ~113):

```cmake
add_executable(
    jarvis_tests
    tests/capability_registry_test.cpp
    tests/plugin_config_test.cpp
    tests/consent_gate_test.cpp
    tests/minimal_json_test.cpp
    tests/plugin_loader_test.cpp
    tests/system_control_plugin_test.cpp
    plugins/system-control/plugin_internal.cpp
    core/capability_registry.cpp
    core/command_handler.cpp
    core/engine.cpp
    core/plugin_config.cpp
    core/consent_gate.cpp
    core/minimal_json.cpp
    core/plugin_loader.cpp
)
```

(`plugin_internal.cpp` is compiled directly into `jarvis_tests` — same translation unit sharing
approach the rest of the test binary already uses for `core/*.cpp` — while `plugin.cpp` itself
stays out of `jarvis_tests`, since it pulls in `<spawn.h>` execution code the test binary should
never link or run.)

- [ ] **Step 9: Add `config/capabilities.cfg` entry**

Append:

```
volume.enabled=true
```

- [ ] **Step 10: Build everything and run the full test suite**

Run: `cmake --build build && ./build/jarvis_tests`
Expected: PASS — all suites including the 10 new `ParseVolumeArgumentTest`/`BuildVolumeArgvTest`
cases, and `system_control_plugin` builds to
`build/plugins/system-control/libsystem_control_plugin.so` with its `manifest.json` staged
alongside it.

- [ ] **Step 11: Manually verify `volume` end-to-end (safe — read-only `get` first)**

Run: `./build/jarvis` (interactive CLI), then type `volume get`.
Expected: since `volume` is T2 and ungranted, JARVIS responds with the consent-required message
naming `jarvis --grant volume`. Then run `./build/jarvis --grant volume`, answer `y`, restart
`./build/jarvis`, and type `volume get` again — expect a real `pactl`-backed response (or the
"is 'pactl' installed" fallback message if this machine doesn't have PulseAudio/PipeWire's
`pactl`, which is an acceptable, honestly-reported outcome per INV-7).

- [ ] **Step 12: Commit**

```bash
git add plugins/system-control tests/system_control_plugin_test.cpp CMakeLists.txt config/capabilities.cfg
git commit -m "feat(plugins): add system-control plugin with volume (T2)"
```

---

### Task 4: `system-control` plugin — add `shutdown` (T3)

**Files:**
- Modify: `plugins/system-control/manifest.json`
- Modify: `plugins/system-control/plugin.cpp`
- Modify: `tests/system_control_plugin_test.cpp`
- Modify: `config/capabilities.cfg`

**Interfaces:**
- Consumes: `buildShutdownArgv()` from `plugin_internal.h` (Task 3, already implemented and
  tested — no changes needed to `plugin_internal.h`/`.cpp` in this task).

- [ ] **Step 1: Write the failing test for `buildShutdownArgv`'s existing coverage gap**

`buildShutdownArgv()` was implemented in Task 3 but never asserted on directly (only
`buildVolumeArgv` was). Add to `tests/system_control_plugin_test.cpp`:

```cpp
TEST(BuildShutdownArgvTest, ProducesSystemctlPoweroffCommand) {
    std::vector<std::string> argv = buildShutdownArgv();

    ASSERT_EQ(argv.size(), 2u);
    EXPECT_EQ(argv[0], "systemctl");
    EXPECT_EQ(argv[1], "poweroff");
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --target jarvis_tests && ./build/jarvis_tests --gtest_filter='BuildShutdownArgvTest.*'`
Expected: this specific test actually already PASSES (the function was fully implemented in Task
3) — this step is a verification checkpoint, not a red step. If it fails, `plugin_internal.cpp`
regressed; stop and fix before continuing.

- [ ] **Step 3: Add `shutdown` to the manifest**

Update `plugins/system-control/manifest.json`'s `"capabilities"` array to:

```json
{
  "id": "system-control",
  "version": "1.0.0",
  "abi_version": 1,
  "library": "libsystem_control_plugin.so",
  "capabilities": [
    {
      "intent": "volume",
      "description": "Gets or sets system output volume (0-100). Usage: volume get | volume set <0-100>",
      "power_tier": "T2_SYSTEM_AFFECTING"
    },
    {
      "intent": "shutdown",
      "description": "Powers off the machine. Requires the word 'confirm' in the command every time (T3 — never covered by a grant). Usage: shutdown confirm",
      "power_tier": "T3_DESTRUCTIVE"
    }
  ]
}
```

- [ ] **Step 4: Add the `shutdown` capability to `plugins/system-control/plugin.cpp`**

Add this function alongside `volumeExecute` (inside the anonymous namespace):

```cpp
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
```

Update `jarvis_plugin_register` to register both capabilities:

```cpp
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
```

- [ ] **Step 5: Add `config/capabilities.cfg` entry**

Append:

```
shutdown.enabled=true
```

(Enabled by default like every other capability — INV-9's T3 confirm-token gate is what actually
protects it, not this file, matching how the existing T2 capabilities are enabled-by-default too
and rely on the grant gate for protection.)

- [ ] **Step 6: Build and run the full test suite**

Run: `cmake --build build && ./build/jarvis_tests`
Expected: PASS, including the new `BuildShutdownArgvTest` case and the existing `PluginLoaderTest`
suite (confirms the two-capability manifest still validates and loads cleanly — this is a real
regression risk since `PluginLoader` cross-checks manifest capability count against actual
`registerCapability` calls, and this task changes both at once).

- [ ] **Step 7: Verify the confirm-gate manually — do NOT actually shut the machine down**

Run: `./build/jarvis` and type `shutdown`.
Expected: JARVIS responds with the T3/T4 confirmation-required message from Task 1
(`... requires explicit confirmation on every call ... 'shutdown confirm'`), and the machine does
**not** shut down — `ConsentGate` denies before `shutdownExecute` is ever reached, so this check
is safe to run for real. **Do not type `shutdown confirm` during this verification pass** — that
argv is real (`systemctl poweroff`) and will power off the machine `runCommand` runs on. Verifying
that the confirmed path also reaches `shutdownExecute` is done by code review of Steps 1-4 above,
plus the unit-tested `buildShutdownArgv()` output, not by executing it.

- [ ] **Step 8: Commit**

```bash
git add plugins/system-control tests/system_control_plugin_test.cpp config/capabilities.cfg
git commit -m "feat(plugins): add shutdown (T3) to system-control, gated by confirm token"
```

---

### Task 5: Docs, checklist, and QMUL contemporaneous records

**Files:**
- Modify: `docs/features.md`
- Modify: `docs/roadmap.md`
- Create/Modify: `qmul/notes/genai-usage-log.md` (append-only)
- Create/Modify: `qmul/logbook/` (append a new dated entry file, per that directory's existing
  convention — check its current file-naming pattern before adding one)

**Interfaces:** None — this task is documentation only, no code interfaces produced or consumed.

- [ ] **Step 1: Update `docs/features.md`'s Phase 4 section**

In the Phase 4 block, change the consent-gate bullet's substrate description to note T3/T4 is now
enforced (not "future work"), and change:

```
- 📋 System control plugin — open apps, volume, lock screen, shutdown (needs plugin manager + safety guardrails)
```

to two lines — one marking what shipped, one marking what's explicitly deferred:

```
- ✅ **System control plugin (volume + shutdown slice)** — `system-control` is the second bundled
  dynamic plugin (`plugins/system-control/`), alongside `system-info`. `volume` (T2) gets/sets
  output level via `pactl`, gated by the existing grant flow (`jarvis --grant volume`).
  `shutdown` (T3) powers off via `systemctl poweroff`, gated by `ConsentGate`'s new per-call
  confirm-token enforcement (see the Phase 2.5/consent-gate note below) rather than a grant —
  proving both the T2 and T3 enforcement paths on a real capability. Both commands build their
  OS argv via `execvp`/`posix_spawnp`, never a shell string, so no capability payload can inject
  shell syntax.
- 📋 System control plugin — open apps, lock screen (deferred: arbitrary process launch needs its
  own allowlist design, scoped as a separate follow-up rather than folded into the volume/shutdown
  slice above)
```

Also find the Phase 4 substrate bullet describing `ConsentGate`'s T3/T4 behavior (currently reads
"T3/T4 always denied since enforcement for those tiers isn't implemented yet") and update it to:

```
T3/T4 requires the word "confirm" as a standalone token in the capability's payload on every
call — never satisfied by a persisted grant, per INV-9. Landed alongside the system-control
plugin's `shutdown` capability, the first T3 capability to exist.
```

- [ ] **Step 2: Update `docs/roadmap.md`'s Phase 4 section**

Append a new paragraph after the existing Phase 4 narrative (before the "remaining Phase 4 items"
line), describing what this plan added, matching the prose style of the existing entries (see the
"Next, the plugin SDK & dynamic loader landed..." paragraph for tone/format), and update the
"remaining Phase 4 items" list to drop "system control plugin" and add "open apps / lock screen
(system-control follow-up)" in its place.

- [ ] **Step 3: Append a Gen AI usage log entry**

Read `qmul/notes/genai-usage-log.md`'s existing entries for the exact per-entry template, then
append a new **Category C** entry dated with today's actual date, describing: the ConsentGate
T3/T4 change and the system-control plugin, how it was verified (unit tests run + manual CLI
checks in Tasks 3/4, explicitly noting `shutdown confirm` itself was never executed), and whether
it's defensible at a whiteboard (yes — every line is either a direct extension of the existing
T2 grant pattern or a POSIX `execvp` call with a hand-built argv, nothing library-magic).

- [ ] **Step 4: Append an engineering logbook entry**

Check `qmul/logbook/`'s existing file-naming convention (likely one dated file per entry or a
running file — read its README and most recent existing entry to match format exactly), then
append a factual entry: what was built (T3/T4 confirm-token enforcement + system-control plugin),
the decision to use a payload-text convention over a new proto field or two-phase RPC confirm flow
(and why — no new session state, no surface-specific code, keeps INV-1/INV-3 intact), the decision
to factor `plugin_internal.h` out for testability (first plugin to need it, since `system-info` had
no branching logic), and problems hit (if any arose during actual execution — e.g. `pactl`
missing on the dev machine, a manifest cross-check failure while wiring `shutdown` in Task 4, etc.
— fill in with what actually happened, not hypothetically).

- [ ] **Step 5: Commit**

```bash
git add docs/features.md docs/roadmap.md qmul/notes/genai-usage-log.md qmul/logbook/
git commit -m "docs: record T3/T4 consent enforcement and system-control plugin"
```

---

## Self-Review

**Spec coverage:** ConsentGate T3/T4 enforcement (Task 1-2), system-control plugin with volume T2
+ shutdown T3 (Task 3-4), features/roadmap/QMUL doc updates (Task 5, satisfying CLAUDE.md §4's
"update the feature checklist, not optional" and §6.2's standing documentation exception) — all
covered. "Open apps" and "lock screen" are explicitly scoped out per the Global Constraints section
and called out again in Task 5's doc updates, not silently dropped.

**Placeholder scan:** no TBD/TODO markers; every code step has real, complete code; every test has
concrete assertions.

**Type consistency:** `ConsentGate::check(const Capability&, const std::string&)` is defined once
in Task 1 and consumed identically in both `dispatch()` overloads (Task 2) — no signature drift.
`parseVolumeArgument`/`buildVolumeArgv`/`buildShutdownArgv` are declared once in
`plugin_internal.h` (Task 3) and used with matching signatures in both `plugin.cpp` and the test
file across Tasks 3-4.
