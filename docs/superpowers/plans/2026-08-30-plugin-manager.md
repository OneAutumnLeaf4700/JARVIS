# Plugin Manager (S2 completion) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add config-driven capability enable/disable and a real T2 consent gate to `CapabilityRegistry::dispatch()`, plus a CLI flag to grant T2 consent — with zero new plugins/capabilities.

**Architecture:** Two small new C++ classes (`PluginConfig`, `ConsentGate`) sit in front of the existing, unchanged `Capability::execute()` call inside `CapabilityRegistry::dispatch()`. `CapabilityRegistry` gets an optional `setPluginConfig()` — unset, dispatch behaves exactly as it does today (this keeps all 12 existing `capability_registry_test.cpp` tests compiling and passing completely unmodified). Both `jarvis` (CLI) and `jarvis_grpc_server` load `PluginConfig` at startup and wire it in, so the gate is live on both real dispatch paths.

**Tech Stack:** C++17, GTest, spdlog (already a project dependency) — no new libraries (INV-10: the config format is a hand-rolled flat `key=value` parser, not a YAML/JSON library, since the data is genuinely that simple).

**Spec:** `docs/superpowers/specs/2026-08-30-plugin-manager-design.md`

## Global Constraints

- No new capability/plugin implementation of any kind (file search, media control, etc.) — substrate only.
- `config/capabilities.cfg` is git-tracked; `config/consent_grants.cfg` is gitignored, per-machine.
- A missing `capabilities.cfg` → every capability defaults to enabled. A missing `consent_grants.cfg` → every capability defaults to not-granted.
- T3/T4 are always denied with an honest "not yet implemented" message — never silently allowed, never faked as enforced.
- Malformed config lines are skipped with a `spdlog::warn`, never fatal.
- Capabilities never invent their own consent check (INV-9) — both checks live in `CapabilityRegistry::dispatch()` only.
- `ai/` and `voice/` are untouched by this plan.

---

## Task 1: PluginConfig

**Files:**
- Create: `core/plugin_config.h`
- Create: `core/plugin_config.cpp`
- Create: `config/capabilities.cfg`
- Modify: `.gitignore` (add `config/consent_grants.cfg`)
- Modify: `CMakeLists.txt` (add `core/plugin_config.cpp` to `jarvis`, `jarvis_grpc_server`, and `jarvis_tests`; add `tests/plugin_config_test.cpp` to `jarvis_tests`; link `spdlog::spdlog` into `jarvis` and `jarvis_tests`)
- Test: `tests/plugin_config_test.cpp`

**Interfaces:**
- Produces: `class PluginConfig` with `static PluginConfig load(const std::string& capabilitiesPath, const std::string& grantsPath)`, `bool isEnabled(const std::string& capabilityName) const` (default `true`), `bool isGranted(const std::string& capabilityName) const` (default `false`), `void grant(const std::string& capabilityName)` (persists to the grants file passed to `load()`).

- [ ] **Step 1: Write the failing tests**

