# Capability Registry & Plugin Substrate (S2) — Design

**Status:** Approved by user in brainstorming session, 2026-08-30. Ready for implementation planning.
**Blueprint stage:** S2 — Capability Registry & Plugin Substrate (`docs/architecture-blueprint.md` §5,
the "keystone stage").
**Governing invariants:** INV-1 (transport-agnostic core), INV-4 (adapters at boundaries),
INV-6 (register, don't hardcode), INV-9 (consent scales with power — declared here, enforced
later), INV-10 (build from primitives), INV-13 (every seam is verifiable).
**Roadmap link:** `docs/features.md` Phase 4 ("The plugin manager is the prerequisite for
everything else in this phase"); `docs/architecture-blueprint.md` §6 flags S2 as architecturally
prior to further capability growth even though numbered later than Voice (Phase 3).

---

## 1. Scope

This design replaces `core/command_handler.cpp`'s two hardcoded, hand-maintained maps
(`COMMAND_DISPATCH`, `COMMAND_DESCRIPTIONS`) with a **Capability Registry**: capabilities
declare themselves (name, the intent they answer to, a description, a power tier, and their
execution logic) and register with the registry at startup. Both the CLI (`core/engine.cpp`)
and the gRPC service (`core/jarvis_service.cpp`) dispatch through the same registry instead of
each independently calling `runCMD()` — closing the gap where two dispatch paths existed for
one set of commands.

**In scope for this pass:**
- The `Capability` contract (the "business card" each capability carries).
- The `CapabilityRegistry` (register / resolve / dispatch).
- Re-expressing `echo`, `status`, `about`, `help` as real capabilities.
- An `ExecutionContext` so `status` (needs `Engine`) and `help` (needs the registry itself) can
  reach what they need, while capabilities that don't need it (like `echo`) simply ignore it.
- Explicit, compile-time self-registration (one line per capability, in one obvious place) —
  not dynamic loading.
- A GoogleTest-based C++ test suite for the registry (the project's first C++ tests).

**Explicitly out of scope, deferred to later passes (with the reasoning already agreed):**
- **Dynamic/runtime plugin loading** (`.so` files, hot-reload, discovery without rebuilding).
  No third-party plugins exist yet to justify the real complexity (ABI stability, symbol
  resolution, safe unload) — build it against a real need, not a hypothetical one.
- **A configuration system** (per-capability enable/disable, runtime-reloadable settings).
  Capabilities register themselves in code for now; config is a separate, later piece
  (`docs/architecture-blueprint.md` §4.2).
- **Actual consent-gate enforcement** (blocking execution, confirmation prompts for T2+
  capabilities). Every capability today is T0 (read-only) — there is nothing above T0 to build
  and test the gate against yet. The registry *declares* each capability's power tier now
  (INV-9's registration half); the *enforcement* half is built later against a real T1+
  capability (matches `docs/features.md`'s own note that safety guardrails need "a plugin that
  can actually do something destructive" first).

## 2. The problem, concretely

Today, `core/command_handler.cpp` holds three separate hand-synchronised maps:
`COMMAND_MAP` (string → `CommandType`), `COMMAND_DISPATCH` (`CommandType` → execution function),
`COMMAND_DESCRIPTIONS` (`CommandType` → help text). Adding a capability means touching all
three, plus (per INV-6) the orchestrator is only supposed to resolve intent → capability through
a registry, not hardcode knowledge of specific capabilities — today it does the latter by
construction, since `runCMD()` *is* the hardcoded map lookup. `runHelp()` further hand-builds its
listing by iterating `COMMAND_MAP` and cross-referencing `COMMAND_DESCRIPTIONS` — three sources
of truth for what is, conceptually, one piece of information per capability.

## 3. The `Capability` contract

A capability is a small, self-contained bundle of data plus one function — a "business card,"
not just a bare function pointer:

```cpp
// core/capability.h
#pragma once
#include <functional>
#include <string>
#include "command_handler.h"  // CommandType

enum class PowerTier { T0_READ_ONLY, T1_STATEFUL_LOCAL, T2_SYSTEM_AFFECTING,
                        T3_DESTRUCTIVE, T4_EXTERNAL };

class Engine;
class CapabilityRegistry;

// What a capability's execute() function can reach beyond its own payload. Most capabilities
// (e.g. echo) ignore this entirely; status reads engine, help reads registry.
struct ExecutionContext {
    Engine& engine;
    const CapabilityRegistry& registry;
};

struct Capability {
    std::string name;                // e.g. "echo" — matches today's COMMAND_MAP key
    CommandType intent;               // the CommandType this capability answers to
    std::string description;          // shown by `help`
    PowerTier powerTier;              // declared now; gate enforcement comes later (§1)
    std::function<std::string(const std::string& payload, ExecutionContext& context)> execute;
};
```

Reusing the existing `CommandType` enum (rather than introducing a parallel "intent" type) keeps
this change additive — nothing about how intents are named or how the AI layer classifies them
changes; only *how a resolved intent finds its implementation* changes.

## 4. The `CapabilityRegistry`

```cpp
// core/capability_registry.h
#pragma once
#include <optional>
#include <unordered_map>
#include "capability.h"

class CapabilityRegistry {
public:
    void registerCapability(Capability capability);

    // Returns nullptr if no capability answers this intent (including CommandType::UNKNOWN,
    // which is never registered — "no match" stays a real, distinct outcome, not a capability).
    const Capability* resolve(CommandType intent) const;

    // Convenience: resolve() + execute() in one call. Returns std::nullopt if resolve() would
    // have returned nullptr, so callers can tell "ran, returned this" apart from "nothing to run".
    std::optional<std::string> dispatch(CommandType intent, const std::string& payload,
                                         ExecutionContext& context) const;

    // For `help` to enumerate what's registered, and for tests.
    const std::unordered_map<CommandType, Capability>& all() const;

private:
    std::unordered_map<CommandType, Capability> capabilities_;
};

// One explicit call site registers every built-in capability. Adding a new one means writing
// it (§5) and adding one line here — nothing else in the dispatch path changes.
void registerBuiltinCapabilities(CapabilityRegistry& registry);
```

`registerBuiltinCapabilities()` lives in `core/capability_registry.cpp` and is called once, at
startup, from both `core/main.cpp` (CLI) and `core/grpc_server_main.cpp` (gRPC server) — each
process builds its own `CapabilityRegistry` instance the same way it already builds its own
`Engine`.

**Why explicit registration, not a clever automatic C++ registration trick** (e.g. static
objects whose constructors call `registerCapability()` before `main()` runs): explicit is
easier to read, step through, and explain — worth more here than saving four lines, especially
given INV-10's "every tier must teach" and this being a project whose implementation has to be
defensible at a viva.

## 5. What happens to each existing builtin

| Command | Becomes | Notes |
| --- | --- | --- |
| `echo` | A real capability | Payload in, payload out. Ignores `ExecutionContext`. |
| `about` | A real capability | Static text. Ignores `ExecutionContext`. |
| `status` | A real capability | Reads `context.engine.getStatusInfo()` — this is *why* `ExecutionContext` exists; today's special-cased "STATUS is handled in the Engine layer" comment goes away, replaced by a capability that genuinely does the work. |
| `help` | A real capability | Reads `context.registry.all()` to build its listing dynamically — replaces the hand-maintained `COMMAND_DESCRIPTIONS` cross-reference. **Behaviour change, intentional:** the listing no longer includes an `unknown` entry (it was never a real user-invokable command) — `exit` is still listed, appended as one hardcoded line, since it stays outside the registry but is still a real command a user can type (see next row). |
| `exit` | **Stays outside the registry entirely** | Not a capability — it's engine-lifecycle control ("turn the program off"), not a thing JARVIS *does for you*. Handled exactly as today: only `core/engine.cpp`'s CLI loop reacts to it; confirmed today that sending it over gRPC does *not* terminate the server (`core/jarvis_service.cpp` has no special case for it) — that behaviour is unchanged. |
| `unknown` (no match) | **Stays a distinct outcome, not a capability** | `registry.resolve(CommandType::UNKNOWN)` always returns `nullptr` — `UNKNOWN` is never registered. Callers (both `engine.cpp` and `jarvis_service.cpp`) keep their existing "nothing matched" handling (print "not recognised" / forward to the AI layer) exactly as today, just gated on the registry returning nothing rather than the old map missing an entry. |

## 6. Call-site changes

- `core/engine.cpp`'s `Engine::run()`: replace the call into `handleCommand()`/`runCMD()` with
  `registry.dispatch(cmdType, payload, context)`; on `std::nullopt`, keep today's "not
  recognised" behaviour. `exit` detection stays exactly as today (checked before/around
  dispatch, not through the registry).
- `core/jarvis_service.cpp`'s `ProcessCommand()`: both the direct-command path (Step 3) and the
  AI re-dispatch path (the `UNKNOWN` branch, added last session) call `registry.dispatch(...)`
  instead of `runCMD(...)`. The `STATUS` special-case block that currently pulls `StatusInfo`
  from `Engine` directly is deleted — the new `status` capability does that internally via
  `ExecutionContext`, so there's exactly one place that logic lives instead of two (today it's
  duplicated between `command_handler.cpp`'s blank `STATUS` lambda + `jarvis_service.cpp`'s
  special-case block).
- `core/command_handler.h/.cpp`: `COMMAND_DISPATCH` and `COMMAND_DESCRIPTIONS` are deleted.
  `COMMAND_MAP` (string → `CommandType`) stays — parsing "what word did the user type" is a
  distinct concern from "what runs for that type," and nothing about parsing changes here.
  `runEcho`, `runAbout`, `runHelp`, `runUnknown` move into the new capability implementations
  (§5); `runHelp`'s body is rewritten per the registry-driven behaviour above.

## 7. File layout

- **Create** `core/capability.h` — the `Capability` struct, `PowerTier` enum, `ExecutionContext`.
- **Create** `core/capability_registry.h` — the `CapabilityRegistry` class declaration.
- **Create** `core/capability_registry.cpp` — the class implementation plus
  `registerBuiltinCapabilities()` and the four builtin capability implementations (or one file
  each if that reads better once written — a call the implementation plan can make; start as one
  file, split only if it grows unwieldy, per the project's own file-size guidance).
- **Modify** `core/command_handler.h/.cpp` — remove `COMMAND_DISPATCH`, `COMMAND_DESCRIPTIONS`,
  and the four `run*` function bodies that move into capabilities; `COMMAND_MAP` and the parsing
  functions (`parseCommand`, `extractCommandType`, `extractPayload`, `toLower`) stay.
- **Modify** `core/engine.h/.cpp` — `Engine::run()` takes/uses a `CapabilityRegistry&`.
- **Modify** `core/jarvis_service.h/.cpp` — `JarvisServiceImpl` takes a `CapabilityRegistry&`
  alongside its existing `Engine&` and `JarvisAIClient&`; `ProcessCommand()` dispatches through
  it as described in §6.
- **Modify** `core/main.cpp`, `core/grpc_server_main.cpp` — construct a `CapabilityRegistry`,
  call `registerBuiltinCapabilities()`, pass it to `Engine`/`JarvisServiceImpl`.
- **Create** `CMakeLists.txt` changes — `find_package(GTest REQUIRED)` (already installed
  system-wide, confirmed via `pkg-config --exists gtest`), a new test source directory (e.g.
  `tests/`), and a new CTest-registered executable target that links the registry code plus
  GTest — built separately from `jarvis` and `jarvis_grpc_server`, doesn't affect either.
- **Create** `tests/capability_registry_test.cpp` — see §8.

## 8. Testing plan

The project's first C++ tests, using GoogleTest (already installed, `find_package(GTest)` in
CMake). Covers the registry mechanism itself:

- Registering a capability, then resolving its intent returns it.
- Resolving an intent nothing is registered for (including `CommandType::UNKNOWN` specifically)
  returns `nullptr` / `dispatch()` returns `std::nullopt`.
- `dispatch()` actually invokes the capability's `execute()` and returns its output.
- `registerBuiltinCapabilities()` registers exactly the four expected capabilities
  (`echo`/`status`/`about`/`help`), each with the right `name`/`intent`/`powerTier` (all `T0`).
- The `status` capability's output reflects live `Engine` state (via a real or minimal test
  `Engine`) — proves `ExecutionContext` threading works, not just that the function is callable.
- The `help` capability's "list everything" output enumerates all four builtins without any of
  them being named in `help`'s own implementation (the actual point of this whole change) —
  and does **not** list `unknown`.
- The `help` capability's "help \<command\>" path still finds a specific capability by name and
  still reports "not found" for something unregistered.

Existing verification stays as-is and must still pass unchanged after this refactor (this is a
mechanism change, not a behaviour change, for everything except the two documented exceptions in
§5): `tools/grpc_smoke_test.py`, the full `pytest ai/ tools/` suite (untouched by this change,
but confirms nothing on the Python side broke), and manual CLI checks for `echo`/`status`/
`about`/`help`/`help echo`/`exit`/an unknown command.

## 9. What doesn't change

- `proto/jarvis.proto`, `proto/ai.proto` — no field or message changes.
- `ai/` — nothing. The Understanding tier still emits an intent + confidence; it has no idea the
  registry exists (per the blueprint: S2 "leaves the understanding tier... untouched").
- Any client/surface — the CLI and gRPC client contracts behave identically except the two
  documented `help` output changes in §5.
- The 0.5 confidence re-dispatch threshold in `jarvis_service.cpp`'s AI-escalation path —
  unaffected; it still decides *whether* to re-dispatch, this design only changes *how* a
  re-dispatch (or a direct command) finds its implementation once the decision is made.

## 10. Deliberately deferred (not built in this design)

- **Dynamic plugin loading** — `.so`/shared-library capabilities discovered and loaded at
  runtime without rebuilding. The registry's dispatch mechanism is now fully separated from
  command-wiring concerns (INV-6's specific target), but building actual runtime-loaded plugins
  requires more than just wiring `registerCapability()` calls. Today's identifier space (the
  compile-time `CommandType` enum) cannot be extended by a runtime-loaded `.so` — a new
  capability requires a new enumerator, a `COMMAND_MAP` entry, and a proto enum value, all
  compile-time changes. Genuine runtime discovery and loading would require making this
  identifier space extensible at runtime (e.g., string-keyed intents instead of enumerators),
  which is a substantial, not-yet-done piece of future work, not merely implementation detail
  wiring.
- **Configuration-driven enable/disable** — per `docs/architecture-blueprint.md` §4.2, tracked
  as a separate future piece.
- **Consent-gate enforcement** — the orchestrator reading `powerTier` and actually blocking/
  confirming for T2+. Declared now, enforced later against a real T1+ capability.
