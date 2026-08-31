# Plugin SDK & Dynamic Loader Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a versioned C ABI + manifest-validated `dlopen` plugin loader to JARVIS's C++
core, and convert `system-info` into the bundled reference dynamic plugin, proving
discover → load → dispatch → disable → unload end-to-end.

**Architecture:** Task 1 first proves the CLI/gRPC string-intent bridging using the
already-compiled-in `system-info` capability (lower risk, testable in isolation). Tasks 2-3
build the hand-rolled JSON parser and the pure-C plugin ABI header, independently of each
other. Task 4 builds `PluginLoader` on top of both. Task 5 builds real `.so` fixture plugins
(valid + two deliberately-invalid) purely for `PluginLoader`'s own test suite. Task 6 converts
`system-info` into the actual bundled dynamic plugin and wires `PluginLoader` into both
`jarvis` and `jarvis_grpc_server` startup.

**Tech Stack:** C++17, `dlfcn.h` (Linux), GTest, spdlog — no new external dependency (the
manifest JSON parser is hand-rolled per the spec's INV-10 reasoning — `nlohmann-json` is not
installed on this machine and installing it or fetching it over the network was avoided).

**Spec:** `docs/superpowers/specs/2026-08-31-plugin-sdk-loader-design.md`

## Global Constraints

- Linux-first: `dlfcn.h`/`dlopen`/`dlsym`/`dlclose`. No Windows/macOS loader backend.
- A plugin never crosses C++ types across the `.so` boundary — the ABI (`plugin_sdk/jarvis_plugin_abi.h`) is pure C.
- Manifest validation (JSON parse, ABI version, tier names, duplicate intents) happens fully
  *before* `dlopen` is ever called on a candidate plugin's library.
- A plugin's actual registered capabilities must match its manifest's declared ones
  (intent name + power tier) — mismatch is rejected, nothing from that plugin is committed to
  `CapabilityRegistry`.
- `unloadPlugin()` refuses while any invocation of that plugin is in flight, or before the
  plugin has been disabled.
- One bad plugin directory never aborts discovery of the rest — malformed manifests, ABI
  mismatches, and `dlopen`/`dlsym` failures are all logged via `spdlog::warn` and skipped.
- `config/plugin_dirs.cfg` (git-tracked) and any plugin directories it names follow the same
  fail-open-on-missing-file discipline as `PluginConfig`.
- `ai/`, `voice/` stay untouched except where explicitly noted (Task 1's classifier-intent
  normalization touches only `core/jarvis_service.cpp`, not the Python classifier itself).

---

## Task 1: Revert `SYSTEM_INFO` enum; bridge CLI and gRPC onto string-intent dispatch

**Files:**
- Modify: `proto/jarvis.proto`
- Modify: `core/command_handler.h`, `core/command_handler.cpp`
- Modify: `core/capability_registry.cpp` (`makeSystemInfoCapability`, `makeHelpCapability`)
- Modify: `core/engine.cpp`
- Modify: `core/jarvis_service.cpp`
- Modify: `tools/interactive_client.py`, `tools/grpc_smoke_test.py`, `voice/voice_client.py`
- Modify: `tests/capability_registry_test.cpp`
- Test: same file (adjust/extend existing tests)

**Interfaces:**
- Consumes: `CapabilityRegistry::resolve(const std::string&)` / `dispatch(const std::string&, ...)` (already landed), `CapabilityRegistry::allByIntent()` (already landed).
- Produces: nothing new downstream — this task proves the bridging mechanism Task 6 will reuse for the *real* dynamically-loaded `system-info`.

- [ ] **Step 1: Revert the `SYSTEM_INFO` enum from the proto**