Create `tests/plugin_config_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include <fstream>

#include "plugin_config.h"

namespace {

std::string writeTempFile(const std::string& path, const std::string& contents) {
    std::ofstream file(path);
    file << contents;
    file.close();
    return path;
}

}  // namespace

TEST(PluginConfigTest, MissingCapabilitiesFileDefaultsEverythingEnabled) {
    PluginConfig config = PluginConfig::load(
        "does_not_exist_capabilities.cfg", "does_not_exist_grants.cfg");

    EXPECT_TRUE(config.isEnabled("echo"));
    EXPECT_TRUE(config.isEnabled("anything_unlisted"));
}

TEST(PluginConfigTest, MissingGrantsFileDefaultsEverythingNotGranted) {
    PluginConfig config = PluginConfig::load(
        "does_not_exist_capabilities.cfg", "does_not_exist_grants.cfg");

    EXPECT_FALSE(config.isGranted("volume_control"));
}

TEST(PluginConfigTest, ParsesExplicitEnabledFalse) {
    writeTempFile("test_capabilities_disabled.cfg", "echo.enabled=false\n");

    PluginConfig config = PluginConfig::load(
        "test_capabilities_disabled.cfg", "does_not_exist_grants.cfg");

    EXPECT_FALSE(config.isEnabled("echo"));
    EXPECT_TRUE(config.isEnabled("status"));  // unlisted stays enabled
}

TEST(PluginConfigTest, ParsesExplicitGrantedTrue) {
    writeTempFile("test_grants_present.cfg", "volume_control.granted=true\n");

    PluginConfig config = PluginConfig::load(
        "does_not_exist_capabilities.cfg", "test_grants_present.cfg");

    EXPECT_TRUE(config.isGranted("volume_control"));
    EXPECT_FALSE(config.isGranted("other_capability"));
}

TEST(PluginConfigTest, MalformedLinesAreSkippedNotFatal) {
    writeTempFile("test_capabilities_malformed.cfg",
        "this line has no equals sign\n"
        "echo.enabled=false\n"
        "=noname\n"
        "novalue.=true\n");

    PluginConfig config = PluginConfig::load(
        "test_capabilities_malformed.cfg", "does_not_exist_grants.cfg");

    // The one well-formed line still took effect; the malformed ones were skipped, not fatal.
    EXPECT_FALSE(config.isEnabled("echo"));
}

TEST(PluginConfigTest, GrantPersistsToDiskAndIsReReadOnFreshLoad) {
    // Start from a clean grants file for this test.
    writeTempFile("test_grants_roundtrip.cfg", "");

    PluginConfig config = PluginConfig::load(
        "does_not_exist_capabilities.cfg", "test_grants_roundtrip.cfg");
    EXPECT_FALSE(config.isGranted("volume_control"));

    config.grant("volume_control");
    EXPECT_TRUE(config.isGranted("volume_control"));  // in-memory takes effect immediately

    PluginConfig reloaded = PluginConfig::load(
        "does_not_exist_capabilities.cfg", "test_grants_roundtrip.cfg");
    EXPECT_TRUE(reloaded.isGranted("volume_control"));  // and survives a fresh load()
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake -S . -B build && cmake --build build --target jarvis_tests`
Expected: build FAILS — `plugin_config.h` does not exist yet.

- [ ] **Step 3: Write `core/plugin_config.h`**

```cpp
#pragma once

#include <string>
#include <unordered_map>

// Loads two flat, line-based "key=value" config files — no YAML/JSON dependency, the data is
// genuinely this simple (INV-10). Lines look like "<capability_name>.enabled=true" or
// "<capability_name>.granted=true". A missing file is not an error: every lookup just falls
// back to its documented default.
class PluginConfig {
 public:
    static PluginConfig load(const std::string& capabilitiesPath, const std::string& grantsPath);

    // Default true if capabilityName is unlisted — fail-open for availability, matching how
    // the system behaves today with zero config.
    bool isEnabled(const std::string& capabilityName) const;

    // Default false if capabilityName is unlisted — fail-closed for consent (INV-9's intent).
    bool isGranted(const std::string& capabilityName) const;

    // Records consent in memory and appends it to the grants file passed to load().
    void grant(const std::string& capabilityName);

 private:
    std::unordered_map<std::string, bool> enabled_;
    std::unordered_map<std::string, bool> granted_;
    std::string grantsPath_;
};
```

- [ ] **Step 4: Write `core/plugin_config.cpp`**

