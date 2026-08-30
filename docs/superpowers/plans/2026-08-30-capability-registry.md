# Capability Registry Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `core/command_handler.cpp`'s hardcoded `COMMAND_DISPATCH`/`COMMAND_DESCRIPTIONS`
maps with a `CapabilityRegistry` that `echo`/`status`/`about`/`help` register themselves into,
so both the CLI and the gRPC service dispatch through one mechanism instead of two, and add the
project's first C++ tests (GoogleTest) to verify it.

**Architecture:** A new `Capability` struct (name, intent, description, power tier, an
`execute(payload, context)` function) plus a `CapabilityRegistry` (register/resolve/dispatch) in
new files `core/capability.h` and `core/capability_registry.h/.cpp`. `echo`/`about`/`status`/
`help` become real capabilities built by factory functions and registered via one explicit
`registerBuiltinCapabilities()` call from each binary's `main()`. `exit` and "no match" (`UNKNOWN`)
stay outside the registry exactly as today. `core/engine.cpp` and `core/jarvis_service.cpp` both
switch from calling the old `runCMD()` to `registry.dispatch(...)`.

**Tech Stack:** C++17, CMake, GoogleTest (already installed system-wide — `pkg-config --exists
gtest` confirmed).

**Spec:** `docs/superpowers/specs/2026-08-30-capability-registry-design.md`

## Global Constraints

- No dynamic/runtime plugin loading (`.so` files) — capabilities are compiled in and
  self-register via one explicit function call, not a clever static-initializer trick.
- No configuration system — capabilities register themselves in code.
- No consent-gate *enforcement* — every capability declares `PowerTier::T0_READ_ONLY` (the only
  tier any current capability needs); nothing blocks or prompts based on tier yet.
- `CommandType::UNKNOWN` is never registered in the registry — "no capability matched" stays a
  distinct outcome (`registry.resolve()` returns `nullptr` / `dispatch()` returns
  `std::nullopt`), not a capability.
- `exit` stays completely outside the registry — handled only in `core/engine.cpp`'s CLI loop,
  exactly as today. Sending `COMMAND_TYPE_EXIT` over gRPC still does not terminate the server
  (unchanged, confirmed pre-existing behaviour).
- `help`'s "list everything" output must NOT include an `unknown` entry (intentional behaviour
  change — `unknown` was never a real user-invokable command) and MUST still include `exit`
  (appended as one hardcoded line, since it's a real command the user can type even though it's
  not in the registry).
- Existing external behaviour must be unchanged except the two documented `help` differences
  above: `tools/grpc_smoke_test.py` and manual CLI checks (`echo`, `status`, `about`, `help`,
  `help echo`, `exit`, an unknown command) must all still pass/behave as before.
- Follow the existing CMake pattern: each executable (and now the test binary) lists its own
  `.cpp` sources directly (no new shared library target — the codebase doesn't use one today,
  don't introduce the abstraction here).

---

### Task 1: `Capability` contract, `CapabilityRegistry` class, and GTest setup

**Files:**
- Create: `core/capability.h`
- Create: `core/capability_registry.h`
- Create: `core/capability_registry.cpp` (class methods only this task — builtin capabilities
  come in later tasks)
- Create: `tests/capability_registry_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `CommandType` from `core/command_handler.h` (existing, unmodified this task).
- Produces (for later tasks): `PowerTier` enum, `ExecutionContext` struct (`Engine& engine`,
  `const CapabilityRegistry& registry`), `Capability` struct (`name`, `intent`, `description`,
  `powerTier`, `execute` — `std::function<std::string(const std::string&, ExecutionContext&)>`),
  `CapabilityRegistry` class with `registerCapability(Capability)`,
  `resolve(CommandType) const -> const Capability*`,
  `dispatch(CommandType, const std::string&, ExecutionContext&) const -> std::optional<std::string>`,
  `all() const -> const std::unordered_map<CommandType, Capability>&`.
  Also declares (but does not yet define a body for) `void registerBuiltinCapabilities(CapabilityRegistry&);`
  — Task 2 gives it a real body.

- [ ] **Step 1: Create the contract header**

Create `core/capability.h`:

```cpp
#pragma once

#include <functional>
#include <string>

#include "command_handler.h"

// A capability's declared risk level. Every capability today is T0 — nothing above T0 exists
// yet, so there's nothing to build/test real gate-enforcement logic against (see the design
// spec §1 for why enforcement is deliberately deferred).
enum class PowerTier {
    T0_READ_ONLY,
    T1_STATEFUL_LOCAL,
    T2_SYSTEM_AFFECTING,
    T3_DESTRUCTIVE,
    T4_EXTERNAL
};

class Engine;
class CapabilityRegistry;

// What a capability's execute() function can reach beyond its own payload. Most capabilities
// (e.g. echo) ignore this entirely; status reads engine, help reads registry.
struct ExecutionContext {
    Engine& engine;
    const CapabilityRegistry& registry;
};

// One thing JARVIS can do — a self-contained bundle of what it's called, what intent it
// answers to, what it does (for `help`), how risky it is, and the function that does the work.
struct Capability {
    std::string name;
    CommandType intent;
    std::string description;
    PowerTier powerTier;
    std::function<std::string(const std::string& payload, ExecutionContext& context)> execute;
};
```

- [ ] **Step 2: Create the registry header**

Create `core/capability_registry.h`:

```cpp
#pragma once

#include <optional>
#include <string>
#include <unordered_map>

#include "capability.h"

// Resolves an intent (CommandType) to the capability that handles it. Replaces the old
// hardcoded COMMAND_DISPATCH/COMMAND_DESCRIPTIONS maps in command_handler.cpp — capabilities
// register themselves here instead of being hand-added to a central table.
class CapabilityRegistry {
 public:
    void registerCapability(Capability capability);

    // Returns nullptr if no capability answers this intent. CommandType::UNKNOWN is never
    // registered, so resolving it always returns nullptr — "no match" stays a real, distinct
    // outcome, not a capability.
    const Capability* resolve(CommandType intent) const;

    // resolve() + execute() in one call. std::nullopt means resolve() would have returned
    // nullptr, so callers can tell "ran, returned this" apart from "nothing to run".
    std::optional<std::string> dispatch(CommandType intent, const std::string& payload,
                                         ExecutionContext& context) const;

    // For `help` to enumerate what's registered, and for tests.
    const std::unordered_map<CommandType, Capability>& all() const;

 private:
    std::unordered_map<CommandType, Capability> capabilities_;
};

// One explicit call site registers every built-in capability. Adding a new one means writing
// it and adding one line here — nothing else in the dispatch path changes.
void registerBuiltinCapabilities(CapabilityRegistry& registry);
```

- [ ] **Step 3: Write the failing tests**

Create `tests/capability_registry_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include "capability_registry.h"
#include "engine.h"

TEST(CapabilityRegistryTest, ResolveReturnsNullForUnregisteredIntent) {
    CapabilityRegistry registry;
    EXPECT_EQ(registry.resolve(CommandType::ECHO), nullptr);
}

TEST(CapabilityRegistryTest, ResolveReturnsRegisteredCapability) {
    CapabilityRegistry registry;
    registry.registerCapability(Capability{
        "echo", CommandType::ECHO, "test echo", PowerTier::T0_READ_ONLY,
        [](const std::string& payload, ExecutionContext&) { return payload; }
    });

    const Capability* found = registry.resolve(CommandType::ECHO);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->name, "echo");
}