In `proto/jarvis.proto`, remove these two lines (added in the immediately prior commit, never
released/pushed — no wire compatibility exists to preserve):
```
    // Extensible capability intent. When set, this takes precedence over the legacy enum.
```
stays (that's the `intent` field, keep it). Remove only:
```
    COMMAND_TYPE_SYSTEM_INFO = 7;
```
from the `CommandType` enum block.

- [ ] **Step 2: Regenerate proto stubs**

Run:
```bash
protoc --proto_path=proto \
       --cpp_out=generated/cpp \
       --grpc_out=generated/cpp \
       --plugin=protoc-gen-grpc=$(which grpc_cpp_plugin) \
       proto/jarvis.proto proto/ai.proto

.venv/bin/python -m grpc_tools.protoc -Iproto \
       --python_out=generated/python \
       --grpc_python_out=generated/python \
       proto/jarvis.proto proto/ai.proto
```
Expected: both commands exit 0, `generated/cpp/jarvis.pb.h` and `generated/python/jarvis_pb2.py` no longer define a `SYSTEM_INFO`/`COMMAND_TYPE_SYSTEM_INFO` symbol.

- [ ] **Step 3: Remove `SYSTEM_INFO` from `core/command_handler.h`/`.cpp`**

In `core/command_handler.h`, remove `SYSTEM_INFO,` from the `CommandType` enum (leave `STATUS`
as the last remaining pre-existing entry).

In `core/command_handler.cpp`, remove this line from `COMMAND_MAP`:
```cpp
    {"system-info", CommandType::SYSTEM_INFO}
```
(leave the trailing comma correctly attached to the `status` line above it, i.e. that line
becomes the last entry in the map with no trailing comma).

- [ ] **Step 4: `makeSystemInfoCapability()` registers by intent name only**

In `core/capability_registry.cpp`, change `makeSystemInfoCapability()`'s second field from
`CommandType::SYSTEM_INFO` to `CommandType::UNKNOWN`:

```cpp
Capability makeSystemInfoCapability() {
    return Capability{
        "system-info",
        CommandType::UNKNOWN,
        "Shows local OS, architecture, compiler, and hardware-thread information. Usage: system-info",
        PowerTier::T0_READ_ONLY,
        [](const std::string& /*payload*/, ExecutionContext& /*context*/) -> std::string {
            std::ostringstream out;
            out << "System information:\n";
            out << "OS: " << operatingSystemName() << "\n";
            out << "Architecture: " << architectureName() << "\n";
            out << "Compiler: " << compilerName() << "\n";
            out << "C++ standard: " << __cplusplus << "\n";
            out << "Hardware threads: ";
            const unsigned int threadCount = std::thread::hardware_concurrency();
            if (threadCount == 0) {
                out << "unavailable";
            } else {
                out << threadCount;
            }
            return out.str();
        }
    };
}
```
(Only the `CommandType::SYSTEM_INFO` → `CommandType::UNKNOWN` change; the lambda body is
unchanged. `registerCapability()`'s existing `intentName.empty()` default fills in
`intentName = "system-info"` from `capability.name`, and its
`if (capability.intent != CommandType::UNKNOWN)` guard now skips inserting this into the
`capabilities_` (CommandType-keyed) map — it only lives in `namedCapabilities_` from here on.)

- [ ] **Step 5: `makeHelpCapability()` iterates `allByIntent()`, not `all()`**

`context.registry.all()` is CommandType-keyed and will no longer include `system-info` once
Step 4 lands. In `core/capability_registry.cpp`'s `makeHelpCapability()`, change **both**
occurrences of `context.registry.all()` to `context.registry.allByIntent()`:

```cpp
            if (payload.empty()) {
                out << "Available commands:\n";
                for (const auto& [intent, capability] : context.registry.allByIntent()) {
                    if (isDisabled(capability)) {
                        continue;
                    }
                    out << "  - " << capability.name << ": " << capability.description << "\n";
                }
                out << "  - exit: " << kExitDescription << "\n";
                return out.str();
            }

            std::string commandName = toLower(payload);

            if (commandName == "exit") {
                return "exit: " + kExitDescription;
            }

            for (const auto& [intent, capability] : context.registry.allByIntent()) {
                if (capability.name == commandName && !isDisabled(capability)) {
                    out << capability.name << ": " << capability.description;
                    return out.str();
                }
            }
```
(Everything else in `makeHelpCapability()` — the `isDisabled` lambda, the `exit` special case,
the not-found fallback — is unchanged. `allByIntent()` contains every builtin including the
4 CommandType-backed ones, since `registerCapability()` always populates `namedCapabilities_`
regardless of whether a real `CommandType` also exists — so this is a strictly more-inclusive
iteration source, not a behavior change for `echo`/`about`/`status`/`help` themselves.)

- [ ] **Step 6: CLI fallback — `core/engine.cpp`**

Replace `Engine::run()`'s dispatch block:

```cpp
        ParsedCommand parsed = parseCommand(input);

        //Update last command tracker to current command
        if (parsed.type != CommandType::STATUS) {
            std::string commandName = extractCommandName(input);
            if (!commandName.empty()) {
                lastCommand = commandName;
            }
        }

        //Engine layer handles termination — exit stays outside the registry
        if (parsed.type == CommandType::EXIT) {
            terminate();
            continue;
        }

        ExecutionContext context{*this, registry};
        std::optional<std::string> output = registry.dispatch(parsed.type, parsed.payload, context);

        if (output) {
            if (!output->empty()) {
                std::cout << *output << std::endl;
            }
        } else if (parsed.type == CommandType::UNKNOWN) {
            std::cout << runUnknown() << std::endl;
        } else {
            // Resolved command, but disabled — a distinct outcome from "unrecognised" (INV-7).
            std::cout << "Command is currently unavailable." << std::endl;
        }
```

with:

```cpp
        ParsedCommand parsed = parseCommand(input);
        std::string commandName = extractCommandName(input);

        //Update last command tracker to current command
        if (parsed.type != CommandType::STATUS) {
            if (!commandName.empty()) {
                lastCommand = commandName;
            }
        }

        //Engine layer handles termination — exit stays outside the registry
        if (parsed.type == CommandType::EXIT) {
            terminate();
            continue;
        }

        ExecutionContext context{*this, registry};
        std::optional<std::string> output = registry.dispatch(parsed.type, parsed.payload, context);

        // A word the CommandType enum doesn't know about might still be a plugin-registered
        // string intent (e.g. "system-info") — try that before declaring it unrecognised.
        bool stringIntentKnown = false;
        if (!output && parsed.type == CommandType::UNKNOWN && !commandName.empty()) {
            stringIntentKnown = registry.resolve(commandName) != nullptr;
            output = registry.dispatch(commandName, parsed.payload, context);
        }

        if (output) {
            if (!output->empty()) {
                std::cout << *output << std::endl;
            }
        } else if (parsed.type == CommandType::UNKNOWN && !stringIntentKnown) {
            std::cout << runUnknown() << std::endl;
        } else {
            // Resolved command, but disabled — a distinct outcome from "unrecognised" (INV-7).
            std::cout << "Command is currently unavailable." << std::endl;
        }
```
(This moves the `extractCommandName(input)` call up so it's computed once and reused for both
the last-command tracker and the new fallback — same value it always computed, no behavior
change there.)

- [ ] **Step 7: gRPC classifier-intent bridge — `core/jarvis_service.cpp`**

Add `#include <cctype>` to the top of the file's includes.

Add this free function near the top of the file (after the includes, before
`JarvisServiceImpl::JarvisServiceImpl`):

```cpp
namespace {
// Bridges the Understanding tier's SCREAMING_SNAKE intent labels ("SYSTEM_INFO") onto the
// registry's lowercase-kebab intent names ("system-info") — the one general mapping needed so
// a plugin-only capability (no CommandType) is still reachable through natural-language
// classification, without hardcoding each intent name here one at a time.
std::string normalizeClassifierIntent(const std::string& intent) {
  std::string result;
  result.reserve(intent.size());
  for (char c : intent) {
    result += (c == '_') ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return result;
}
}  // namespace
```

Replace the `internalCmd == CommandType::UNKNOWN` block in `ProcessCommand`:

```cpp
  if (intentName.empty() && internalCmd == CommandType::UNKNOWN) {
    spdlog::info("ProcessCommand: unrecognised command, forwarding to AI layer");
    AIResult aiResult = aiClient_.ProcessNaturalLanguage(payload);

    constexpr float kConfidenceThreshold = 0.5f;
    CommandType classifiedCmd = intentToCommandType(aiResult.intent);

    if (aiResult.success && classifiedCmd != CommandType::UNKNOWN &&
        aiResult.confidence >= kConfidenceThreshold) {
      spdlog::info("ProcessCommand: AI classified intent={} confidence={:.2f}, re-dispatching",
          aiResult.intent, aiResult.confidence);
      internalCmd = classifiedCmd;
      dispatchResult = registry_.dispatch(classifiedCmd, payload, execContext);
      if (const Capability* capability = registry_.resolve(classifiedCmd)) {
        intentName = capability->intentName;
      }
      dispatchFailed = !dispatchResult.has_value();
      output = dispatchResult.value_or("Command is currently unavailable.");
    } else {
      output = aiResult.reply;
    }
  }
```

with:

```cpp
  if (intentName.empty() && internalCmd == CommandType::UNKNOWN) {
    spdlog::info("ProcessCommand: unrecognised command, forwarding to AI layer");
    AIResult aiResult = aiClient_.ProcessNaturalLanguage(payload);

    constexpr float kConfidenceThreshold = 0.5f;
    CommandType classifiedCmd = intentToCommandType(aiResult.intent);
    std::string normalizedIntent = normalizeClassifierIntent(aiResult.intent);
    bool stringIntentKnown = classifiedCmd == CommandType::UNKNOWN &&
        registry_.resolve(normalizedIntent) != nullptr;

    if (aiResult.success && aiResult.confidence >= kConfidenceThreshold &&
        (classifiedCmd != CommandType::UNKNOWN || stringIntentKnown)) {
      spdlog::info("ProcessCommand: AI classified intent={} confidence={:.2f}, re-dispatching",
          aiResult.intent, aiResult.confidence);
      if (classifiedCmd != CommandType::UNKNOWN) {
        internalCmd = classifiedCmd;
        dispatchResult = registry_.dispatch(classifiedCmd, payload, execContext);
        if (const Capability* capability = registry_.resolve(classifiedCmd)) {
          intentName = capability->intentName;
        }
      } else {
        intentName = normalizedIntent;
        dispatchResult = registry_.dispatch(normalizedIntent, payload, execContext);
      }
      dispatchFailed = !dispatchResult.has_value();
      output = dispatchResult.value_or("Command is currently unavailable.");
    } else {
      output = aiResult.reply;
    }
  }
```

Remove the `SYSTEM_INFO`-related `case`s from `protoCommandToInternal`, `internalCommandToProto`,
and the `if (intent == "SYSTEM_INFO") return CommandType::SYSTEM_INFO;` line in
`intentToCommandType` (all three switch/if-chains in this file — just delete the one line/case
each that references `SYSTEM_INFO`/`COMMAND_TYPE_SYSTEM_INFO`).

- [ ] **Step 8: Update the three Python clients to send `intent=` for `system-info`**

In `tools/interactive_client.py`, find the `KNOWN_COMMANDS` dict and `build_request`-equivalent
logic (`main()`'s loop building `ExecuteCommandRequest`). Add, right after `KNOWN_COMMANDS`:

```python
# Commands with no CommandType enum value — reachable only through the intent field.
KNOWN_INTENTS = {
    "system-info": "system-info",
}
```

Then find where `KNOWN_COMMANDS` is consulted to build the request (the `if first_word in
KNOWN_COMMANDS:` branch) and add an `elif` right after it:

```python
        if first_word in KNOWN_COMMANDS:
            request = jarvis_pb2.ExecuteCommandRequest(
                command=KNOWN_COMMANDS[first_word], payload=payload
            )
        elif first_word in KNOWN_INTENTS:
            request = jarvis_pb2.ExecuteCommandRequest(
                intent=KNOWN_INTENTS[first_word], payload=payload
            )
        else:
            request = jarvis_pb2.ExecuteCommandRequest(
                command=jarvis_pb2.COMMAND_TYPE_UNKNOWN, payload=stripped
            )
```
(Match this against the file's actual current structure — the exact variable names `stripped`/
`payload`/`first_word` should already exist in the file; wire the new `elif` branch into
whatever the existing if/else chain looks like without altering the two branches either side
of it.)

Apply the same two-dict-plus-branch pattern to `tools/grpc_smoke_test.py` (which calls
`call_command(stub, jarvis_pb2.COMMAND_TYPE_SYSTEM_INFO, "", label="system information")` today
— remove that line, and instead add a call using the `intent` field directly. If
`call_command`'s signature only accepts a `CommandType` enum today, add a second small
helper `call_intent(stub, intent_name, payload, label)` that builds
`jarvis_pb2.ExecuteCommandRequest(intent=intent_name, payload=payload)` and otherwise mirrors
`call_command`'s existing request/print logic — read the file first to match its exact
existing style before adding this.)

Apply the same pattern to `voice/voice_client.py`'s `KNOWN_COMMANDS` dict and
`build_request()` function (same shape as `tools/interactive_client.py`'s, per that file's own
comment noting the two are "deliberately duplicated").

- [ ] **Step 9: Update `tests/capability_registry_test.cpp`**

Replace these existing tests (find them by name — they currently reference
`CommandType::SYSTEM_INFO`, which no longer compiles after Step 3):

```cpp
TEST(SystemInfoCapabilityTest, ReturnsLocalRuntimeInformation) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    Engine engine;
    ExecutionContext context{engine, registry};
    std::optional<std::string> result = registry.dispatch(CommandType::SYSTEM_INFO, "", context);

    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->find("System information:"), std::string::npos);
    EXPECT_NE(result->find("OS:"), std::string::npos);
    EXPECT_NE(result->find("Architecture:"), std::string::npos);
    EXPECT_NE(result->find("Hardware threads:"), std::string::npos);
}

TEST(SystemInfoCapabilityTest, IsPowerTierT0) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    const Capability* systemInfo = registry.resolve(CommandType::SYSTEM_INFO);
    ASSERT_NE(systemInfo, nullptr);
    EXPECT_EQ(systemInfo->powerTier, PowerTier::T0_READ_ONLY);
}

TEST(BuiltinCapabilitiesTest, RegistersExactlyFiveExpectedCapabilities) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    EXPECT_EQ(registry.all().size(), 5u);
    EXPECT_NE(registry.resolve(CommandType::ECHO), nullptr);
    EXPECT_NE(registry.resolve(CommandType::STATUS), nullptr);
    EXPECT_NE(registry.resolve(CommandType::ABOUT), nullptr);
    EXPECT_NE(registry.resolve(CommandType::HELP), nullptr);
    EXPECT_NE(registry.resolve(CommandType::SYSTEM_INFO), nullptr);
    EXPECT_EQ(registry.resolve(CommandType::UNKNOWN), nullptr);
}
```

with:

```cpp
TEST(SystemInfoCapabilityTest, ReturnsLocalRuntimeInformation) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    Engine engine;
    ExecutionContext context{engine, registry};
    std::optional<std::string> result = registry.dispatch(std::string("system-info"), "", context);

    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->find("System information:"), std::string::npos);
    EXPECT_NE(result->find("OS:"), std::string::npos);
    EXPECT_NE(result->find("Architecture:"), std::string::npos);
    EXPECT_NE(result->find("Hardware threads:"), std::string::npos);
}

TEST(SystemInfoCapabilityTest, IsPowerTierT0) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    const Capability* systemInfo = registry.resolve(std::string("system-info"));
    ASSERT_NE(systemInfo, nullptr);
    EXPECT_EQ(systemInfo->powerTier, PowerTier::T0_READ_ONLY);
}

TEST(SystemInfoCapabilityTest, HasNoCommandTypeEntry) {
    // system-info is intent-only (proves the string-dispatch bridge, ahead of becoming a
    // real dynamically-loaded plugin) — it must never appear in the CommandType-keyed map.
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    for (const auto& [commandType, capability] : registry.all()) {
        EXPECT_NE(capability.name, "system-info");
    }
}

TEST(BuiltinCapabilitiesTest, RegistersExactlyFourCommandTypeBackedCapabilities) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    EXPECT_EQ(registry.all().size(), 4u);
    EXPECT_NE(registry.resolve(CommandType::ECHO), nullptr);
    EXPECT_NE(registry.resolve(CommandType::STATUS), nullptr);
    EXPECT_NE(registry.resolve(CommandType::ABOUT), nullptr);
    EXPECT_NE(registry.resolve(CommandType::HELP), nullptr);
    EXPECT_EQ(registry.resolve(CommandType::UNKNOWN), nullptr);
}

TEST(BuiltinCapabilitiesTest, RegistersExactlyFiveCapabilitiesByIntentName) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    EXPECT_EQ(registry.allByIntent().size(), 5u);
    EXPECT_NE(registry.resolve(std::string("echo")), nullptr);
    EXPECT_NE(registry.resolve(std::string("status")), nullptr);
    EXPECT_NE(registry.resolve(std::string("about")), nullptr);
    EXPECT_NE(registry.resolve(std::string("help")), nullptr);
    EXPECT_NE(registry.resolve(std::string("system-info")), nullptr);
}
```

Find `TEST(HelpCapabilityTest, ListsAllFiveBuiltinsWithoutHardcodingThem)` — it's unchanged in
body (it already checks `result->find("system-info")`, and will still pass since Step 5 makes
`help` iterate `allByIntent()`); no edit needed there.

Find `TEST(CapabilityRegistryPluginGateTest, AllFiveBuiltinsDispatchWithPluginConfigSetAndEmptyConfig)`
— replace its last line:
```cpp
    EXPECT_TRUE(registry.dispatch(CommandType::SYSTEM_INFO, "", context).has_value());
```
with:
```cpp
    EXPECT_TRUE(registry.dispatch(std::string("system-info"), "", context).has_value());
```

- [ ] **Step 10: Build and run everything**

```bash
cmake --build build --target jarvis jarvis_grpc_server jarvis_tests
./build/jarvis_tests
PYTHONPATH=ai:generated/python .venv/bin/python -m pytest ai tools voice/tests -q
```
Expected: all three binaries build clean; `jarvis_tests` all pass; Python suites all pass
(the `test_ai_server.py`/`test_intent_classifier.py` `SYSTEM_INFO` cases added in the prior
commit are classifier-label tests — they don't reference the C++ enum and need no change).

- [ ] **Step 11: Manual verification**

```bash
# Terminal 1
.venv/bin/python ai/jarvis_ai_server.py
# Terminal 2
./build/jarvis_grpc_server
# Terminal 3
echo -e "system-info\nexit" | ./build/jarvis          # CLI: typed command still works
python3 tools/interactive_client.py                     # type "system-info", confirm it works
python3 tools/grpc_smoke_test.py                         # confirm system-info + classified path
```
Expected: `system-info` output identical to before this task in every path; `help` still
lists it.

- [ ] **Step 12: Commit**

```bash
git add proto/jarvis.proto core/command_handler.h core/command_handler.cpp \
        core/capability_registry.cpp core/engine.cpp core/jarvis_service.cpp \
        tools/interactive_client.py tools/grpc_smoke_test.py voice/voice_client.py \
        tests/capability_registry_test.cpp
git commit -m "refactor(core): bridge CLI/gRPC onto string-intent dispatch, drop SYSTEM_INFO enum"
```

---

## Task 2: Hand-rolled JSON parser for plugin manifests

**Files:**
- Create: `core/minimal_json.h`
- Create: `core/minimal_json.cpp`
- Modify: `CMakeLists.txt` (add `core/minimal_json.cpp` to `jarvis`, `jarvis_grpc_server`, `jarvis_tests` — plugin manifests are read by the loader these all eventually link, per Task 4)
- Test: `tests/minimal_json_test.cpp`

**Interfaces:**
- Produces: `JsonValue`/`JsonType` (§4 of the spec) and `std::optional<JsonValue> parseJson(const std::string&)`, consumed by `PluginLoader` in Task 4.

- [ ] **Step 1: Write the failing tests**

Create `tests/minimal_json_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include "minimal_json.h"

TEST(MinimalJsonTest, ParsesEmptyObject) {
    auto result = parseJson("{}");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->isObject());
    EXPECT_EQ(result->objectValue.size(), 0u);
}

TEST(MinimalJsonTest, ParsesStringField) {
    auto result = parseJson(R"({"id": "system-info"})");
    ASSERT_TRUE(result.has_value());
    const JsonValue* id = result->find("id");
    ASSERT_NE(id, nullptr);
    EXPECT_EQ(id->asString(), "system-info");
}

TEST(MinimalJsonTest, ParsesNumberField) {
    auto result = parseJson(R"({"abi_version": 1})");
    ASSERT_TRUE(result.has_value());
    const JsonValue* abi = result->find("abi_version");
    ASSERT_NE(abi, nullptr);
    EXPECT_EQ(abi->type, JsonType::Number);
    EXPECT_DOUBLE_EQ(abi->numberValue, 1.0);
}

TEST(MinimalJsonTest, ParsesNestedArrayOfObjects) {
    auto result = parseJson(R"({
        "capabilities": [
            {"intent": "system-info", "description": "desc", "power_tier": "T0_READ_ONLY"}
        ]
    })");
    ASSERT_TRUE(result.has_value());
    const JsonValue* caps = result->find("capabilities");
    ASSERT_NE(caps, nullptr);
    ASSERT_TRUE(caps->isArray());
    ASSERT_EQ(caps->arrayValue.size(), 1u);
    const JsonValue& first = caps->arrayValue[0];
    ASSERT_TRUE(first.isObject());
    EXPECT_EQ(first.find("intent")->asString(), "system-info");
    EXPECT_EQ(first.find("power_tier")->asString(), "T0_READ_ONLY");
}

TEST(MinimalJsonTest, ParsesBooleansAndNull) {
    auto result = parseJson(R"({"a": true, "b": false, "c": null})");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->find("a")->type, JsonType::Boolean);
    EXPECT_TRUE(result->find("a")->boolValue);
    EXPECT_EQ(result->find("b")->type, JsonType::Boolean);
    EXPECT_FALSE(result->find("b")->boolValue);
    EXPECT_EQ(result->find("c")->type, JsonType::Null);
}

TEST(MinimalJsonTest, ParsesEscapedStringCharacters) {
    auto result = parseJson(R"({"text": "line1\nline2\ttabbed\\backslash\"quoted\""})");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->find("text")->asString(), "line1\nline2\ttabbed\\backslash\"quoted\"");
}

TEST(MinimalJsonTest, ParsesNegativeAndFractionalNumbers) {
    auto result = parseJson(R"({"a": -3.5, "b": 42})");
    ASSERT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ(result->find("a")->numberValue, -3.5);
    EXPECT_DOUBLE_EQ(result->find("b")->numberValue, 42.0);
}

TEST(MinimalJsonTest, RejectsMalformedJson) {
    EXPECT_FALSE(parseJson("{").has_value());
    EXPECT_FALSE(parseJson("{\"a\":}").has_value());
    EXPECT_FALSE(parseJson("not json at all").has_value());
    EXPECT_FALSE(parseJson("").has_value());
    EXPECT_FALSE(parseJson(R"({"a": 1)").has_value());
}

TEST(MinimalJsonTest, RejectsTrailingGarbage) {
    EXPECT_FALSE(parseJson("{} garbage").has_value());
}

TEST(MinimalJsonTest, FindReturnsNullptrForMissingOrNonObjectKey) {
    auto result = parseJson(R"({"a": 1})");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->find("missing"), nullptr);

    auto arrayResult = parseJson("[1, 2, 3]");
    ASSERT_TRUE(arrayResult.has_value());
    EXPECT_EQ(arrayResult->find("anything"), nullptr);
}

TEST(MinimalJsonTest, AsStringReturnsFallbackForNonString) {
    auto result = parseJson(R"({"a": 1})");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->find("a")->asString("fallback"), "fallback");
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build --target jarvis_tests`
Expected: build FAILS — `minimal_json.h` does not exist yet.

- [ ] **Step 3: Write `core/minimal_json.h`**

```cpp
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

enum class JsonType { Null, Boolean, Number, String, Array, Object };

// A minimal JSON value — hand-rolled specifically to read plugin manifest.json files against
// the fixed schema in the plugin SDK design spec. Not a general-purpose JSON library: no
// \uXXXX unicode escapes, no streaming, no pretty-printing. See
// docs/superpowers/specs/2026-08-31-plugin-sdk-loader-design.md for why this is hand-rolled
// rather than a third-party dependency (INV-10).
class JsonValue {
 public:
    JsonType type = JsonType::Null;
    bool boolValue = false;
    double numberValue = 0.0;
    std::string stringValue;
    std::vector<JsonValue> arrayValue;
    std::map<std::string, JsonValue> objectValue;

    // Object lookup; nullptr if this isn't an object or the key is absent.
    const JsonValue* find(const std::string& key) const;

    // Returns stringValue if type is String, otherwise fallback — never throws.
    std::string asString(const std::string& fallback = "") const;

    bool isString() const;
    bool isArray() const;
    bool isObject() const;
};

// Parses text as a single JSON document. Returns std::nullopt on any malformed input
// (including trailing garbage after a valid document) — never throws, never crashes on
// untrusted input.
std::optional<JsonValue> parseJson(const std::string& text);
```

- [ ] **Step 4: Write `core/minimal_json.cpp`**

```cpp
#include "minimal_json.h"

#include <cctype>

namespace {

class JsonParser {
 public:
    explicit JsonParser(const std::string& text) : text_(text) {}

    std::optional<JsonValue> parse() {
        skipWhitespace();
        auto value = parseValue();
        if (!value) {
            return std::nullopt;
        }
        skipWhitespace();
        if (pos_ != text_.size()) {
            return std::nullopt;  // trailing garbage after the document
        }
        return value;
    }

 private:
    const std::string& text_;
    std::size_t pos_ = 0;

    void skipWhitespace() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
        }
    }

    bool atEnd() const { return pos_ >= text_.size(); }
    char peek() const { return atEnd() ? '\0' : text_[pos_]; }

    bool consume(char expected) {
        if (peek() != expected) {
            return false;
        }
        ++pos_;
        return true;
    }

    std::optional<JsonValue> parseValue() {
        skipWhitespace();
        if (atEnd()) {
            return std::nullopt;
        }
        const char c = peek();
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') return parseString();
        if (c == 't' || c == 'f') return parseBool();
        if (c == 'n') return parseNull();
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return parseNumber();
        return std::nullopt;
    }

    std::optional<JsonValue> parseObject() {
        if (!consume('{')) {
            return std::nullopt;
        }
        JsonValue value;
        value.type = JsonType::Object;
        skipWhitespace();
        if (consume('}')) {
            return value;
        }
        while (true) {
            skipWhitespace();
            if (peek() != '"') {
                return std::nullopt;
            }
            auto key = parseRawString();
            if (!key) {
                return std::nullopt;
            }
            skipWhitespace();
            if (!consume(':')) {
                return std::nullopt;
            }
            auto val = parseValue();
            if (!val) {
                return std::nullopt;
            }
            value.objectValue[*key] = std::move(*val);
            skipWhitespace();
            if (consume(',')) {
                continue;
            }
            if (consume('}')) {
                break;
            }
            return std::nullopt;
        }
        return value;
    }

    std::optional<JsonValue> parseArray() {
        if (!consume('[')) {
            return std::nullopt;
        }
        JsonValue value;
        value.type = JsonType::Array;
        skipWhitespace();
        if (consume(']')) {
            return value;
        }
        while (true) {
            auto val = parseValue();
            if (!val) {
                return std::nullopt;
            }
            value.arrayValue.push_back(std::move(*val));
            skipWhitespace();
            if (consume(',')) {
                continue;
            }
            if (consume(']')) {
                break;
            }
            return std::nullopt;
        }
        return value;
    }

    std::optional<std::string> parseRawString() {
        if (!consume('"')) {
            return std::nullopt;
        }
        std::string result;
        while (true) {
            if (atEnd()) {
                return std::nullopt;
            }
            const char c = text_[pos_++];
            if (c == '"') {
                break;
            }
            if (c == '\\') {
                if (atEnd()) {
                    return std::nullopt;
                }
                const char esc = text_[pos_++];
                switch (esc) {
                    case '"':  result += '"';  break;
                    case '\\': result += '\\'; break;
                    case '/':  result += '/';  break;
                    case 'n':  result += '\n'; break;
                    case 't':  result += '\t'; break;
                    case 'r':  result += '\r'; break;
                    case 'b':  result += '\b'; break;
                    case 'f':  result += '\f'; break;
                    default:   return std::nullopt;  // includes unsupported \uXXXX
                }
            } else {
                result += c;
            }
        }
        return result;
    }

    std::optional<JsonValue> parseString() {
        auto raw = parseRawString();
        if (!raw) {
            return std::nullopt;
        }
        JsonValue value;
        value.type = JsonType::String;
        value.stringValue = *raw;
        return value;
    }

    std::optional<JsonValue> parseBool() {
        if (text_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            JsonValue value;
            value.type = JsonType::Boolean;
            value.boolValue = true;
            return value;
        }
        if (text_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            JsonValue value;
            value.type = JsonType::Boolean;
            value.boolValue = false;
            return value;
        }
        return std::nullopt;
    }

    std::optional<JsonValue> parseNull() {
        if (text_.compare(pos_, 4, "null") == 0) {
            pos_ += 4;
            return JsonValue{};
        }
        return std::nullopt;
    }

    std::optional<JsonValue> parseNumber() {
        const std::size_t start = pos_;
        if (peek() == '-') {
            ++pos_;
        }
        if (atEnd() || !std::isdigit(static_cast<unsigned char>(peek()))) {
            return std::nullopt;
        }
        while (!atEnd() && std::isdigit(static_cast<unsigned char>(peek()))) {
            ++pos_;
        }
        if (!atEnd() && peek() == '.') {
            ++pos_;
            if (atEnd() || !std::isdigit(static_cast<unsigned char>(peek()))) {
                return std::nullopt;
            }
            while (!atEnd() && std::isdigit(static_cast<unsigned char>(peek()))) {
                ++pos_;
            }
        }
        if (!atEnd() && (peek() == 'e' || peek() == 'E')) {
            ++pos_;
            if (!atEnd() && (peek() == '+' || peek() == '-')) {
                ++pos_;
            }
            if (atEnd() || !std::isdigit(static_cast<unsigned char>(peek()))) {
                return std::nullopt;
            }
            while (!atEnd() && std::isdigit(static_cast<unsigned char>(peek()))) {
                ++pos_;
            }
        }
        const std::string numText = text_.substr(start, pos_ - start);
        JsonValue value;
        value.type = JsonType::Number;
        try {
            value.numberValue = std::stod(numText);
        } catch (...) {
            return std::nullopt;
        }
        return value;
    }
};

}  // namespace

std::optional<JsonValue> parseJson(const std::string& text) {
    JsonParser parser(text);
    return parser.parse();
}

const JsonValue* JsonValue::find(const std::string& key) const {
    if (type != JsonType::Object) {
        return nullptr;
    }
    auto it = objectValue.find(key);
    if (it == objectValue.end()) {
        return nullptr;
    }
    return &it->second;
}

std::string JsonValue::asString(const std::string& fallback) const {
    if (type != JsonType::String) {
        return fallback;
    }
    return stringValue;
}

bool JsonValue::isString() const { return type == JsonType::String; }
bool JsonValue::isArray() const { return type == JsonType::Array; }
bool JsonValue::isObject() const { return type == JsonType::Object; }
```

- [ ] **Step 5: Wire into CMakeLists.txt**

Add `core/minimal_json.cpp` to the `jarvis`, `jarvis_grpc_server`, and `jarvis_tests`
executables' source lists (these all eventually need it for `PluginLoader` in Task 4 — wiring
it now keeps each task's CMake diff scoped to what that task actually adds). Add
`tests/minimal_json_test.cpp` and `core/minimal_json.cpp` to `jarvis_tests`'s sources.

- [ ] **Step 6: Run tests to verify they pass**

Run: `cmake --build build --target jarvis_tests && ./build/jarvis_tests --gtest_filter=MinimalJsonTest.*`
Expected: PASS (all 10 new tests)

- [ ] **Step 7: Commit**

```bash
git add core/minimal_json.h core/minimal_json.cpp tests/minimal_json_test.cpp CMakeLists.txt
git commit -m "feat(core): add hand-rolled JSON parser for plugin manifests"
```

---

## Task 3: Plugin ABI header

**Files:**
- Create: `plugin_sdk/jarvis_plugin_abi.h`

**Interfaces:**
- Produces: `JARVIS_PLUGIN_ABI_VERSION`, `JarvisPowerTier`, `JarvisCapabilityFn`,
  `JarvisPluginHost`, consumed by `PluginLoader` (Task 4) and every plugin `.cpp` (Tasks 5-6).

- [ ] **Step 1: Write `plugin_sdk/jarvis_plugin_abi.h`**

```c
#ifndef JARVIS_PLUGIN_ABI_H
#define JARVIS_PLUGIN_ABI_H

/* The JARVIS plugin ABI — pure C, no C++ types cross this boundary. This is the ONLY header a
 * plugin author includes. See docs/superpowers/specs/2026-08-31-plugin-sdk-loader-design.md
 * §3 for the full rationale (avoiding STL-ABI/exception/RTTI mismatches across a dlopen
 * boundary between independently-built binaries). */

#ifdef __cplusplus
extern "C" {
#endif

#define JARVIS_PLUGIN_ABI_VERSION 1

typedef enum {
    JARVIS_POWER_TIER_T0_READ_ONLY = 0,
    JARVIS_POWER_TIER_T1_STATEFUL_LOCAL = 1,
    JARVIS_POWER_TIER_T2_SYSTEM_AFFECTING = 2,
    JARVIS_POWER_TIER_T3_DESTRUCTIVE = 3,
    JARVIS_POWER_TIER_T4_EXTERNAL = 4
} JarvisPowerTier;

/* payload is a NUL-terminated UTF-8 string owned by the host, read-only, valid only for the
 * duration of the call. Returns a NUL-terminated UTF-8 string the plugin allocated with
 * malloc(); the host copies it and then free()s the original — malloc/free is the one
 * allocator both sides can agree on across a dlopen boundary without a shared allocator
 * library, unlike `new`/`delete` or std::string. A null return is treated as an empty
 * string by the host. */
typedef char* (*JarvisCapabilityFn)(const char* payload);

typedef struct {
    /* Called by the plugin's jarvis_plugin_register(), once per capability, during load.
     * Returns 1 on success, 0 if the host rejected it. host_context is the opaque pointer the
     * host passed into jarvis_plugin_register() — pass it back unchanged. */
    int (*registerCapability)(
        void* host_context,
        const char* intent_name,
        const char* description,
        JarvisPowerTier power_tier,
        JarvisCapabilityFn execute);
} JarvisPluginHost;

/* Every plugin exports both of these symbols with these exact names.
 *
 * int jarvis_plugin_abi_version(void);
 *     Returns JARVIS_PLUGIN_ABI_VERSION the plugin was BUILT against (i.e. return the literal
 *     macro value, so this always tracks whatever header the plugin compiled with). The
 *     loader refuses to call jarvis_plugin_register() at all on a mismatch.
 *
 * int jarvis_plugin_register(void* host_context, const JarvisPluginHost* host);
 *     Called once at load time. Call host->registerCapability(...) once per capability this
 *     plugin provides. Returns 1 if registration succeeded, 0 on failure (the loader then
 *     refuses to activate this plugin).
 */

#ifdef __cplusplus
}
#endif

#endif /* JARVIS_PLUGIN_ABI_H */
```

- [ ] **Step 2: Commit**

```bash
git add plugin_sdk/jarvis_plugin_abi.h
git commit -m "feat(plugin-sdk): add the versioned C plugin ABI header"
```

(No test for this task — it's a header with no logic; Tasks 4-6 exercise it directly through
real compiled plugins and the loader that consumes it.)

---

## Task 4: `PluginLoader`

**Files:**
- Modify: `core/capability_registry.h`, `core/capability_registry.cpp` (add `unregisterCapability`)
- Create: `core/plugin_loader.h`
- Create: `core/plugin_loader.cpp`
- Create: `config/plugin_dirs.cfg`
- Modify: `CMakeLists.txt` (add `core/plugin_loader.cpp` to `jarvis`, `jarvis_grpc_server`, `jarvis_tests`; link `${CMAKE_DL_LIBS}` into all three; add `plugin_sdk/` to each target's include directories; add `tests/plugin_loader_test.cpp` to `jarvis_tests`)
- Test: `tests/plugin_loader_test.cpp` (this task's own tests use hand-written fake plugin
  handles where possible; Task 5 adds the *real* `.so` fixtures this task's tests will be
  extended to use — see Task 5's own test additions)

**Interfaces:**
- Consumes: `JsonValue`/`parseJson` (Task 2), `plugin_sdk/jarvis_plugin_abi.h` (Task 3),
  `CapabilityRegistry::resolve(const std::string&)`/`registerCapability`/`unregisterCapability`.
- Produces: `PluginLoadResult`, `class PluginLoader` with `loadFromDirectory`,
  `disablePlugin`, `unloadPlugin`, `loadedPluginIds`; free function
  `std::vector<std::string> loadPluginDirs(const std::string& path)`. Consumed by Task 6's
  `main.cpp`/`grpc_server_main.cpp` wiring.

- [ ] **Step 1: Add `CapabilityRegistry::unregisterCapability`**

In `core/capability_registry.h`, add this public method (after `registerCapability`, before the
`resolve(const std::string&)` overload):

```cpp
    // Removes a capability from the string-intent dispatch path only (loaded plugins never
    // have a CommandType, so this has no effect on the enum-keyed path). Returns true if a
    // capability with this intent name was actually removed. Used by PluginLoader when
    // disabling a plugin — a separate, coarser, in-memory-only mechanism from PluginConfig's
    // persistent, config-file-driven enable/disable, which still applies independently.
    bool unregisterCapability(const std::string& intentName);
```

In `core/capability_registry.cpp`, add the implementation (after `registerCapability`):

```cpp
bool CapabilityRegistry::unregisterCapability(const std::string& intentName) {
    return namedCapabilities_.erase(intentName) > 0;
}
```

- [ ] **Step 2: Write the failing tests (hand-written fakes, no real `.so` yet)**

Create `tests/plugin_loader_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include <fstream>

#include "capability_registry.h"
#include "engine.h"
#include "plugin_loader.h"

TEST(LoadPluginDirsTest, MissingFileReturnsEmptyList) {
    std::vector<std::string> dirs = loadPluginDirs("does_not_exist_plugin_dirs.cfg");
    EXPECT_TRUE(dirs.empty());
}

TEST(LoadPluginDirsTest, ParsesOneDirectoryPerLineSkippingCommentsAndBlanks) {
    {
        std::ofstream file("test_plugin_dirs.cfg");
        file << "# a comment\n";
        file << "\n";
        file << "build/plugins/\n";
        file << "another/dir\n";
    }
    std::vector<std::string> dirs = loadPluginDirs("test_plugin_dirs.cfg");
    std::remove("test_plugin_dirs.cfg");

    ASSERT_EQ(dirs.size(), 2u);
    EXPECT_EQ(dirs[0], "build/plugins/");
    EXPECT_EQ(dirs[1], "another/dir");
}

TEST(PluginLoaderTest, LoadFromMissingDirectoryReturnsEmptyResultList) {
    CapabilityRegistry registry;
    PluginLoader loader;

    std::vector<PluginLoadResult> results = loader.loadFromDirectory("does_not_exist_dir", registry);

    EXPECT_TRUE(results.empty());
    EXPECT_TRUE(loader.loadedPluginIds().empty());
}

TEST(PluginLoaderTest, SkipsSubdirectoryWithNoManifest) {
    std::filesystem::create_directories("test_plugins_no_manifest/not-a-plugin");
    CapabilityRegistry registry;
    PluginLoader loader;

    std::vector<PluginLoadResult> results = loader.loadFromDirectory("test_plugins_no_manifest", registry);

    std::filesystem::remove_all("test_plugins_no_manifest");
    EXPECT_TRUE(results.empty());
}

TEST(PluginLoaderTest, RejectsMalformedManifestJson) {
    std::filesystem::create_directories("test_plugins_bad_json/broken");
    {
        std::ofstream file("test_plugins_bad_json/broken/manifest.json");
        file << "{ this is not valid json";
    }
    CapabilityRegistry registry;
    PluginLoader loader;

    std::vector<PluginLoadResult> results = loader.loadFromDirectory("test_plugins_bad_json", registry);

    std::filesystem::remove_all("test_plugins_bad_json");
    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results[0].loaded);
    EXPECT_TRUE(registry.allByIntent().empty());
}

TEST(PluginLoaderTest, RejectsManifestMissingRequiredField) {
    std::filesystem::create_directories("test_plugins_missing_field/incomplete");
    {
        std::ofstream file("test_plugins_missing_field/incomplete/manifest.json");
        file << R"({"id": "incomplete", "version": "1.0.0"})";
    }
    CapabilityRegistry registry;
    PluginLoader loader;

    std::vector<PluginLoadResult> results = loader.loadFromDirectory("test_plugins_missing_field", registry);

    std::filesystem::remove_all("test_plugins_missing_field");
    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results[0].loaded);
}

TEST(PluginLoaderTest, RejectsAbiVersionMismatchBeforeDlopen) {
    std::filesystem::create_directories("test_plugins_bad_abi/mismatched");
    {
        std::ofstream file("test_plugins_bad_abi/mismatched/manifest.json");
        file << R"({
            "id": "mismatched",
            "version": "1.0.0",
            "abi_version": 999,
            "library": "does_not_exist.so",
            "capabilities": []
        })";
    }
    CapabilityRegistry registry;
    PluginLoader loader;

    std::vector<PluginLoadResult> results = loader.loadFromDirectory("test_plugins_bad_abi", registry);

    std::filesystem::remove_all("test_plugins_bad_abi");
    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results[0].loaded);
    // Proves rejection happened before dlopen was ever attempted: the library path in the
    // manifest doesn't exist, so a dlopen attempt would also fail, but for a DIFFERENT
    // reason — the loader must never get that far for an ABI mismatch.
    EXPECT_NE(results[0].reason.find("ABI"), std::string::npos);
}

TEST(PluginLoaderTest, RejectsDuplicateIntentAgainstExistingBuiltin) {
    std::filesystem::create_directories("test_plugins_dup_intent/dup");
    {
        std::ofstream file("test_plugins_dup_intent/dup/manifest.json");
        file << R"({
            "id": "dup",
            "version": "1.0.0",
            "abi_version": 1,
            "library": "does_not_exist.so",
            "capabilities": [
                {"intent": "echo", "description": "d", "power_tier": "T0_READ_ONLY"}
            ]
        })";
    }
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);  // registers "echo" already
    PluginLoader loader;

    std::vector<PluginLoadResult> results = loader.loadFromDirectory("test_plugins_dup_intent", registry);

    std::filesystem::remove_all("test_plugins_dup_intent");
    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results[0].loaded);
    EXPECT_NE(results[0].reason.find("already registered"), std::string::npos);
}

TEST(PluginLoaderTest, DisableAndUnloadOnUnknownPluginIdReturnFalse) {
    CapabilityRegistry registry;
    PluginLoader loader;

    EXPECT_FALSE(loader.disablePlugin("nonexistent", registry));
    EXPECT_FALSE(loader.unloadPlugin("nonexistent"));
}
```

(This task's tests exercise every *rejection* path without needing a real `.so` — every case
above fails validation before `dlopen` would ever run. The *acceptance* path — a plugin that
actually loads, dispatches, disables, and unloads — is proven in Task 5/6 against real
fixture/reference plugins, since that needs an actual compiled shared library to be
meaningful.)

- [ ] **Step 3: Run tests to verify they fail**

Run: `cmake --build build --target jarvis_tests`
Expected: build FAILS — `plugin_loader.h` does not exist yet.

- [ ] **Step 4: Write `core/plugin_loader.h`**

```cpp
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class CapabilityRegistry;

struct PluginLoadResult {
    std::string pluginId;
    bool loaded;
    std::string reason;  // empty if loaded; human-readable rejection reason otherwise
};

// Discovers, validates, and dlopen()s plugins from configured trusted local directories. See
// docs/superpowers/specs/2026-08-31-plugin-sdk-loader-design.md for the full manifest
// validation order and safe-unload design.
class PluginLoader {
 public:
    ~PluginLoader();

    // Scans pluginDir for one subdirectory per plugin, validates and loads each, registering
    // its capabilities into registry. Returns one result per subdirectory containing a
    // manifest.json (loaded or rejected). A missing pluginDir yields an empty list, not an
    // error.
    std::vector<PluginLoadResult> loadFromDirectory(const std::string& pluginDir,
                                                      CapabilityRegistry& registry);

    // Removes this plugin's capabilities from string-intent dispatch (registry.dispatch(intent, ...)
    // will no longer resolve them) but keeps its library mapped. Returns false if pluginId is
    // unknown or already disabled.
    bool disablePlugin(const std::string& pluginId, CapabilityRegistry& registry);

    // dlclose()s a plugin's library. Returns false if the plugin is unknown, not yet
    // disabled, or has an invocation currently in flight.
    bool unloadPlugin(const std::string& pluginId);

    const std::vector<std::string>& loadedPluginIds() const;

 private:
    struct LoadedPlugin {
        void* handle = nullptr;
        std::vector<std::string> intents;
        std::shared_ptr<std::atomic<int>> invocationCount;
        bool disabled = false;
    };

    std::unordered_map<std::string, LoadedPlugin> plugins_;
    std::vector<std::string> loadedPluginIds_;
};

// Reads a flat, one-directory-per-line file (see config/plugin_dirs.cfg) — "#" comments and
// blank lines skipped. A missing file yields an empty list, not an error, matching
// PluginConfig's own missing-file discipline.
std::vector<std::string> loadPluginDirs(const std::string& path);
```

- [ ] **Step 5: Write `core/plugin_loader.cpp`**

```cpp
#include "plugin_loader.h"

#include <dlfcn.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <spdlog/spdlog.h>

#include "capability.h"
#include "capability_registry.h"
#include "minimal_json.h"
#include "jarvis_plugin_abi.h"

namespace {

PowerTier toPowerTier(JarvisPowerTier tier) {
    switch (tier) {
        case JARVIS_POWER_TIER_T0_READ_ONLY: return PowerTier::T0_READ_ONLY;
        case JARVIS_POWER_TIER_T1_STATEFUL_LOCAL: return PowerTier::T1_STATEFUL_LOCAL;
        case JARVIS_POWER_TIER_T2_SYSTEM_AFFECTING: return PowerTier::T2_SYSTEM_AFFECTING;
        case JARVIS_POWER_TIER_T3_DESTRUCTIVE: return PowerTier::T3_DESTRUCTIVE;
        case JARVIS_POWER_TIER_T4_EXTERNAL: return PowerTier::T4_EXTERNAL;
    }
    return PowerTier::T4_EXTERNAL;  // unreachable; most-restrictive fallback if it ever is
}

std::optional<PowerTier> parsePowerTierName(const std::string& name) {
    if (name == "T0_READ_ONLY") return PowerTier::T0_READ_ONLY;
    if (name == "T1_STATEFUL_LOCAL") return PowerTier::T1_STATEFUL_LOCAL;
    if (name == "T2_SYSTEM_AFFECTING") return PowerTier::T2_SYSTEM_AFFECTING;
    if (name == "T3_DESTRUCTIVE") return PowerTier::T3_DESTRUCTIVE;
    if (name == "T4_EXTERNAL") return PowerTier::T4_EXTERNAL;
    return std::nullopt;
}

std::string trim(const std::string& text) {
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

struct DeclaredCapability {
    std::string intent;
    PowerTier tier;
};

struct HostContext {
    std::vector<Capability>* staged;
    std::shared_ptr<std::atomic<int>> invocationCounter;
};

int hostRegisterCapability(void* ctx, const char* intentName, const char* description,
                            JarvisPowerTier tier, JarvisCapabilityFn fn) {
    auto* hostContext = static_cast<HostContext*>(ctx);
    auto counter = hostContext->invocationCounter;

    Capability capability;
    capability.name = intentName;
    capability.intent = CommandType::UNKNOWN;
    capability.description = description;
    capability.powerTier = toPowerTier(tier);
    capability.intentName = intentName;
    capability.execute = [fn, counter](const std::string& payload, ExecutionContext&) -> std::string {
        counter->fetch_add(1, std::memory_order_relaxed);
        struct Guard {
            std::atomic<int>* counter;
            ~Guard() { counter->fetch_sub(1, std::memory_order_relaxed); }
        } guard{counter.get()};

        char* raw = fn(payload.c_str());
        std::string result = raw ? raw : "";
        if (raw) {
            std::free(raw);
        }
        return result;
    };

    hostContext->staged->push_back(std::move(capability));
    return 1;
}

}  // namespace

std::vector<std::string> loadPluginDirs(const std::string& path) {
    std::vector<std::string> dirs;
    std::ifstream file(path);
    if (!file.is_open()) {
        return dirs;
    }

    std::string line;
    while (std::getline(file, line)) {
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }
        dirs.push_back(trimmed);
    }
    return dirs;
}

std::vector<PluginLoadResult> PluginLoader::loadFromDirectory(const std::string& pluginDir,
                                                                CapabilityRegistry& registry) {
    std::vector<PluginLoadResult> results;

    std::error_code ec;
    if (!std::filesystem::is_directory(pluginDir, ec)) {
        return results;
    }

    for (const auto& entry : std::filesystem::directory_iterator(pluginDir, ec)) {
        if (!entry.is_directory()) {
            continue;
        }

        const std::string pluginPath = entry.path().string();
        const std::string manifestPath = pluginPath + "/manifest.json";
        const std::string pluginId = entry.path().filename().string();

        if (!std::filesystem::exists(manifestPath)) {
            continue;  // not every subdirectory is necessarily a plugin — silent skip
        }

        std::ifstream manifestFile(manifestPath);
        std::stringstream buffer;
        buffer << manifestFile.rdbuf();

        auto parsed = parseJson(buffer.str());
        if (!parsed || !parsed->isObject()) {
            spdlog::warn("PluginLoader: malformed manifest JSON at {}", manifestPath);
            results.push_back({pluginId, false, "malformed manifest JSON"});
            continue;
        }

        const JsonValue& manifest = *parsed;
        const JsonValue* idField = manifest.find("id");
        const JsonValue* versionField = manifest.find("version");
        const JsonValue* abiField = manifest.find("abi_version");
        const JsonValue* libraryField = manifest.find("library");
        const JsonValue* capsField = manifest.find("capabilities");

        if (!idField || !idField->isString() || !versionField || !versionField->isString() ||
            !abiField || abiField->type != JsonType::Number ||
            !libraryField || !libraryField->isString() ||
            !capsField || !capsField->isArray()) {
            spdlog::warn("PluginLoader: manifest at {} is missing a required field", manifestPath);
            results.push_back({pluginId, false, "missing required manifest field"});
            continue;
        }

        const std::string manifestId = idField->asString();
        const int declaredAbiVersion = static_cast<int>(abiField->numberValue);
        const std::string libraryFile = libraryField->asString();

        if (declaredAbiVersion != JARVIS_PLUGIN_ABI_VERSION) {
            spdlog::warn("PluginLoader: {} declares abi_version {} but host is {}",
                manifestPath, declaredAbiVersion, JARVIS_PLUGIN_ABI_VERSION);
            results.push_back({manifestId, false, "ABI version mismatch"});
            continue;
        }

        std::vector<DeclaredCapability> declared;
        bool manifestValid = true;
        std::string invalidReason;

        for (const JsonValue& capValue : capsField->arrayValue) {
            const JsonValue* intentField = capValue.find("intent");
            const JsonValue* descField = capValue.find("description");
            const JsonValue* tierField = capValue.find("power_tier");
            if (!intentField || !intentField->isString() || !descField || !descField->isString() ||
                !tierField || !tierField->isString()) {
                manifestValid = false;
                invalidReason = "malformed capability entry";
                break;
            }

            std::optional<PowerTier> tier = parsePowerTierName(tierField->asString());
            if (!tier) {
                manifestValid = false;
                invalidReason = "unknown power_tier '" + tierField->asString() + "'";
                break;
            }

            if (registry.resolve(intentField->asString()) != nullptr) {
                manifestValid = false;
                invalidReason = "intent '" + intentField->asString() + "' already registered";
                break;
            }

            declared.push_back({intentField->asString(), *tier});
        }

        if (!manifestValid) {
            spdlog::warn("PluginLoader: {} rejected: {}", manifestPath, invalidReason);
            results.push_back({manifestId, false, invalidReason});
            continue;
        }

        const std::string libraryPath = pluginPath + "/" + libraryFile;
        void* handle = dlopen(libraryPath.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            const std::string reason = std::string("dlopen failed: ") + dlerror();
            spdlog::warn("PluginLoader: {}: {}", manifestPath, reason);
            results.push_back({manifestId, false, reason});
            continue;
        }

        using AbiVersionFn = int (*)();
        using RegisterFn = int (*)(void*, const JarvisPluginHost*);

        auto abiVersionFn = reinterpret_cast<AbiVersionFn>(dlsym(handle, "jarvis_plugin_abi_version"));
        auto registerFn = reinterpret_cast<RegisterFn>(dlsym(handle, "jarvis_plugin_register"));

        if (!abiVersionFn || !registerFn) {
            spdlog::warn("PluginLoader: {} is missing a required ABI symbol", libraryPath);
            dlclose(handle);
            results.push_back({manifestId, false, "missing ABI symbol"});
            continue;
        }

        if (abiVersionFn() != JARVIS_PLUGIN_ABI_VERSION) {
            spdlog::warn("PluginLoader: {} reports a different ABI version at runtime than declared", libraryPath);
            dlclose(handle);
            results.push_back({manifestId, false, "runtime ABI version mismatch"});
            continue;
        }

        std::vector<Capability> staged;
        auto invocationCounter = std::make_shared<std::atomic<int>>(0);
        HostContext hostContext{&staged, invocationCounter};

        JarvisPluginHost host;
        host.registerCapability = &hostRegisterCapability;

        if (!registerFn(&hostContext, &host)) {
            spdlog::warn("PluginLoader: {}'s jarvis_plugin_register() returned failure", libraryPath);
            dlclose(handle);
            results.push_back({manifestId, false, "plugin registration failed"});
            continue;
        }

        bool crossCheckOk = staged.size() == declared.size();
        if (crossCheckOk) {
            for (const Capability& capability : staged) {
                const bool found = std::any_of(declared.begin(), declared.end(),
                    [&capability](const DeclaredCapability& d) {
                        return d.intent == capability.intentName && d.tier == capability.powerTier;
                    });
                if (!found) {
                    crossCheckOk = false;
                    break;
                }
            }
        }

        if (!crossCheckOk) {
            spdlog::warn("PluginLoader: {}: registered capabilities do not match its manifest", libraryPath);
            dlclose(handle);
            results.push_back({manifestId, false, "registered capabilities do not match manifest"});
            continue;
        }

        std::vector<std::string> intents;
        for (Capability& capability : staged) {
            intents.push_back(capability.intentName);
            registry.registerCapability(std::move(capability));
        }

        LoadedPlugin loaded;
        loaded.handle = handle;
        loaded.intents = std::move(intents);
        loaded.invocationCount = invocationCounter;
        loaded.disabled = false;

        plugins_[manifestId] = std::move(loaded);
        loadedPluginIds_.push_back(manifestId);
        results.push_back({manifestId, true, ""});
        spdlog::info("PluginLoader: loaded plugin '{}' from {}", manifestId, pluginPath);
    }

    return results;
}

bool PluginLoader::disablePlugin(const std::string& pluginId, CapabilityRegistry& registry) {
    auto it = plugins_.find(pluginId);
    if (it == plugins_.end() || it->second.disabled) {
        return false;
    }
    for (const std::string& intent : it->second.intents) {
        registry.unregisterCapability(intent);
    }
    it->second.disabled = true;
    return true;
}

bool PluginLoader::unloadPlugin(const std::string& pluginId) {
    auto it = plugins_.find(pluginId);
    if (it == plugins_.end() || !it->second.disabled) {
        return false;
    }
    if (it->second.invocationCount->load(std::memory_order_relaxed) != 0) {
        return false;
    }

    dlclose(it->second.handle);
    plugins_.erase(it);

    auto idIt = std::find(loadedPluginIds_.begin(), loadedPluginIds_.end(), pluginId);
    if (idIt != loadedPluginIds_.end()) {
        loadedPluginIds_.erase(idIt);
    }
    return true;
}

const std::vector<std::string>& PluginLoader::loadedPluginIds() const {
    return loadedPluginIds_;
}

PluginLoader::~PluginLoader() {
    // By the time this destructor runs, the CapabilityRegistry that held these plugins'
    // Capability::execute closures must already be destroyed — main()/grpc_server_main()
    // (Task 6) declare `PluginLoader loader;` BEFORE `CapabilityRegistry registry;`, so C++'s
    // reverse-construction-order destruction runs registry's destructor first, then this
    // one. dlclose()-ing here is then safe: nothing still references these libraries.
    for (auto& [id, plugin] : plugins_) {
        if (plugin.handle) {
            dlclose(plugin.handle);
        }
    }
}
```

- [ ] **Step 6: Create the default `config/plugin_dirs.cfg`**

```
# Directories PluginLoader scans at startup, one per line. Relative paths are resolved
# against the process's working directory (matches how config/capabilities.cfg is already
# looked up). A missing or empty file means no plugin directories are scanned.
build/plugins/
```

- [ ] **Step 7: Wire into CMakeLists.txt**

- Add `core/plugin_loader.cpp` to the `jarvis`, `jarvis_grpc_server`, and `jarvis_tests`
  targets' source lists.
- Add `${CMAKE_DL_LIBS}` to the `target_link_libraries` of `jarvis`, `jarvis_grpc_server`, and
  `jarvis_tests` (portable `dlopen`/`dlsym`/`dlclose` linkage on Linux).
- Add `${CMAKE_SOURCE_DIR}/plugin_sdk` to the `target_include_directories` of `jarvis`,
  `jarvis_grpc_server`, and `jarvis_tests`.
- Add `tests/plugin_loader_test.cpp` to `jarvis_tests`'s sources.

- [ ] **Step 8: Run tests to verify they pass**

Run: `cmake --build build --target jarvis_tests && ./build/jarvis_tests --gtest_filter=PluginLoaderTest.*:LoadPluginDirsTest.*`
Expected: PASS (all new tests from Step 2)

- [ ] **Step 9: Run the full suite once more**

Run: `./build/jarvis_tests`
Expected: PASS — every test in the project, including Tasks 1-3's, unchanged.

- [ ] **Step 10: Commit**

```bash
git add core/capability_registry.h core/capability_registry.cpp core/plugin_loader.h \
        core/plugin_loader.cpp config/plugin_dirs.cfg tests/plugin_loader_test.cpp CMakeLists.txt
git commit -m "feat(core): add PluginLoader — manifest-validated dlopen plugin discovery"
```

---

## Task 5: Fixture plugins for `PluginLoader`'s acceptance-path tests

**Files:**
- Create: `tests/fixtures/plugins/valid-echo/plugin.cpp`, `tests/fixtures/plugins/valid-echo/manifest.json`
- Create: `tests/fixtures/plugins/bad-abi/plugin.cpp`, `tests/fixtures/plugins/bad-abi/manifest.json`
- Create: `tests/fixtures/plugins/mismatched-tier/plugin.cpp`, `tests/fixtures/plugins/mismatched-tier/manifest.json`
- Modify: `CMakeLists.txt` (three new `SHARED` library targets, built into a test-fixtures output directory, plus manifest-copy steps)
- Modify: `tests/plugin_loader_test.cpp` (append acceptance-path and runtime-rejection tests using these real `.so`s)

**Interfaces:**
- Consumes: `plugin_sdk/jarvis_plugin_abi.h` (Task 3).
- Produces: nothing consumed by later tasks — purely test infrastructure. Task 6's real
  `system-info` plugin follows the same `plugin.cpp`/`manifest.json`/CMake-target pattern
  established here.

- [ ] **Step 1: `valid-echo` — a real, correctly-behaving fixture plugin**

Create `tests/fixtures/plugins/valid-echo/plugin.cpp`:

```cpp
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
```

Create `tests/fixtures/plugins/valid-echo/manifest.json`:

```json
{
  "id": "valid-echo",
  "version": "1.0.0",
  "abi_version": 1,
  "library": "libvalid_echo_fixture.so",
  "capabilities": [
    {
      "intent": "fixture-echo",
      "description": "Test fixture: echoes payload back.",
      "power_tier": "T0_READ_ONLY"
    }
  ]
}
```

- [ ] **Step 2: `bad-abi` — declares a mismatched runtime ABI version**

Create `tests/fixtures/plugins/bad-abi/plugin.cpp`:

```cpp
#include "jarvis_plugin_abi.h"

extern "C" int jarvis_plugin_abi_version() {
    return 999;  // deliberately wrong — must be rejected before jarvis_plugin_register() runs
}

extern "C" int jarvis_plugin_register(void* /*host_context*/, const JarvisPluginHost* /*host*/) {
    return 1;  // never actually called if the loader is correct
}
```

Create `tests/fixtures/plugins/bad-abi/manifest.json` (its OWN declared `abi_version` is
correct — the point of this fixture is that the *runtime* `jarvis_plugin_abi_version()` call
disagrees with what was declared, which only surfaces after `dlopen`):

```json
{
  "id": "bad-abi",
  "version": "1.0.0",
  "abi_version": 1,
  "library": "libbad_abi_fixture.so",
  "capabilities": []
}
```

- [ ] **Step 3: `mismatched-tier` — registers a different tier than its manifest declares**

Create `tests/fixtures/plugins/mismatched-tier/plugin.cpp`:

```cpp
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
```

Create `tests/fixtures/plugins/mismatched-tier/manifest.json`:

```json
{
  "id": "mismatched-tier",
  "version": "1.0.0",
  "abi_version": 1,
  "library": "libmismatched_tier_fixture.so",
  "capabilities": [
    {
      "intent": "fixture-mismatched",
      "description": "Test fixture: tier mismatch.",
      "power_tier": "T0_READ_ONLY"
    }
  ]
}
```

- [ ] **Step 4: Wire the three fixture targets into CMakeLists.txt**

Add, near the `jarvis_tests` target definition:

```cmake
add_library(valid_echo_fixture SHARED tests/fixtures/plugins/valid-echo/plugin.cpp)
target_include_directories(valid_echo_fixture PRIVATE ${CMAKE_SOURCE_DIR}/plugin_sdk)
set_target_properties(valid_echo_fixture PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/fixtures/plugins/valid-echo)
add_custom_command(TARGET valid_echo_fixture POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_SOURCE_DIR}/tests/fixtures/plugins/valid-echo/manifest.json
        ${CMAKE_BINARY_DIR}/tests/fixtures/plugins/valid-echo/manifest.json)

add_library(bad_abi_fixture SHARED tests/fixtures/plugins/bad-abi/plugin.cpp)
target_include_directories(bad_abi_fixture PRIVATE ${CMAKE_SOURCE_DIR}/plugin_sdk)
set_target_properties(bad_abi_fixture PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/fixtures/plugins/bad-abi)
add_custom_command(TARGET bad_abi_fixture POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_SOURCE_DIR}/tests/fixtures/plugins/bad-abi/manifest.json
        ${CMAKE_BINARY_DIR}/tests/fixtures/plugins/bad-abi/manifest.json)

add_library(mismatched_tier_fixture SHARED tests/fixtures/plugins/mismatched-tier/plugin.cpp)
target_include_directories(mismatched_tier_fixture PRIVATE ${CMAKE_SOURCE_DIR}/plugin_sdk)
set_target_properties(mismatched_tier_fixture PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/fixtures/plugins/mismatched-tier)
add_custom_command(TARGET mismatched_tier_fixture POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_SOURCE_DIR}/tests/fixtures/plugins/mismatched-tier/manifest.json
        ${CMAKE_BINARY_DIR}/tests/fixtures/plugins/mismatched-tier/manifest.json)

add_dependencies(jarvis_tests valid_echo_fixture bad_abi_fixture mismatched_tier_fixture)
```

(`add_dependencies` ensures the three fixture `.so`s are always built before `jarvis_tests`
runs, since its tests `dlopen` them by relative path into `${CMAKE_BINARY_DIR}/tests/...`.)

- [ ] **Step 5: Append acceptance-path and runtime-rejection tests**

Append to `tests/plugin_loader_test.cpp`:

```cpp
TEST(PluginLoaderTest, LoadsDispatchesAndCallsRealValidFixturePlugin) {
    CapabilityRegistry registry;
    PluginLoader loader;

    std::vector<PluginLoadResult> results =
        loader.loadFromDirectory("tests/fixtures/plugins/valid-echo/..", registry);

    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].loaded);
    EXPECT_EQ(results[0].pluginId, "valid-echo");
    ASSERT_EQ(loader.loadedPluginIds().size(), 1u);

    Engine engine;
    ExecutionContext context{engine, registry};
    std::optional<std::string> dispatchResult =
        registry.dispatch(std::string("fixture-echo"), "hello fixture", context);

    ASSERT_TRUE(dispatchResult.has_value());
    EXPECT_EQ(*dispatchResult, "hello fixture");
}

TEST(PluginLoaderTest, DisableThenUnloadRealValidFixturePlugin) {
    CapabilityRegistry registry;
    PluginLoader loader;
    loader.loadFromDirectory("tests/fixtures/plugins/valid-echo/..", registry);

    ASSERT_NE(registry.resolve(std::string("fixture-echo")), nullptr);

    EXPECT_TRUE(loader.disablePlugin("valid-echo", registry));
    EXPECT_EQ(registry.resolve(std::string("fixture-echo")), nullptr);

    EXPECT_TRUE(loader.unloadPlugin("valid-echo"));
    EXPECT_TRUE(loader.loadedPluginIds().empty());

    // Idempotency: can't disable/unload twice.
    EXPECT_FALSE(loader.disablePlugin("valid-echo", registry));
    EXPECT_FALSE(loader.unloadPlugin("valid-echo"));
}

TEST(PluginLoaderTest, UnloadRefusesBeforeDisable) {
    CapabilityRegistry registry;
    PluginLoader loader;
    loader.loadFromDirectory("tests/fixtures/plugins/valid-echo/..", registry);

    EXPECT_FALSE(loader.unloadPlugin("valid-echo"));  // still enabled — refuse
}

TEST(PluginLoaderTest, RejectsRealBadAbiFixtureAfterDlopen) {
    CapabilityRegistry registry;
    PluginLoader loader;

    std::vector<PluginLoadResult> results =
        loader.loadFromDirectory("tests/fixtures/plugins/bad-abi/..", registry);

    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results[0].loaded);
    EXPECT_NE(results[0].reason.find("runtime ABI"), std::string::npos);
    EXPECT_TRUE(registry.allByIntent().empty());
}

TEST(PluginLoaderTest, RejectsRealMismatchedTierFixtureAndRollsBackNothingCommitted) {
    CapabilityRegistry registry;
    PluginLoader loader;

    std::vector<PluginLoadResult> results =
        loader.loadFromDirectory("tests/fixtures/plugins/mismatched-tier/..", registry);

    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results[0].loaded);
    EXPECT_NE(results[0].reason.find("do not match manifest"), std::string::npos);
    // Nothing from the rejected plugin ends up dispatchable, at either tier.
    EXPECT_EQ(registry.resolve(std::string("fixture-mismatched")), nullptr);
}
```

(Each test passes the fixture's *parent* directory — e.g.
`"tests/fixtures/plugins/valid-echo/.."` resolves to `tests/fixtures/plugins`, which contains
exactly one subdirectory with a `manifest.json` relevant to that test, since each test's
working directory is `${CMAKE_BINARY_DIR}` where `tests/fixtures/plugins/<name>/` was populated
by Step 4's `add_custom_command` copy steps. If this relative path doesn't resolve correctly
against the actual CTest working directory, adjust to the correct relative path — verify with
`pwd` inside a failing test run before changing the loader itself.)

- [ ] **Step 6: Run tests to verify they pass**

Run: `cmake --build build --target jarvis_tests valid_echo_fixture bad_abi_fixture mismatched_tier_fixture && ./build/jarvis_tests --gtest_filter=PluginLoaderTest.*`
Expected: PASS (all tests, including the 5 new ones using real `.so` fixtures)

- [ ] **Step 7: Run the full suite once more**

Run: `./build/jarvis_tests`
Expected: PASS — everything, unchanged.

- [ ] **Step 8: Commit**

```bash
git add tests/fixtures/ tests/plugin_loader_test.cpp CMakeLists.txt
git commit -m "test(core): add real .so fixture plugins for PluginLoader acceptance tests"
```

---

## Task 6: `system-info` becomes the bundled reference dynamic plugin

**Files:**
- Create: `plugins/system-info/plugin.cpp`
- Create: `plugins/system-info/manifest.json`
- Modify: `core/capability_registry.cpp` (delete `makeSystemInfoCapability`, remove it from `registerBuiltinCapabilities`, remove its now-unused `operatingSystemName`/`architectureName`/`compilerName` helpers — or keep them if Step 1 below reuses them via a shared header; see Step 1)
- Modify: `core/capability_registry.h` (remove `makeSystemInfoCapability` declaration)
- Modify: `core/main.cpp`, `core/grpc_server_main.cpp` (construct `PluginLoader`, load `config/plugin_dirs.cfg`, call `loadFromDirectory`)
- Modify: `CMakeLists.txt` (new `system_info_plugin` target)
- Modify: `tests/capability_registry_test.cpp` (the `SystemInfoCapabilityTest`/`RegistersExactlyFiveCapabilitiesByIntentName` tests now need a loaded plugin, not `registerBuiltinCapabilities` alone)
- Test: same files above

**Interfaces:**
- Consumes: `PluginLoader::loadFromDirectory` (Task 4), `plugin_sdk/jarvis_plugin_abi.h` (Task 3).
- Produces: nothing further downstream — this is the capstone task proving the whole spec end-to-end.

- [ ] **Step 1: Write `plugins/system-info/plugin.cpp`**

This re-implements the exact logic Task 1 left in `core/capability_registry.cpp`'s
`makeSystemInfoCapability()`, as a real dynamically-loaded plugin:

```cpp
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>

#include "jarvis_plugin_abi.h"

namespace {

std::string operatingSystemName() {
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#else
    return "Unknown";
#endif
}

std::string architectureName() {
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#else
    return "Unknown";
#endif
}

std::string compilerName() {
#if defined(__clang__)
    return "Clang " + std::to_string(__clang_major__) + "." + std::to_string(__clang_minor__);
#elif defined(__GNUC__)
    return "GCC " + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__);
#elif defined(_MSC_VER)
    return "MSVC " + std::to_string(_MSC_VER);
#else
    return "Unknown";
#endif
}

char* systemInfoExecute(const char* /*payload*/) {
    std::ostringstream out;
    out << "System information:\n";
    out << "OS: " << operatingSystemName() << "\n";
    out << "Architecture: " << architectureName() << "\n";
    out << "Compiler: " << compilerName() << "\n";
    out << "C++ standard: " << __cplusplus << "\n";
    out << "Hardware threads: ";
    const unsigned int threadCount = std::thread::hardware_concurrency();
    if (threadCount == 0) {
        out << "unavailable";
    } else {
        out << threadCount;
    }

    const std::string text = out.str();
    char* result = static_cast<char*>(std::malloc(text.size() + 1));
    std::memcpy(result, text.c_str(), text.size() + 1);
    return result;
}

}  // namespace

extern "C" int jarvis_plugin_abi_version() {
    return JARVIS_PLUGIN_ABI_VERSION;
}

extern "C" int jarvis_plugin_register(void* host_context, const JarvisPluginHost* host) {
    return host->registerCapability(
        host_context, "system-info",
        "Shows local OS, architecture, compiler, and hardware-thread information. Usage: system-info",
        JARVIS_POWER_TIER_T0_READ_ONLY, &systemInfoExecute);
}
```

- [ ] **Step 2: Write `plugins/system-info/manifest.json`**

```json
{
  "id": "system-info",
  "version": "1.0.0",
  "abi_version": 1,
  "library": "libsystem_info_plugin.so",
  "capabilities": [
    {
      "intent": "system-info",
      "description": "Shows local OS, architecture, compiler, and hardware-thread information. Usage: system-info",
      "power_tier": "T0_READ_ONLY"
    }
  ]
}
```

- [ ] **Step 3: Remove the compiled-in `system-info` builtin**

In `core/capability_registry.h`, remove the line:
```cpp
Capability makeSystemInfoCapability();
```

In `core/capability_registry.cpp`:
- Remove the `makeSystemInfoCapability()` function entirely.
- Remove the line `registry.registerCapability(makeSystemInfoCapability());` from
  `registerBuiltinCapabilities()`.
- Remove the now-unused `operatingSystemName()`, `architectureName()`, `compilerName()`
  helper functions and the `#include <thread>` line (they moved to `plugins/system-info/plugin.cpp`
  in Step 1 — leaving unused copies here would be dead code).

- [ ] **Step 4: Wire the `system_info_plugin` CMake target**

Add to `CMakeLists.txt`:

```cmake
add_library(system_info_plugin SHARED plugins/system-info/plugin.cpp)
target_include_directories(system_info_plugin PRIVATE ${CMAKE_SOURCE_DIR}/plugin_sdk)
set_target_properties(system_info_plugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins/system-info)
add_custom_command(TARGET system_info_plugin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_SOURCE_DIR}/plugins/system-info/manifest.json
        ${CMAKE_BINARY_DIR}/plugins/system-info/manifest.json)

add_dependencies(jarvis system_info_plugin)
add_dependencies(jarvis_grpc_server system_info_plugin)
add_dependencies(jarvis_tests system_info_plugin)
```

(`config/plugin_dirs.cfg` from Task 4 already points at `build/plugins/`, which
`${CMAKE_BINARY_DIR}/plugins/system-info/` sits inside when `CMAKE_BINARY_DIR` is the
project's `build/` directory — matching the README's existing `cmake -S . -B build`
convention. If a build is configured with a different binary directory name, the shipped
`config/plugin_dirs.cfg` value stops matching; that's an accepted limitation of a relative,
convention-based default, not a bug to fix in this task.)

- [ ] **Step 5: Wire `PluginLoader` into `core/main.cpp`**

In `core/main.cpp`, add `#include "plugin_loader.h"` to the includes. In `main()`, change:

```cpp
    Engine engine;
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);
```

to:

```cpp
    Engine engine;
    PluginLoader pluginLoader;  // declared before registry so it outlives it (see
                                 // PluginLoader's destructor comment in core/plugin_loader.cpp)
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    for (const std::string& dir : loadPluginDirs("config/plugin_dirs.cfg")) {
        for (const PluginLoadResult& result : pluginLoader.loadFromDirectory(dir, registry)) {
            if (!result.loaded) {
                spdlog::warn("Plugin '{}' failed to load: {}", result.pluginId, result.reason);
            }
        }
    }
```

(This must run before the `--grant` flag's early-return branch and before
`registry.setPluginConfig(&pluginConfig)`, since `runGrantFlow` enumerates `registry.all()` to
list grantable capabilities — actually `runGrantFlow` iterates `registry.all()`, the
CommandType-keyed map, which never includes plugin-loaded capabilities (they have no
CommandType). This is an intentional, pre-existing scope limit of `--grant`'s "unknown
capability" listing and `system-info` itself is T0 (never grantable) — not a regression this
task needs to fix, since `--grant system-info` already correctly hits the "doesn't require a
consent grant" branch once `system-info` is resolvable by name at all... which it currently
ISN'T through `runGrantFlow`'s enum-based lookup loop, since that loop also iterates
`registry.all()`. Change `runGrantFlow`'s `for (const auto& [intent, cap] : registry.all())`
lookup loop to `registry.allByIntent()` in both places it appears in `core/main.cpp`, so a
plugin-loaded capability is a valid `--grant` target lookup too, consistent with Task 1 Step 5's
identical fix to `makeHelpCapability()`.)

- [ ] **Step 6: Wire `PluginLoader` into `core/grpc_server_main.cpp`**

Add `#include "plugin_loader.h"` to the includes. Change:

```cpp
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    PluginConfig pluginConfig = PluginConfig::load("config/capabilities.cfg", "config/consent_grants.cfg");
    registry.setPluginConfig(&pluginConfig);
```

to:

```cpp
    PluginLoader pluginLoader;  // declared before registry so it outlives it
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    for (const std::string& dir : loadPluginDirs("config/plugin_dirs.cfg")) {
        for (const PluginLoadResult& result : pluginLoader.loadFromDirectory(dir, registry)) {
            if (!result.loaded) {
                spdlog::warn("Plugin '{}' failed to load: {}", result.pluginId, result.reason);
            }
        }
    }

    PluginConfig pluginConfig = PluginConfig::load("config/capabilities.cfg", "config/consent_grants.cfg");
    registry.setPluginConfig(&pluginConfig);
```

- [ ] **Step 7: Update `tests/capability_registry_test.cpp`**

`registerBuiltinCapabilities(registry)` alone no longer registers `system-info` — it must now
come from an actual `PluginLoader::loadFromDirectory()` call against the built plugin. Replace
the tests Task 1 added/adjusted:

```cpp
TEST(SystemInfoCapabilityTest, ReturnsLocalRuntimeInformation) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    Engine engine;
    ExecutionContext context{engine, registry};
    std::optional<std::string> result = registry.dispatch(std::string("system-info"), "", context);

    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->find("System information:"), std::string::npos);
    EXPECT_NE(result->find("OS:"), std::string::npos);
    EXPECT_NE(result->find("Architecture:"), std::string::npos);
    EXPECT_NE(result->find("Hardware threads:"), std::string::npos);
}

TEST(SystemInfoCapabilityTest, IsPowerTierT0) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    const Capability* systemInfo = registry.resolve(std::string("system-info"));
    ASSERT_NE(systemInfo, nullptr);
    EXPECT_EQ(systemInfo->powerTier, PowerTier::T0_READ_ONLY);
}

TEST(SystemInfoCapabilityTest, HasNoCommandTypeEntry) {
    // system-info is intent-only (proves the string-dispatch bridge, ahead of becoming a
    // real dynamically-loaded plugin) — it must never appear in the CommandType-keyed map.
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    for (const auto& [commandType, capability] : registry.all()) {
        EXPECT_NE(capability.name, "system-info");
    }
}
```

with:

```cpp
TEST(SystemInfoPluginTest, LoadsAndDispatchesViaPluginLoader) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);
    PluginLoader pluginLoader;

    std::vector<PluginLoadResult> results = pluginLoader.loadFromDirectory("plugins", registry);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].loaded);
    EXPECT_EQ(results[0].pluginId, "system-info");

    Engine engine;
    ExecutionContext context{engine, registry};
    std::optional<std::string> result = registry.dispatch(std::string("system-info"), "", context);

    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->find("System information:"), std::string::npos);
    EXPECT_NE(result->find("OS:"), std::string::npos);
    EXPECT_NE(result->find("Architecture:"), std::string::npos);
    EXPECT_NE(result->find("Hardware threads:"), std::string::npos);
}

TEST(SystemInfoPluginTest, IsPowerTierT0) {
    CapabilityRegistry registry;
    PluginLoader pluginLoader;
    pluginLoader.loadFromDirectory("plugins", registry);

    const Capability* systemInfo = registry.resolve(std::string("system-info"));
    ASSERT_NE(systemInfo, nullptr);
    EXPECT_EQ(systemInfo->powerTier, PowerTier::T0_READ_ONLY);
}

TEST(SystemInfoPluginTest, RegistersNoCommandTypeEntry) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);
    PluginLoader pluginLoader;
    pluginLoader.loadFromDirectory("plugins", registry);

    for (const auto& [commandType, capability] : registry.all()) {
        EXPECT_NE(capability.name, "system-info");
    }
}

TEST(SystemInfoPluginTest, DisableThenUnload) {
    CapabilityRegistry registry;
    PluginLoader pluginLoader;
    pluginLoader.loadFromDirectory("plugins", registry);
    ASSERT_NE(registry.resolve(std::string("system-info")), nullptr);

    EXPECT_TRUE(pluginLoader.disablePlugin("system-info", registry));
    EXPECT_EQ(registry.resolve(std::string("system-info")), nullptr);
    EXPECT_TRUE(pluginLoader.unloadPlugin("system-info"));
}
```

Add `#include "plugin_loader.h"` to the top of `tests/capability_registry_test.cpp`.

Also fix `TEST(BuiltinCapabilitiesTest, RegistersExactlyFiveCapabilitiesByIntentName)` — since
`system-info` no longer comes from `registerBuiltinCapabilities()`, that test's expectation of
`5u`/`resolve(std::string("system-info"))` was already testing the WRONG source after Task 1
already made it plugin-only in spirit (Task 1 only proved the *bridge*, not real loading).
Rename/adjust it to check 4 builtins only:

```cpp
TEST(BuiltinCapabilitiesTest, RegistersExactlyFiveCapabilitiesByIntentName) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    EXPECT_EQ(registry.allByIntent().size(), 4u);
    EXPECT_NE(registry.resolve(std::string("echo")), nullptr);
    EXPECT_NE(registry.resolve(std::string("status")), nullptr);
    EXPECT_NE(registry.resolve(std::string("about")), nullptr);
    EXPECT_NE(registry.resolve(std::string("help")), nullptr);
}
```
(Test name says "Five" but now checks 4 — rename it to
`RegistersExactlyFourCapabilitiesByIntentName` to match; this is a one-line rename plus the
`5u`→`4u`/dropped `system-info` line change above.)

`HelpCapabilityTest.ListsAllFiveBuiltinsWithoutHardcodingThem` and
`CapabilityRegistryPluginGateTest.AllFiveBuiltinsDispatchWithPluginConfigSetAndEmptyConfig`
both reference `system-info` — since `help`'s test only calls `registerBuiltinCapabilities`
(no `PluginLoader`), `system-info` won't be listed anymore. Remove the
`EXPECT_NE(result->find("system-info"), std::string::npos);` line from
`ListsAllFiveBuiltinsWithoutHardcodingThem` (rename it `ListsAllFourBuiltinsWithoutHardcodingThem`)
and remove the
`EXPECT_TRUE(registry.dispatch(std::string("system-info"), "", context).has_value());` line
from `AllFiveBuiltinsDispatchWithPluginConfigSetAndEmptyConfig` (rename it
`AllFourBuiltinsDispatchWithPluginConfigSetAndEmptyConfig`) — both tests now only exercise the
4 compiled-in builtins, which is accurate: `system-info` is no longer a builtin at all.

- [ ] **Step 8: Build and run everything**

```bash
cmake --build build --target jarvis jarvis_grpc_server jarvis_tests system_info_plugin
./build/jarvis_tests
```
Expected: all four targets build clean; every test passes, including the new
`SystemInfoPluginTest` suite.

- [ ] **Step 9: Manual verification**

```bash
ls build/plugins/system-info/          # confirm libsystem_info_plugin.so + manifest.json both present
echo -e "system-info\nhelp\nexit" | ./build/jarvis
```
Expected: `system-info` output identical to Task 1's; `help` lists `system-info` again (now
sourced from the loaded plugin, not a compiled-in builtin).

```bash
# Simulate a broken plugin directory and confirm startup survives it:
mkdir -p /tmp/jarvis-plugin-smoke/broken
echo '{ not valid json' > /tmp/jarvis-plugin-smoke/broken/manifest.json
echo "build/plugins/
/tmp/jarvis-plugin-smoke" > config/plugin_dirs.cfg
echo -e "system-info\nexit" | ./build/jarvis
# Expected: a spdlog warning about the broken manifest, but system-info still works and the
# process doesn't crash. Restore config/plugin_dirs.cfg to just "build/plugins/" afterward.
git checkout config/plugin_dirs.cfg
```

- [ ] **Step 10: Run the full test suite one final time**

```bash
cmake --build build --target jarvis_tests && ./build/jarvis_tests
PYTHONPATH=ai:generated/python .venv/bin/python -m pytest ai tools voice/tests -q
```
Expected: everything passes.

- [ ] **Step 11: Commit**

```bash
git add plugins/ core/capability_registry.h core/capability_registry.cpp core/main.cpp \
        core/grpc_server_main.cpp tests/capability_registry_test.cpp CMakeLists.txt
git commit -m "feat(plugins): convert system-info into the bundled reference dynamic plugin"
```

---

## Post-plan housekeeping (not a task — do after Task 6, per project CLAUDE.md §4)

- Update `docs/features.md`: mark "Runtime plugin discovery & loading" as done, describe the
  ABI/manifest/loader briefly, note `system-info` is now the reference dynamic plugin (not a
  compiled-in builtin).
- Update `docs/roadmap.md` Phase 4 section.
- Update `README.md`'s "What works today" list and add a short "Plugins" section documenting
  `plugin_sdk/jarvis_plugin_abi.h`, the manifest format, and `config/plugin_dirs.cfg`.
- Append to `qmul/notes/genai-usage-log.md` and a new `qmul/logbook/2026-08-31-plugin-sdk-loader.md`
  entry, per the standing documentation duty (project CLAUDE.md §6.2) — note the direction
  change from the prior session's "framework only" scoping, since that's exactly the kind of
  decision-and-rationale record the logbook exists for.