```cpp
#include "plugin_config.h"

#include <fstream>
#include <functional>

#include <spdlog/spdlog.h>

namespace {

// Parses "<name>.<field>=<value>" lines from path, calling onEntry(name, field, value) for each
// well-formed one. Malformed lines are skipped with a warning, never fatal — a typo in a
// hand-edited config file shouldn't crash startup. A missing file yields no entries at all,
// letting the caller's own defaults apply untouched.
void parseKeyValueFile(
    const std::string& path,
    const std::function<void(const std::string&, const std::string&, const std::string&)>& onEntry) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }

        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            spdlog::warn("PluginConfig: skipping malformed line in {}: '{}'", path, line);
            continue;
        }

        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);

        const auto dot = key.rfind('.');
        if (dot == std::string::npos || dot == 0 || dot == key.size() - 1) {
            spdlog::warn("PluginConfig: skipping malformed line in {}: '{}'", path, line);
            continue;
        }

        onEntry(key.substr(0, dot), key.substr(dot + 1), value);
    }
}

bool parseBool(const std::string& value) {
    return value == "true";
}

}  // namespace

PluginConfig PluginConfig::load(const std::string& capabilitiesPath, const std::string& grantsPath) {
    PluginConfig config;
    config.grantsPath_ = grantsPath;

    parseKeyValueFile(capabilitiesPath,
        [&config, &capabilitiesPath](const std::string& name, const std::string& field, const std::string& value) {
            if (field == "enabled") {
                config.enabled_[name] = parseBool(value);
            } else {
                spdlog::warn("PluginConfig: unknown field '{}' for '{}' in {}", field, name, capabilitiesPath);
            }
        });

    parseKeyValueFile(grantsPath,
        [&config, &grantsPath](const std::string& name, const std::string& field, const std::string& value) {
            if (field == "granted") {
                config.granted_[name] = parseBool(value);
            } else {
                spdlog::warn("PluginConfig: unknown field '{}' for '{}' in {}", field, name, grantsPath);
            }
        });

    return config;
}

bool PluginConfig::isEnabled(const std::string& capabilityName) const {
    const auto it = enabled_.find(capabilityName);
    if (it == enabled_.end()) {
        return true;
    }
    return it->second;
}

bool PluginConfig::isGranted(const std::string& capabilityName) const {
    const auto it = granted_.find(capabilityName);
    if (it == granted_.end()) {
        return false;
    }
    return it->second;
}

void PluginConfig::grant(const std::string& capabilityName) {
    granted_[capabilityName] = true;

    std::ofstream file(grantsPath_, std::ios::app);
    file << capabilityName << ".granted=true\n";
}
```

- [ ] **Step 5: Create the default `config/capabilities.cfg`**

```
# JARVIS capability enable/disable config. Format: <capability_name>.enabled=true|false
# A capability not listed here defaults to enabled. This file ships with every builtin
# listed explicitly, enabled, for documentation value.
echo.enabled=true
about.enabled=true
status.enabled=true
help.enabled=true
```

- [ ] **Step 6: Add the gitignore entry**

Add to `.gitignore` (near the other per-machine/runtime entries):

```
# Per-machine T2 consent grants — never shared across machines
config/consent_grants.cfg
```

- [ ] **Step 7: Wire into CMakeLists.txt**

In `CMakeLists.txt`:
- Add `core/plugin_config.cpp` to the `jarvis` executable's source list (after `core/capability_registry.cpp`).
- Add `spdlog::spdlog` to `jarvis`'s `target_link_libraries` (it didn't need spdlog before; it does now via plugin_config.cpp).
- Add `core/plugin_config.cpp` to the `jarvis_grpc_server` executable's source list.
- Add `tests/plugin_config_test.cpp` and `core/plugin_config.cpp` to the `jarvis_tests` executable's source list.
- Add `spdlog::spdlog` to `jarvis_tests`'s `target_link_libraries` (it didn't need spdlog before either).

- [ ] **Step 8: Run tests to verify they pass**

Run: `cmake --build build --target jarvis_tests && ./build/jarvis_tests --gtest_filter=PluginConfigTest.*`
Expected: PASS (all 6 new tests)

- [ ] **Step 9: Commit**

```bash
git add core/plugin_config.h core/plugin_config.cpp tests/plugin_config_test.cpp config/capabilities.cfg .gitignore CMakeLists.txt
git commit -m "feat(core): add PluginConfig for capability enable/disable and consent grants"
```

---

## Task 2: ConsentGate

**Files:**
- Create: `core/consent_gate.h`
- Create: `core/consent_gate.cpp`
- Modify: `CMakeLists.txt` (add `core/consent_gate.cpp` to `jarvis`, `jarvis_grpc_server`, `jarvis_tests`; add `tests/consent_gate_test.cpp` to `jarvis_tests`)
- Test: `tests/consent_gate_test.cpp`

**Interfaces:**
- Consumes: `PluginConfig` (Task 1) — `bool isGranted(const std::string&) const`. `Capability`/`PowerTier` from `core/capability.h` — `capability.name`, `capability.powerTier`.
- Produces: `struct ConsentResult { bool allowed; std::string reason; };` and `class ConsentGate` with `explicit ConsentGate(const PluginConfig& config)` and `ConsentResult check(const Capability& capability) const`.

- [ ] **Step 1: Write the failing tests**

Create `tests/consent_gate_test.cpp`:

```cpp
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

    ConsentResult result = gate.check(makeCapability("echo", PowerTier::T0_READ_ONLY));

    EXPECT_TRUE(result.allowed);
    EXPECT_TRUE(result.reason.empty());
}

TEST(ConsentGateTest, T1AlwaysAllowedRegardlessOfConfig) {
    PluginConfig config = PluginConfig::load("does_not_exist.cfg", "does_not_exist.cfg");
    ConsentGate gate(config);

    ConsentResult result = gate.check(makeCapability("some_stateful_thing", PowerTier::T1_STATEFUL_LOCAL));

    EXPECT_TRUE(result.allowed);
}

TEST(ConsentGateTest, T2DeniedWithoutGrant) {
    PluginConfig config = PluginConfig::load("does_not_exist.cfg", "does_not_exist.cfg");
    ConsentGate gate(config);

    ConsentResult result = gate.check(makeCapability("volume_control", PowerTier::T2_SYSTEM_AFFECTING));

    EXPECT_FALSE(result.allowed);
    EXPECT_NE(result.reason.find("volume_control"), std::string::npos);
    EXPECT_NE(result.reason.find("--grant"), std::string::npos);
}

TEST(ConsentGateTest, T2AllowedWithGrant) {
    PluginConfig config = PluginConfig::load("does_not_exist.cfg", "does_not_exist.cfg");
    config.grant("volume_control");
    ConsentGate gate(config);

    ConsentResult result = gate.check(makeCapability("volume_control", PowerTier::T2_SYSTEM_AFFECTING));

    EXPECT_TRUE(result.allowed);
    EXPECT_TRUE(result.reason.empty());
}

TEST(ConsentGateTest, T3AlwaysDeniedEvenIfSomehowGranted) {
    PluginConfig config = PluginConfig::load("does_not_exist.cfg", "does_not_exist.cfg");
    config.grant("delete_files");  // granting is meaningless for T3 — gate must ignore it
    ConsentGate gate(config);

    ConsentResult result = gate.check(makeCapability("delete_files", PowerTier::T3_DESTRUCTIVE));

    EXPECT_FALSE(result.allowed);
    EXPECT_NE(result.reason.find("not yet implemented"), std::string::npos);
}

TEST(ConsentGateTest, T4AlwaysDeniedEvenIfSomehowGranted) {
    PluginConfig config = PluginConfig::load("does_not_exist.cfg", "does_not_exist.cfg");
    config.grant("call_external_api");
    ConsentGate gate(config);

    ConsentResult result = gate.check(makeCapability("call_external_api", PowerTier::T4_EXTERNAL));

    EXPECT_FALSE(result.allowed);
    EXPECT_NE(result.reason.find("not yet implemented"), std::string::npos);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build --target jarvis_tests`
Expected: build FAILS — `consent_gate.h` does not exist yet.

- [ ] **Step 3: Write `core/consent_gate.h`**

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
// CapabilityRegistry::dispatch() only.
class ConsentGate {
 public:
    explicit ConsentGate(const PluginConfig& config);
    ConsentResult check(const Capability& capability) const;

 private:
    const PluginConfig& config_;
};
```

- [ ] **Step 4: Write `core/consent_gate.cpp`**

```cpp
#include "consent_gate.h"

ConsentGate::ConsentGate(const PluginConfig& config) : config_(config) {}