TEST(CapabilityRegistryTest, DispatchInvokesRegisteredCapability) {
    CapabilityRegistry registry;
    registry.registerCapability(Capability{
        "echo", CommandType::ECHO, "test echo", PowerTier::T0_READ_ONLY,
        [](const std::string& payload, ExecutionContext&) { return "echoed: " + payload; }
    });

    Engine engine;
    ExecutionContext context{engine, registry};
    std::optional<std::string> result = registry.dispatch(CommandType::ECHO, "hello", context);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "echoed: hello");
}

TEST(CapabilityRegistryTest, DispatchReturnsNulloptForUnregisteredIntent) {
    CapabilityRegistry registry;
    Engine engine;
    ExecutionContext context{engine, registry};

    std::optional<std::string> result = registry.dispatch(CommandType::UNKNOWN, "anything", context);
    EXPECT_FALSE(result.has_value());
}
```

- [ ] **Step 4: Implement the registry class**

Create `core/capability_registry.cpp`:

```cpp
#include "capability_registry.h"

void CapabilityRegistry::registerCapability(Capability capability) {
    capabilities_[capability.intent] = std::move(capability);
}

const Capability* CapabilityRegistry::resolve(CommandType intent) const {
    auto it = capabilities_.find(intent);
    if (it == capabilities_.end()) {
        return nullptr;
    }
    return &it->second;
}

std::optional<std::string> CapabilityRegistry::dispatch(
    CommandType intent, const std::string& payload, ExecutionContext& context) const {
    const Capability* capability = resolve(intent);
    if (!capability) {
        return std::nullopt;
    }
    return capability->execute(payload, context);
}

const std::unordered_map<CommandType, Capability>& CapabilityRegistry::all() const {
    return capabilities_;
}

// Real body added in Task 2 — declared in capability_registry.h, defined here so this file
// compiles and links standalone. Empty is correct for this task: no capability exists yet.
void registerBuiltinCapabilities(CapabilityRegistry& /*registry*/) {
}
```

- [ ] **Step 5: Add GTest to the build**

Modify `CMakeLists.txt`. After the existing `find_package(spdlog REQUIRED)` line, add:

```cmake
find_package(GTest REQUIRED)
```

After `set(CMAKE_CXX_STANDARD_REQUIRED ON)` near the top, add:

```cmake
enable_testing()
```

At the end of the file, add:

```cmake
add_executable(
    jarvis_tests
    tests/capability_registry_test.cpp
    core/capability_registry.cpp
    core/command_handler.cpp
    core/engine.cpp
)

target_include_directories(
    jarvis_tests
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/core
)

target_link_libraries(
    jarvis_tests
    PRIVATE
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(jarvis_tests)
```

(`GTest::gtest_main` provides `main()` — don't write one in the test file.)

- [ ] **Step 6: Build and run the tests**

Run:
```bash
cd "/home/rayyan/Programming/Remote Repos/Public/JARVIS"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
./build/jarvis_tests
```
Expected: clean build (existing `jarvis`/`jarvis_grpc_server` targets still build too — this
task doesn't touch anything they depend on), then all 4 tests in `jarvis_tests` PASS.

- [ ] **Step 7: Commit**

```bash
git add core/capability.h core/capability_registry.h core/capability_registry.cpp tests/capability_registry_test.cpp CMakeLists.txt
git commit -m "feat(core): add Capability contract, CapabilityRegistry, and GTest setup"
```

---

### Task 2: `echo` and `about` capabilities

**Files:**
- Modify: `core/capability_registry.cpp` (add factory functions + real `registerBuiltinCapabilities` body)
- Modify: `core/capability_registry.h` (declare the two factory functions)
- Test: `tests/capability_registry_test.cpp`

**Interfaces:**
- Consumes: `Capability`, `CapabilityRegistry`, `ExecutionContext` (Task 1).
- Produces (for later tasks): `Capability makeEchoCapability();`, `Capability makeAboutCapability();`
  — free functions declared in `core/capability_registry.h`, defined in
  `core/capability_registry.cpp`. `registerBuiltinCapabilities()` now registers both.

- [ ] **Step 1: Write the failing tests**

Append to `tests/capability_registry_test.cpp`:

```cpp
TEST(BuiltinCapabilitiesTest, EchoReturnsPayloadUnchanged) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    Engine engine;
    ExecutionContext context{engine, registry};
    std::optional<std::string> result = registry.dispatch(CommandType::ECHO, "hello there", context);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "hello there");
}

TEST(BuiltinCapabilitiesTest, AboutReturnsFixedDescription) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    Engine engine;
    ExecutionContext context{engine, registry};
    std::optional<std::string> result = registry.dispatch(CommandType::ABOUT, "", context);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "JARVIS Core Engine v1.0\nDeveloped by Rayyan.");
}

TEST(BuiltinCapabilitiesTest, EchoAndAboutAreBothPowerTierT0) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    const Capability* echo = registry.resolve(CommandType::ECHO);
    const Capability* about = registry.resolve(CommandType::ABOUT);
    ASSERT_NE(echo, nullptr);
    ASSERT_NE(about, nullptr);
    EXPECT_EQ(echo->powerTier, PowerTier::T0_READ_ONLY);
    EXPECT_EQ(about->powerTier, PowerTier::T0_READ_ONLY);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build -j4 && ./build/jarvis_tests`
Expected: the 3 new tests FAIL (echo/about resolve to `nullptr` since nothing is registered yet
— `registerBuiltinCapabilities` still has an empty body from Task 1). The 4 tests from Task 1
still PASS.

- [ ] **Step 3: Add the factory function declarations**

In `core/capability_registry.h`, add after the `registerBuiltinCapabilities` declaration:

```cpp
// Builtin capability factories — exposed for direct testing (see tests/capability_registry_test.cpp).
Capability makeEchoCapability();
Capability makeAboutCapability();
```

- [ ] **Step 4: Implement the two capabilities**

In `core/capability_registry.cpp`, replace the empty `registerBuiltinCapabilities` body and add
the two factory functions above it:

```cpp
Capability makeEchoCapability() {
    return Capability{
        "echo",
        CommandType::ECHO,
        "Echoes the input back to the user. Usage: echo [text]",
        PowerTier::T0_READ_ONLY,
        [](const std::string& payload, ExecutionContext& /*context*/) -> std::string {
            return payload;
        }
    };
}

Capability makeAboutCapability() {
    return Capability{
        "about",
        CommandType::ABOUT,
        "Provides information about JARVIS. Usage: about",
        PowerTier::T0_READ_ONLY,
        [](const std::string& /*payload*/, ExecutionContext& /*context*/) -> std::string {
            return "JARVIS Core Engine v1.0\nDeveloped by Rayyan.";
        }
    };
}

void registerBuiltinCapabilities(CapabilityRegistry& registry) {
    registry.registerCapability(makeEchoCapability());
    registry.registerCapability(makeAboutCapability());
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cmake --build build -j4 && ./build/jarvis_tests`
Expected: all 7 tests PASS (4 from Task 1 + 3 new).

- [ ] **Step 6: Commit**

```bash
git add core/capability_registry.h core/capability_registry.cpp tests/capability_registry_test.cpp
git commit -m "feat(core): register echo and about as real capabilities"
```

---

### Task 3: `status` capability (first use of `ExecutionContext`)

**Files:**
- Modify: `core/capability_registry.cpp` (add `makeStatusCapability`, register it)
- Modify: `core/capability_registry.h` (declare `makeStatusCapability`)
- Test: `tests/capability_registry_test.cpp`

**Interfaces:**
- Consumes: `Engine::getStatusInfo() -> StatusInfo` (existing, unmodified, from `core/engine.h`).
- Produces: `Capability makeStatusCapability();`, registered in `registerBuiltinCapabilities()`.

- [ ] **Step 1: Write the failing test**

Append to `tests/capability_registry_test.cpp`:

```cpp
TEST(StatusCapabilityTest, ReflectsLiveEngineState) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    Engine engine;
    ExecutionContext context{engine, registry};
    std::optional<std::string> result = registry.dispatch(CommandType::STATUS, "", context);

    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->find("Engine: running"), std::string::npos);
    EXPECT_NE(result->find("Last command: none"), std::string::npos);
}

TEST(StatusCapabilityTest, IsPowerTierT0) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    const Capability* status = registry.resolve(CommandType::STATUS);
    ASSERT_NE(status, nullptr);
    EXPECT_EQ(status->powerTier, PowerTier::T0_READ_ONLY);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build -j4 && ./build/jarvis_tests`
Expected: the 2 new tests FAIL (`registry.resolve(CommandType::STATUS)` returns `nullptr` —
nothing registers STATUS yet). The 7 existing tests still PASS.

- [ ] **Step 3: Add the factory function declaration**

In `core/capability_registry.h`, add after `makeAboutCapability`:

```cpp
Capability makeStatusCapability();
```

- [ ] **Step 4: Implement the status capability**

In `core/capability_registry.cpp`:

1. Add `#include "engine.h"` and `#include <iomanip>` and `#include <sstream>` to the top of
   the file (needed for `Engine::getStatusInfo()`/`StatusInfo` and the formatting below).

2. Add this factory function (after `makeAboutCapability`):

```cpp
Capability makeStatusCapability() {
    return Capability{
        "status",
        CommandType::STATUS,
        "Shows engine state, uptime, and last command. Usage: status",
        PowerTier::T0_READ_ONLY,
        [](const std::string& /*payload*/, ExecutionContext& context) -> std::string {
            StatusInfo info = context.engine.getStatusInfo();

            const long hours   = info.uptimeSeconds / 3600;
            const long minutes = (info.uptimeSeconds % 3600) / 60;
            const long seconds = info.uptimeSeconds % 60;

            std::ostringstream out;
            out << "Engine: " << (info.running ? "running" : "stopped") << "\n";
            out << "Uptime: "
                << std::setfill('0') << std::setw(2) << hours   << ":"
                << std::setw(2)      << minutes << ":"
                << std::setw(2)      << seconds << "\n";
            out << "Last command: " << info.lastCommand;
            return out.str();
        }
    };
}
```

3. Update `registerBuiltinCapabilities` to also register it:

```cpp
void registerBuiltinCapabilities(CapabilityRegistry& registry) {
    registry.registerCapability(makeEchoCapability());
    registry.registerCapability(makeAboutCapability());
    registry.registerCapability(makeStatusCapability());
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cmake --build build -j4 && ./build/jarvis_tests`
Expected: all 9 tests PASS.

- [ ] **Step 6: Commit**