ConsentResult ConsentGate::check(const Capability& capability) const {
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
            return {false, "Power tier T3/T4 enforcement is not yet implemented — capability '" +
                                capability.name + "' cannot run yet."};
    }
    return {false, "Unknown power tier."};
}
```

- [ ] **Step 5: Wire into CMakeLists.txt**

- Add `core/consent_gate.cpp` to the `jarvis` executable's source list.
- Add `core/consent_gate.cpp` to the `jarvis_grpc_server` executable's source list.
- Add `tests/consent_gate_test.cpp` and `core/consent_gate.cpp` to the `jarvis_tests` executable's source list.

- [ ] **Step 6: Run tests to verify they pass**

Run: `cmake --build build --target jarvis_tests && ./build/jarvis_tests --gtest_filter=ConsentGateTest.*`
Expected: PASS (all 6 new tests)

- [ ] **Step 7: Commit**

```bash
git add core/consent_gate.h core/consent_gate.cpp tests/consent_gate_test.cpp CMakeLists.txt
git commit -m "feat(core): add ConsentGate mapping PowerTier to allow/deny decisions"
```

---

## Task 3: Wire the gate into `CapabilityRegistry::dispatch()`

**Files:**
- Modify: `core/capability_registry.h`
- Modify: `core/capability_registry.cpp`
- Modify: `tests/capability_registry_test.cpp` (append new tests — do not touch the 12 existing ones)

**Interfaces:**
- Consumes: `PluginConfig` (Task 1), `ConsentGate`/`ConsentResult` (Task 2).
- Produces: `CapabilityRegistry::setPluginConfig(const PluginConfig* pluginConfig)`. `dispatch()`'s signature is unchanged — this is what keeps all 12 existing tests and both existing production call sites (`core/engine.cpp:47`, `core/jarvis_service.cpp:44,61`) compiling untouched.

- [ ] **Step 1: Write the failing tests**

Append to the end of `tests/capability_registry_test.cpp` (add `#include "consent_gate.h"` and `#include "plugin_config.h"` to its includes at the top first):

```cpp
TEST(CapabilityRegistryPluginGateTest, UnsetPluginConfigBehavesExactlyLikeBefore) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);
    // setPluginConfig() never called — this is the default, zero-config state.

    Engine engine;
    ExecutionContext context{engine, registry};
    std::optional<std::string> result = registry.dispatch(CommandType::ECHO, "hello", context);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "hello");
}

TEST(CapabilityRegistryPluginGateTest, DisabledCapabilityDispatchesLikeUnregistered) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    PluginConfig config = PluginConfig::load("does_not_exist.cfg", "does_not_exist.cfg");
    // Simulate a disabled capability by writing a real file, since PluginConfig has no
    // in-memory "disable" setter — only load() populates enabled_.
    {
        std::ofstream file("test_registry_disabled.cfg");
        file << "echo.enabled=false\n";
    }
    config = PluginConfig::load("test_registry_disabled.cfg", "does_not_exist.cfg");
    registry.setPluginConfig(&config);

    Engine engine;
    ExecutionContext context{engine, registry};
    std::optional<std::string> result = registry.dispatch(CommandType::ECHO, "hello", context);

    EXPECT_FALSE(result.has_value());  // same as CommandType::UNKNOWN would return
}

TEST(CapabilityRegistryPluginGateTest, EnabledT0CapabilityStillDispatchesWithPluginConfigSet) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    PluginConfig config = PluginConfig::load("does_not_exist.cfg", "does_not_exist.cfg");
    registry.setPluginConfig(&config);

    Engine engine;
    ExecutionContext context{engine, registry};
    std::optional<std::string> result = registry.dispatch(CommandType::STATUS, "", context);

    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->find("Engine: running"), std::string::npos);
}

TEST(CapabilityRegistryPluginGateTest, T2CapabilityWithoutGrantReturnsDenialInsteadOfExecuting) {
    CapabilityRegistry registry;
    bool executed = false;
    registry.registerCapability(Capability{
        "volume_control", CommandType::ECHO, "test", PowerTier::T2_SYSTEM_AFFECTING,
        [&executed](const std::string& payload, ExecutionContext&) {
            executed = true;
            return payload;
        }
    });

    PluginConfig config = PluginConfig::load("does_not_exist.cfg", "does_not_exist.cfg");
    registry.setPluginConfig(&config);

    Engine engine;
    ExecutionContext context{engine, registry};
    std::optional<std::string> result = registry.dispatch(CommandType::ECHO, "up", context);

    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->find("requires consent"), std::string::npos);
    EXPECT_FALSE(executed);
}

TEST(CapabilityRegistryPluginGateTest, T2CapabilityWithGrantExecutesNormally) {
    CapabilityRegistry registry;
    registry.registerCapability(Capability{
        "volume_control", CommandType::ECHO, "test", PowerTier::T2_SYSTEM_AFFECTING,
        [](const std::string& payload, ExecutionContext&) { return "volume set to " + payload; }
    });

    PluginConfig config = PluginConfig::load("does_not_exist.cfg", "does_not_exist.cfg");
    config.grant("volume_control");
    registry.setPluginConfig(&config);

    Engine engine;
    ExecutionContext context{engine, registry};
    std::optional<std::string> result = registry.dispatch(CommandType::ECHO, "50", context);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "volume set to 50");
}

TEST(CapabilityRegistryPluginGateTest, AllFourBuiltinsStillDispatchWithPluginConfigSetAndEmptyConfig) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    PluginConfig config = PluginConfig::load("does_not_exist.cfg", "does_not_exist.cfg");
    registry.setPluginConfig(&config);

    Engine engine;
    ExecutionContext context{engine, registry};

    EXPECT_TRUE(registry.dispatch(CommandType::ECHO, "hi", context).has_value());
    EXPECT_TRUE(registry.dispatch(CommandType::STATUS, "", context).has_value());
    EXPECT_TRUE(registry.dispatch(CommandType::ABOUT, "", context).has_value());
    EXPECT_TRUE(registry.dispatch(CommandType::HELP, "", context).has_value());
}
```