```bash
git add core/capability_registry.h core/capability_registry.cpp tests/capability_registry_test.cpp
git commit -m "feat(core): register status as a real capability, reading live Engine state"
```

---

### Task 4: `help` capability (enumerates the registry; drops `unknown`, keeps `exit`)

**Files:**
- Modify: `core/capability_registry.cpp` (add `makeHelpCapability`, register it)
- Modify: `core/capability_registry.h` (declare `makeHelpCapability`)
- Test: `tests/capability_registry_test.cpp`

**Interfaces:**
- Consumes: `CapabilityRegistry::all()` (Task 1), `toLower()` from `core/command_handler.h`
  (existing, unmodified).
- Produces: `Capability makeHelpCapability();`, registered in `registerBuiltinCapabilities()`.
  After this task, `registerBuiltinCapabilities()` registers all four capabilities — this is the
  point other tasks (5, 6) can rely on `registry.all().size() == 4`.

- [ ] **Step 1: Write the failing tests**

Append to `tests/capability_registry_test.cpp`:

```cpp
TEST(BuiltinCapabilitiesTest, RegistersExactlyFourExpectedCapabilities) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    EXPECT_EQ(registry.all().size(), 4u);
    EXPECT_NE(registry.resolve(CommandType::ECHO), nullptr);
    EXPECT_NE(registry.resolve(CommandType::STATUS), nullptr);
    EXPECT_NE(registry.resolve(CommandType::ABOUT), nullptr);
    EXPECT_NE(registry.resolve(CommandType::HELP), nullptr);
    EXPECT_EQ(registry.resolve(CommandType::UNKNOWN), nullptr);
}

TEST(HelpCapabilityTest, ListsAllFourBuiltinsWithoutHardcodingThem) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    Engine engine;
    ExecutionContext context{engine, registry};
    std::optional<std::string> result = registry.dispatch(CommandType::HELP, "", context);

    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->find("echo"), std::string::npos);
    EXPECT_NE(result->find("status"), std::string::npos);
    EXPECT_NE(result->find("about"), std::string::npos);
    EXPECT_NE(result->find("help"), std::string::npos);
}

TEST(HelpCapabilityTest, DoesNotListUnknown) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    Engine engine;
    ExecutionContext context{engine, registry};
    std::optional<std::string> result = registry.dispatch(CommandType::HELP, "", context);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->find("- unknown:"), std::string::npos);
}

TEST(HelpCapabilityTest, ListsExitEvenThoughItIsNotACapability) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    Engine engine;
    ExecutionContext context{engine, registry};
    std::optional<std::string> result = registry.dispatch(CommandType::HELP, "", context);

    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->find("exit"), std::string::npos);
}

TEST(HelpCapabilityTest, SpecificCommandLookupFindsRegisteredCapability) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    Engine engine;
    ExecutionContext context{engine, registry};
    std::optional<std::string> result = registry.dispatch(CommandType::HELP, "echo", context);

    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->find("echo"), std::string::npos);
    EXPECT_NE(result->find("Echoes"), std::string::npos);
}

TEST(HelpCapabilityTest, SpecificCommandLookupReportsNotFoundForUnregistered) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    Engine engine;
    ExecutionContext context{engine, registry};
    std::optional<std::string> result = registry.dispatch(CommandType::HELP, "bananas", context);

    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->find("Command not found"), std::string::npos);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build -j4 && ./build/jarvis_tests`
Expected: the 6 new tests FAIL (`registry.resolve(CommandType::HELP)` returns `nullptr`, so
`dispatch()` returns `std::nullopt` and every `ASSERT_TRUE(result.has_value())` fails; the count
test fails at `all().size() == 4u` since only 3 are registered). The 9 existing tests still PASS.

- [ ] **Step 3: Add the factory function declaration**

In `core/capability_registry.h`, add after `makeStatusCapability`:

```cpp
Capability makeHelpCapability();
```

- [ ] **Step 4: Implement the help capability**

In `core/capability_registry.cpp`:

1. Add this factory function (after `makeStatusCapability`):

```cpp
Capability makeHelpCapability() {
    return Capability{
        "help",
        CommandType::HELP,
        "Provides information about available capabilities. Usage: help [command]",
        PowerTier::T0_READ_ONLY,
        [](const std::string& payload, ExecutionContext& context) -> std::string {
            std::ostringstream out;

            if (payload.empty()) {
                out << "Available commands:\n";
                for (const auto& [intent, capability] : context.registry.all()) {
                    out << "  - " << capability.name << ": " << capability.description << "\n";
                }
                // exit stays outside the registry (engine-lifecycle control, not a
                // capability) but is still a real command the user can type — listed here
                // as one hardcoded line rather than being invented as a fake capability.
                out << "  - exit: Terminates the JARVIS Core Engine. Usage: exit\n";
                return out.str();
            }

            std::string commandName = toLower(payload);

            if (commandName == "exit") {
                return "exit: Terminates the JARVIS Core Engine. Usage: exit";
            }

            for (const auto& [intent, capability] : context.registry.all()) {
                if (capability.name == commandName) {
                    out << capability.name << ": " << capability.description;
                    return out.str();
                }
            }

            out << "Command not found: " << commandName << "\n";
            out << "Type 'help' to see all available commands.";
            return out.str();
        }
    };
}
```

2. Update `registerBuiltinCapabilities` to register it too:

```cpp
void registerBuiltinCapabilities(CapabilityRegistry& registry) {
    registry.registerCapability(makeEchoCapability());
    registry.registerCapability(makeAboutCapability());
    registry.registerCapability(makeStatusCapability());
    registry.registerCapability(makeHelpCapability());
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cmake --build build -j4 && ./build/jarvis_tests`
Expected: all 15 tests PASS.

- [ ] **Step 6: Commit**

```bash
git add core/capability_registry.h core/capability_registry.cpp tests/capability_registry_test.cpp
git commit -m "feat(core): register help as a real capability, enumerating the registry"
```

---

### Task 5: Wire the CLI (`core/command_handler`, `core/engine`, `core/main.cpp`) to the registry

**Files:**
- Modify: `core/command_handler.h`
- Modify: `core/command_handler.cpp`
- Modify: `core/engine.h`
- Modify: `core/engine.cpp`
- Modify: `core/main.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `CapabilityRegistry::dispatch()` (Task 1), `registerBuiltinCapabilities()` (Task 4
  gives it its final 4-capability body). `runUnknown()` (existing, kept — see below).
- Produces: `Engine::run(CapabilityRegistry& registry)` — signature change from today's
  `Engine::run()` (no args). Task 6 does the equivalent for the gRPC side independently; neither
  task depends on the other's interface.

- [ ] **Step 1: Replace `core/command_handler.h`**

Replace the full contents of `core/command_handler.h` with:

```cpp
#pragma once

#include <string>
#include <unordered_map>

//Enum classes

//Command types supported by JARVIS
enum class CommandType {
    ECHO,
    UNKNOWN,
    EXIT,
    HELP, 
    ABOUT,
    STATUS
};

//Structs
struct ParsedCommand {
    CommandType type;
    std::string payload;
};

//User input handling functions
ParsedCommand parseCommand(const std::string& command);

//Parsing helper functions
std::string toLower(std::string text);
CommandType extractCommandType(std::istringstream& stream);
std::string extractPayload(std::istringstream& stream);

// Fallback text when no capability matched the parsed command. Kept here (not a capability —
// CommandType::UNKNOWN is never registered in the CapabilityRegistry) since both the CLI and
// the gRPC service need this exact string when a dispatch comes back empty.
std::string runUnknown();
```

(Removed: `handleCommand`, `runCMD`, `runEcho`, `runHelp`, `runAbout` declarations — their
logic now lives in the capabilities from Tasks 2–4, or is inlined into the callers below.)

- [ ] **Step 2: Replace `core/command_handler.cpp`**

Replace the full contents of `core/command_handler.cpp` with:

```cpp
#include "command_handler.h"

#include <sstream>
#include <algorithm>
#include <cctype>
#include <unordered_map>

//COMMAND MAPPING

//User -> CommandType mapping
//Maps command strings to their corresponding CommandType
static const std::unordered_map<std::string, CommandType> COMMAND_MAP = {
    {"echo", CommandType::ECHO},
    {"unknown", CommandType::UNKNOWN},
    {"exit", CommandType::EXIT}, 
    {"help", CommandType::HELP}, 
    {"about", CommandType::ABOUT},
    {"status", CommandType::STATUS}
};

//PARSING HELPER FUNCTIONS

//Trim function to remove leading and trailing whitespace from a string
namespace {
std::string trim(const std::string& text) {
    const std::size_t first = text.find_first_not_of(" \t\n\r");
    if (first == std::string::npos) {
        return "";
    }

    const std::size_t last = text.find_last_not_of(" \t\n\r");
    return text.substr(first, last - first + 1);
    }
}

//Convert a string to lowercase for case-insensitive command parsing
std::string toLower(std::string text) {
    std::transform(
        text.begin(),
        text.end(),
        text.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        }
    );
    return text;
}

//Parse the user input to determine the type of command
ParsedCommand parseCommand(const std::string& input) {
    //Commands are broken down into a command type (first word) and an optional payload
    //The parsed command object is then returned for execution

    //Return a default object of type unknown if the input cannot be parsed
    ParsedCommand result{CommandType::UNKNOWN, ""};

    //Remove leading and trailing whitespace
    std::string cleaned = trim(input); 

    if (cleaned.empty()) { // If the cleaned input is empty, return the default unknown command
        return result;
    }

    std::istringstream stream(cleaned); // Turn the cleaned input into a string stream for word by word parsing

    //Part 1: Command extraction
    //Extract the command type (first word)
    CommandType commandType = extractCommandType(stream);

    //Part 2: Payload extraction
    //Extract rest of the input as payload
    std::string payload = extractPayload(stream);
    
    //Build result object
    result.type = commandType;
    result.payload = payload;

    return result;
}

//Extract command type 
CommandType extractCommandType(std::istringstream& stream) {
    std::string command;
    stream >> command;
    command = toLower(command);

    //Look up the command in the command map
    auto it = COMMAND_MAP.find(command);

    if (it !=COMMAND_MAP.end()) {
        //Command found in map, return the corresponding CommandType
        return it->second;
    }
    
    //Command not found, return UNKNOWN
    return CommandType::UNKNOWN;
}

//Extract payload content
std::string extractPayload(std::istringstream& stream) {
    std::string payload;
    std::getline(stream, payload);
    return trim(payload);
}

//Fallback text when no capability matched
std::string runUnknown() {
    return "Command not recognised. Please try again.";
}
```

- [ ] **Step 3: Replace `core/engine.h`**

Replace the full contents of `core/engine.h` with:

```cpp
#pragma once

#include <chrono>
#include <string>

class CapabilityRegistry;

//Status struct to return status information from engine to service layer
struct StatusInfo {
    bool running;
    long uptimeSeconds;
    std::string lastCommand;
};

class Engine{
    private:
        bool running; //Running flag
        std::chrono::steady_clock::time_point startTime; //track uptime
        std::string lastCommand;//track last command

        static std::string extractCommandName(const std::string& input);

    public:
        Engine();     
        void run(CapabilityRegistry& registry);
        void terminate();
        StatusInfo getStatusInfo() const;
};
```

(Removed the `printStatus()` declaration — the `status` capability from Task 3 now produces
that formatted string itself; the CLI just prints whatever the registry returns, same as every
other capability.)

- [ ] **Step 4: Replace `core/engine.cpp`**

Replace the full contents of `core/engine.cpp` with:

```cpp
#include "engine.h"
#include "capability.h"
#include "capability_registry.h"
#include "command_handler.h"

#include <cctype>
#include <chrono>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

//Engine Constructor
Engine::Engine() {
    running = true;
    startTime = std::chrono::steady_clock::now();
    lastCommand = "none";
}