Also add `#include <fstream>` to the top of `tests/capability_registry_test.cpp` (needed by the disabled-capability test above).

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build --target jarvis_tests`
Expected: build FAILS — `CapabilityRegistry` has no `setPluginConfig` member yet.

- [ ] **Step 3: Modify `core/capability_registry.h`**

Add `#include "plugin_config.h"` to the top of the includes, and add this public member to `CapabilityRegistry` (after `dispatch()`, before `all()`):

```cpp
    // Wires plugin enable/disable + T2 consent gating into dispatch(). Not calling this at all
    // (pluginConfig_ stays nullptr) means dispatch() behaves exactly as it did before this
    // feature existed — every existing caller and test keeps working unmodified.
    void setPluginConfig(const PluginConfig* pluginConfig);
```

And add the new private member below `capabilities_`:

```cpp
    const PluginConfig* pluginConfig_ = nullptr;
```

- [ ] **Step 4: Modify `core/capability_registry.cpp`**

Add `#include "consent_gate.h"` to the top of the includes. Replace the existing `dispatch()` implementation:

```cpp
std::optional<std::string> CapabilityRegistry::dispatch(
    CommandType intent, const std::string& payload, ExecutionContext& context) const {
    const Capability* capability = resolve(intent);
    if (!capability) {
        return std::nullopt;
    }

    if (pluginConfig_ != nullptr) {
        if (!pluginConfig_->isEnabled(capability->name)) {
            return std::nullopt;
        }

        ConsentGate gate(*pluginConfig_);
        ConsentResult consent = gate.check(*capability);
        if (!consent.allowed) {
            return consent.reason;
        }
    }

    return capability->execute(payload, context);
}
```

Add the new setter, right after `registerCapability()`:

```cpp
void CapabilityRegistry::setPluginConfig(const PluginConfig* pluginConfig) {
    pluginConfig_ = pluginConfig;
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cmake --build build --target jarvis_tests && ./build/jarvis_tests`
Expected: PASS — all pre-existing tests (unchanged) plus the 6 new `CapabilityRegistryPluginGateTest` cases, plus all Task 1/2 tests.

- [ ] **Step 6: Commit**

```bash
git add core/capability_registry.h core/capability_registry.cpp tests/capability_registry_test.cpp
git commit -m "feat(core): gate CapabilityRegistry::dispatch() on plugin enable/disable + consent"
```

---

## Task 4: `jarvis --grant` CLI flag and production wiring

**Files:**
- Modify: `core/main.cpp`
- Modify: `core/grpc_server_main.cpp`

**Interfaces:**
- Consumes: `PluginConfig::load()` (Task 1), `CapabilityRegistry::setPluginConfig()` (Task 3), `Capability`/`PowerTier` (existing `core/capability.h`).
- Produces: nothing further downstream — this is the outermost wiring layer.

- [ ] **Step 1: Modify `core/main.cpp`**

Replace the full file contents:

```cpp
#include <iostream>
#include <string>

#include "capability_registry.h"
#include "engine.h"
#include "plugin_config.h"

namespace {

// The only interactive consent surface (per the design spec §3.4) — CLI-only, since it's the
// one surface with a real terminal attached. Returns the process exit code.
int runGrantFlow(const std::string& capabilityName, const CapabilityRegistry& registry) {
    const Capability* capability = nullptr;
    for (const auto& [intent, cap] : registry.all()) {
        if (cap.name == capabilityName) {
            capability = &cap;
            break;
        }
    }

    if (capability == nullptr) {
        std::cout << "Unknown capability: " << capabilityName << "\n";
        std::cout << "Registered capabilities:\n";
        for (const auto& [intent, cap] : registry.all()) {
            std::cout << "  - " << cap.name << "\n";
        }
        return 1;
    }

    if (capability->powerTier == PowerTier::T3_DESTRUCTIVE ||
        capability->powerTier == PowerTier::T4_EXTERNAL) {
        std::cout << "'" << capabilityName << "' is power tier T3/T4 — enforcement isn't "
                     "implemented yet, so it cannot be granted.\n";
        return 1;
    }

    if (capability->powerTier != PowerTier::T2_SYSTEM_AFFECTING) {
        std::cout << "'" << capabilityName << "' is power tier T0/T1 — it doesn't require a "
                     "consent grant.\n";
        return 1;
    }

    std::cout << "Grant consent for '" << capabilityName << "' (power tier T2)? [y/n] ";
    std::string answer;
    std::getline(std::cin, answer);
    if (answer != "y" && answer != "Y") {
        std::cout << "Not granted.\n";
        return 0;
    }

    PluginConfig config = PluginConfig::load("config/capabilities.cfg", "config/consent_grants.cfg");
    config.grant(capabilityName);
    std::cout << "Granted.\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Engine engine;
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    if (argc == 3 && std::string(argv[1]) == "--grant") {
        return runGrantFlow(argv[2], registry);
    }

    PluginConfig pluginConfig = PluginConfig::load("config/capabilities.cfg", "config/consent_grants.cfg");
    registry.setPluginConfig(&pluginConfig);

    engine.run(registry);

    return 0;
}
```

- [ ] **Step 2: Modify `core/grpc_server_main.cpp`**

Add `#include "plugin_config.h"` to the includes, and insert plugin config loading right after `registerBuiltinCapabilities(registry);`:

```cpp
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    PluginConfig pluginConfig = PluginConfig::load("config/capabilities.cfg", "config/consent_grants.cfg");
    registry.setPluginConfig(&pluginConfig);
```

(`pluginConfig` must be declared before `JarvisServiceImpl service(...)` and stay alive for the lifetime of `main()` — same scope as `registry` and `engine`, so this is safe: the server runs until `server->Wait()` returns, at which point everything unwinds together.)

- [ ] **Step 3: Build both binaries**

Run: `cmake --build build --target jarvis jarvis_grpc_server`
Expected: both build cleanly.

- [ ] **Step 4: Manual verification — unknown capability**

Run: `./build/jarvis --grant nonexistent_thing`
Expected: prints "Unknown capability: nonexistent_thing" and the list of 4 registered builtins (echo, about, status, help), exits with code 1.

Run: `echo $?`
Expected: `1`

- [ ] **Step 5: Manual verification — T0 capability doesn't need a grant**

Run: `./build/jarvis --grant echo`
Expected: prints "'echo' is power tier T0/T1 — it doesn't require a consent grant.", exits 0.

- [ ] **Step 6: Manual verification — existing behavior is unchanged**

Run: `echo -e "status\nexit" | ./build/jarvis`
Expected: identical output to before this plan — `status` still runs and reports engine state, since it's a T0 builtin listed as `enabled=true` in `config/capabilities.cfg`.

- [ ] **Step 7: Run the full test suite one more time**

Run: `cmake --build build --target jarvis_tests && ./build/jarvis_tests`
Expected: PASS — every test in the project, unchanged and new.

- [ ] **Step 8: Commit**

```bash
git add core/main.cpp core/grpc_server_main.cpp
git commit -m "feat(core): add jarvis --grant CLI flag, wire PluginConfig into both entry points"
```

---

## Post-plan housekeeping (not a task — do after Task 4, per project CLAUDE.md §4)

- Update `docs/features.md`: mark the Plugin Manager substrate as done, explicitly noting it ships zero new plugins by design.
- Update `docs/roadmap.md` Phase 4 section to reflect this piece landed.
- Append to `qmul/notes/genai-usage-log.md` and a new `qmul/logbook/2026-08-30-plugin-manager.md` entry, per the standing documentation duty (project CLAUDE.md §6.2).