//Engine entry point
void Engine::run(CapabilityRegistry& registry) {
    std::cout << "JARVIS Core Engine starting..." << std::endl;

    std::string input;

    while (running) {
        std::cout << ">";
        std::getline(std::cin, input);

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
        } else {
            std::cout << runUnknown() << std::endl;
        }
    }
}

//Program termination
void Engine::terminate() {
    std::cout << "Terminating JARVIS Core Engine..." << std::endl;
    running = false;
}

//Determine command type to perform required action
std::string Engine::extractCommandName(const std::string& input) {
    std::istringstream stream(input);
    std::string commandName;
    stream >> commandName;
    for (char& ch : commandName) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return commandName;
}

//Get status information to send to jarvis service layer
StatusInfo Engine::getStatusInfo() const {
    const auto now = std::chrono::steady_clock::now();
    const auto secondsElapsed = std::chrono::duration_cast<std::chrono::seconds>(now - startTime).count();

    StatusInfo info;
    info.running = running;
    info.uptimeSeconds = secondsElapsed;
    info.lastCommand = lastCommand;
    return info;
}
```

- [ ] **Step 5: Update `core/main.cpp`**

Replace the full contents of `core/main.cpp` with:

```cpp
#include <iostream>
#include <string>
#include "capability_registry.h"
#include "engine.h"

int main() {
    Engine engine;
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    engine.run(registry);
    
    return 0;
}
```

- [ ] **Step 6: Add `core/capability_registry.cpp` to the `jarvis` CMake target**

In `CMakeLists.txt`, find the `add_executable(jarvis ...)` block and add
`core/capability_registry.cpp` to its source list, so it reads:

```cmake
add_executable(
    jarvis
    core/main.cpp
    core/command_handler.cpp
    core/engine.cpp
    core/capability_registry.cpp
)
```

- [ ] **Step 7: Build and verify**

Run:
```bash
cd "/home/rayyan/Programming/Remote Repos/Public/JARVIS"
cmake --build build -j4
```
Expected: `jarvis`, `jarvis_grpc_server` (unaffected by this task, still builds — Task 6 wires
it), and `jarvis_tests` all build cleanly.

Run: `./build/jarvis_tests`
Expected: all 15 tests still PASS (this task didn't touch capability logic, only how the CLI
calls it).

Then manually verify the CLI end-to-end — run `./build/jarvis` and type each of, checking the
output matches what's described:

```
status          -> "Engine: running", "Uptime: 00:00:0X", "Last command: none"
echo hello      -> "hello"
about           -> "JARVIS Core Engine v1.0\nDeveloped by Rayyan."
help            -> lists echo/about/status/help (NOT unknown) and exit
help echo       -> "echo: Echoes the input back to the user. Usage: echo [text]"
bananas         -> "Command not recognised. Please try again."
exit            -> "Terminating JARVIS Core Engine..." and the program exits
```

- [ ] **Step 8: Commit**

```bash
git add core/command_handler.h core/command_handler.cpp core/engine.h core/engine.cpp core/main.cpp CMakeLists.txt
git commit -m "refactor(core): wire the CLI through the CapabilityRegistry"
```

---

### Task 6: Wire the gRPC service (`core/jarvis_service`, `core/grpc_server_main.cpp`) to the registry

**Files:**
- Modify: `core/jarvis_service.h`
- Modify: `core/jarvis_service.cpp`
- Modify: `core/grpc_server_main.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `CapabilityRegistry::dispatch()` (Task 1), `registerBuiltinCapabilities()` (Task 4).
- Produces: `JarvisServiceImpl`'s constructor gains a third parameter,
  `JarvisServiceImpl(Engine&, JarvisAIClient&, CapabilityRegistry&)` — a signature change from
  today's two-argument constructor. Nothing else depends on this beyond `core/grpc_server_main.cpp`
  (updated in this same task).

- [ ] **Step 1: Update `core/jarvis_service.h`**

Replace the full contents of `core/jarvis_service.h` with:

```cpp
#pragma once

#include "../generated/cpp/jarvis.grpc.pb.h"
#include "ai_client.h"
#include "capability_registry.h"
#include "command_handler.h"
#include "engine.h"
#include <grpcpp/grpcpp.h>

// JarvisServiceImpl is the server-side implementation of the gRPC JarvisService.
// It inherits from the generated JarvisService::Service base class.
// This class acts as the adapter between gRPC transport layer and internal engine logic.
class JarvisServiceImpl final : public jarvis::v1::JarvisService::Service {
 public:
  JarvisServiceImpl(Engine& engine, JarvisAIClient& aiClient, CapabilityRegistry& registry);
  virtual ~JarvisServiceImpl();

  // Override the ProcessCommand RPC method from the generated service interface.
  // This method is invoked by gRPC when a client sends a ProcessCommand request.
  // Parameters:
  //   - context: gRPC server context (contains metadata about the RPC call)
  //   - request: the incoming request message from the proto definition
  //   - response: the response message we must fill before returning
  // Return:
  //   - grpc::Status: indicates success/failure of the RPC transport level
  ::grpc::Status ProcessCommand(
      ::grpc::ServerContext* context,
      const ::jarvis::v1::ExecuteCommandRequest* request,
      ::jarvis::v1::ExecuteCommandResponse* response) override;

 private:
  Engine& engine_;
  JarvisAIClient& aiClient_;
  CapabilityRegistry& registry_;

  // Helper method to convert proto CommandType enum to internal CommandType enum.
  // Why: proto enums and internal enums are separate.
  // We must translate between them at the adapter boundary.
  CommandType protoCommandToInternal(jarvis::v1::CommandType protoCmd);

  // Helper method to convert internal CommandType enum to proto CommandType enum.
  // Why: we need to send back a normalized command type to the client.
  jarvis::v1::CommandType internalCommandToProto(CommandType internalCmd);

  // Helper method to convert internal error/result into a proto ErrorCode enum.
  // Why: error classification should be transport-agnostic on the proto side.
  jarvis::v1::ErrorCode resultToProtoErrorCode(bool success);

  // Helper method to map an AI-classified intent string (e.g. "STATUS") to the
  // internal CommandType it corresponds to. Returns CommandType::UNKNOWN for
  // "UNKNOWN" or any unrecognised intent string.
  CommandType intentToCommandType(const std::string& intent);
};
```

- [ ] **Step 2: Update `core/jarvis_service.cpp`**

Replace the full contents of `core/jarvis_service.cpp` with:

```cpp
#include "jarvis_service.h"

#include <iomanip>
#include <optional>
#include <sstream>
#include <spdlog/spdlog.h>

// Constructor — takes the Engine (for STATUS), the AI client (for UNKNOWN commands), and the
// CapabilityRegistry (for dispatching every known command, including a classified UNKNOWN).
JarvisServiceImpl::JarvisServiceImpl(Engine& engine, JarvisAIClient& aiClient, CapabilityRegistry& registry)
    : engine_(engine), aiClient_(aiClient), registry_(registry) {
}

// Destructor
JarvisServiceImpl::~JarvisServiceImpl() {
  // Clean up any resources if needed.
}

// Core RPC method: ProcessCommand
// This is the entry point when a client (like Python) sends a command over gRPC.
::grpc::Status JarvisServiceImpl::ProcessCommand(
    ::grpc::ServerContext* context,
    const ::jarvis::v1::ExecuteCommandRequest* request,
    ::jarvis::v1::ExecuteCommandResponse* response) {
  
  // Step 1: Validate — reject unspecified command type before doing any work.
  if (request->command() == jarvis::v1::COMMAND_TYPE_UNSPECIFIED) {
    spdlog::warn("Rejected request: COMMAND_TYPE_UNSPECIFIED");
    response->set_success(false);
    response->set_message("Invalid request: command type must be specified.");
    response->set_command_type(jarvis::v1::COMMAND_TYPE_UNSPECIFIED);
    response->set_error_code(jarvis::v1::ERROR_CODE_INVALID_COMMAND);
    return ::grpc::Status::OK;
  }

  // Step 2: Translate proto enum → internal enum and extract payload.
  CommandType internalCmd = protoCommandToInternal(request->command());
  std::string payload = request->payload();

  spdlog::info("ProcessCommand: command={} payload='{}'",
      jarvis::v1::CommandType_Name(request->command()), payload);

  // Step 3: Dispatch through the registry. For UNKNOWN this always returns std::nullopt
  // (UNKNOWN is never registered) — output starts empty and gets replaced below regardless.
  ExecutionContext execContext{engine_, registry_};
  std::string output = registry_.dispatch(internalCmd, payload, execContext).value_or("");

  // Step 4: UNKNOWN commands are forwarded to the Python AI server. If the AI
  // classifies the text into a known intent with enough confidence, re-dispatch
  // as that command instead of just echoing the AI's reply back.
  if (internalCmd == CommandType::UNKNOWN) {
    spdlog::info("ProcessCommand: unrecognised command, forwarding to AI layer");
    AIResult aiResult = aiClient_.ProcessNaturalLanguage(payload);

    constexpr float kConfidenceThreshold = 0.5f;
    CommandType classifiedCmd = intentToCommandType(aiResult.intent);

    if (aiResult.success && classifiedCmd != CommandType::UNKNOWN &&
        aiResult.confidence >= kConfidenceThreshold) {
      spdlog::info("ProcessCommand: AI classified intent={} confidence={:.2f}, re-dispatching",
          aiResult.intent, aiResult.confidence);
      internalCmd = classifiedCmd;
      output = registry_.dispatch(classifiedCmd, payload, execContext).value_or("");
    } else {
      output = aiResult.reply;
    }
  }

  // Success if we got a non-empty reply (even AI errors return a descriptive string).
  const bool success = (internalCmd != CommandType::UNKNOWN) || !output.empty();

  spdlog::info("ProcessCommand: {}", success ? "OK" : "FAIL");

  // Step 5: Fill the response.
  response->set_success(success);
  response->set_message(output);
  response->set_command_type(internalCommandToProto(internalCmd));
  response->set_error_code(resultToProtoErrorCode(success));

  return ::grpc::Status::OK;
}

// Helper: Convert proto CommandType to internal CommandType.
CommandType JarvisServiceImpl::protoCommandToInternal(jarvis::v1::CommandType protoCmd) {
  switch (protoCmd) {
    case jarvis::v1::COMMAND_TYPE_ECHO:
      return CommandType::ECHO;
    case jarvis::v1::COMMAND_TYPE_UNKNOWN:
      return CommandType::UNKNOWN;
    case jarvis::v1::COMMAND_TYPE_EXIT:
      return CommandType::EXIT;
    case jarvis::v1::COMMAND_TYPE_HELP:
      return CommandType::HELP;
    case jarvis::v1::COMMAND_TYPE_ABOUT:
      return CommandType::ABOUT;
    case jarvis::v1::COMMAND_TYPE_STATUS:
      return CommandType::STATUS;
    case jarvis::v1::COMMAND_TYPE_UNSPECIFIED:
    default:
      return CommandType::UNKNOWN;
  }
}

// Helper: Convert internal CommandType to proto CommandType.
jarvis::v1::CommandType JarvisServiceImpl::internalCommandToProto(CommandType internalCmd) {
  switch (internalCmd) {
    case CommandType::ECHO:
      return jarvis::v1::COMMAND_TYPE_ECHO;
    case CommandType::UNKNOWN:
      return jarvis::v1::COMMAND_TYPE_UNKNOWN;
    case CommandType::EXIT:
      return jarvis::v1::COMMAND_TYPE_EXIT;
    case CommandType::HELP:
      return jarvis::v1::COMMAND_TYPE_HELP;
    case CommandType::ABOUT:
      return jarvis::v1::COMMAND_TYPE_ABOUT;
    case CommandType::STATUS:
      return jarvis::v1::COMMAND_TYPE_STATUS;
    default:
      return jarvis::v1::COMMAND_TYPE_UNSPECIFIED;
  }
}

// Helper: Convert an AI-classified intent string to internal CommandType.
CommandType JarvisServiceImpl::intentToCommandType(const std::string& intent) {
  if (intent == "STATUS") return CommandType::STATUS;
  if (intent == "ECHO") return CommandType::ECHO;
  if (intent == "ABOUT") return CommandType::ABOUT;
  return CommandType::UNKNOWN;
}

// Helper: Convert result bool to proto ErrorCode enum.
jarvis::v1::ErrorCode JarvisServiceImpl::resultToProtoErrorCode(bool success) {
  if (success) {
    return jarvis::v1::ERROR_CODE_NONE;
  } else {
    return jarvis::v1::ERROR_CODE_EXECUTION_FAILED;
  }
}
```

(The `STATUS` special-case block that used to pull `StatusInfo` from `engine_` directly is
gone — the `status` capability from Task 3 does that now, reached via `ExecutionContext`, so
that formatting logic exists in exactly one place instead of two.)

- [ ] **Step 3: Update `core/grpc_server_main.cpp`**

Replace the full contents of `core/grpc_server_main.cpp` with:

```cpp
#include "ai_client.h"
#include "capability_registry.h"
#include "engine.h"
#include "jarvis_service.h"

#include <grpcpp/grpcpp.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <string>

int main() {
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%H:%M:%S] [%^%l%$] %v");

    const std::string serverAddress = "0.0.0.0:50051";
    Engine engine;

    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    // Connect to the Python AI server. The channel is lazy — no error if Python isn't up yet.
    JarvisAIClient aiClient(grpc::CreateChannel("localhost:50052", grpc::InsecureChannelCredentials()));

    JarvisServiceImpl service(engine, aiClient, registry);

    grpc::ServerBuilder builder;
    builder.AddListeningPort(serverAddress, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    if (!server) {
        spdlog::error("Failed to start gRPC server on {}", serverAddress);
        return 1;
    }

    spdlog::info("JARVIS gRPC server listening on {}", serverAddress);
    server->Wait();
    return 0;
}
```

- [ ] **Step 4: Add `core/capability_registry.cpp` to the `jarvis_grpc_server` CMake target**

In `CMakeLists.txt`, find the `add_executable(jarvis_grpc_server ...)` block and add
`core/capability_registry.cpp` to its source list, so it reads:

```cmake
add_executable(
    jarvis_grpc_server
    core/grpc_server_main.cpp
    core/command_handler.cpp
    core/engine.cpp
    core/capability_registry.cpp
    core/ai_client.cpp
    core/jarvis_service.cpp
)
```

- [ ] **Step 5: Build and verify**

Run:
```bash
cd "/home/rayyan/Programming/Remote Repos/Public/JARVIS"
cmake --build build -j4
./build/jarvis_tests
```
Expected: `jarvis`, `jarvis_grpc_server`, and `jarvis_tests` all build cleanly; all 15 tests in
`jarvis_tests` still PASS.

Then verify the gRPC path end-to-end. Regenerate Python stubs if `generated/python/` is stale
(check `ls generated/python/jarvis_pb2.py` exists first), then:

```bash
cd "/home/rayyan/Programming/Remote Repos/Public/JARVIS"
.venv/bin/python ai/jarvis_ai_server.py > /tmp/ai.log 2>&1 &
sleep 1
./build/jarvis_grpc_server > /tmp/grpc.log 2>&1 &
sleep 1
.venv/bin/python tools/grpc_smoke_test.py
pkill -f jarvis_ai_server.py
pkill -f jarvis_grpc_server
```

Expected: every case in `tools/grpc_smoke_test.py` behaves exactly as before this plan —
known commands (ECHO/HELP/STATUS) succeed with the same `command_type`/`message` shape,
classified natural-language cases (STATUS/ECHO/ABOUT) still re-dispatch correctly, the
unclassified case stays `COMMAND_TYPE_UNKNOWN`, and the LLM-only case still degrades
gracefully if Ollama can't answer in time (unrelated to this plan, unaffected either way).

- [ ] **Step 6: Commit**

```bash
git add core/jarvis_service.h core/jarvis_service.cpp core/grpc_server_main.cpp CMakeLists.txt
git commit -m "refactor(core): wire the gRPC service through the CapabilityRegistry"
```

---

## Self-Review Notes

- **Spec coverage:** §3 (Capability contract) → Task 1; §4 (CapabilityRegistry) → Task 1; §5
  (each builtin's fate — echo/about/status/help/exit/unknown) → Tasks 2, 3, 4 for the four real
  capabilities, exit/unknown handling threaded through Tasks 1 (registry never registers
  UNKNOWN), 4 (help's exit line), 5 (CLI's exit stays special) and 6 (gRPC's UNKNOWN handling
  unchanged); §6 (call-site changes) → Tasks 5, 6; §7 (file layout) → all tasks, matches exactly;
  §8 (testing plan) → Task 1 (mechanism), 2–4 (each builtin), covers every bullet listed there
  (register/resolve, nullptr for unregistered/UNKNOWN specifically, dispatch invokes execute,
  four builtins with correct name/intent/tier, status reflects live Engine state, help lists all
  four without hardcoding + excludes unknown, help "\<command\>" lookup + not-found case); §9
  (what doesn't change) → confirmed no task touches `proto/`, `ai/`, or the confidence threshold;
  §10 (deferred items) → not built, confirmed nothing in any task attempts dynamic loading,
  config, or gate enforcement.
- **Placeholder scan:** none found — every step has real code, real commands, real expected
  output/test counts.
- **Type consistency:** `Capability`/`PowerTier`/`ExecutionContext`/`CapabilityRegistry` defined
  in Task 1 are used identically (same field names, same method signatures) in every later task.
  `registerBuiltinCapabilities(CapabilityRegistry&)` keeps the same signature from its Task 1
  declaration through its Task 4 final body. `Engine::run()`'s signature change
  (`run()` → `run(CapabilityRegistry&)`) is introduced and consumed within the same task (5) —
  no other task calls it. `JarvisServiceImpl`'s constructor signature change is introduced and
  consumed within the same task (6).
