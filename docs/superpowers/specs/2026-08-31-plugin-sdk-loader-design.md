# Plugin SDK & Dynamic Loader — Design Spec

**Status:** approved (user chose "full plan, this is the new direction" for the Codex Phase 4
plan on 2026-08-31 — supersedes the prior session's "framework only, no plugins yet" scope).
**Phase:** Phase 4 — Runtime Plugins, Desktop Control & Integrations. Foundation piece: the
Codex plan's own "Assumptions" section states runtime plugin loading must land before
additional plugin families, since every later capability (file search, notes, system control,
desktop, media, calendar) is meant to ship as a loadable `.so`, not a compiled-in builtin.

## 1. Scope

**In scope:**
- A versioned, minimal C ABI a `.so` plugin implements to register one or more capabilities
  with the host, without exposing unstable C++ internals (name mangling, STL ABI, exceptions
  across the DSO boundary) across the loader boundary.
- A manifest format (one JSON file per plugin) the loader validates *before* ever calling
  `dlopen` on the plugin's library — malformed manifests, ABI mismatches, and duplicate
  intents are all rejected pre-load.
- A `PluginLoader` that discovers manifests from configured trusted local directories, loads
  valid plugins, registers their capabilities into the existing `CapabilityRegistry` through
  the string-keyed `resolve()`/`dispatch()` path added in the prior commit, and supports
  disabling (stop dispatching, keep the library mapped) and safe unloading (`dlclose`, only
  when no invocation of that plugin is currently in flight).
- Converting `system-info` from a compiled-in builtin into the bundled reference dynamic
  plugin — proving discover → load → dispatch → disable → unload end-to-end with a capability
  that already has full test coverage from the previous commit.
- A minimal, purpose-built config surface for plugin directories (same hand-rolled
  `key=value` discipline as `PluginConfig` — INV-10, no JSON/YAML C++ library for the config
  file itself). The plugin *manifest* format is JSON (§4) because manifests are structured and
  potentially third-party-authored, but the JSON itself is parsed by a small **hand-rolled**
  parser scoped to exactly the bounded manifest schema (objects, arrays, strings, numbers,
  booleans — no need for the general JSON grammar), not a third-party library: no JSON/XML
  library is currently installed on this machine (`nlohmann-json` is available via `pacman`
  but not installed, and pulling it via CMake `FetchContent` means a network fetch at
  configure time), and per INV-10 a hand-rolled parser for a schema this small and fixed is
  consistent with how `PluginConfig` and `command_handler.cpp` already parse their own formats
  in this codebase — this is also literally one of the concepts JARVIS exists to teach
  (project `CLAUDE.md` §0: "parsers... schema versioning").

**Out of scope (deliberately, per the Codex plan's own phasing and existing project
precedent):**
- Plugin marketplace distribution, code signing, or sandboxing (Codex plan's own
  "Assumptions": trusted local shared libraries only, selected by the user).
- Any capability beyond `system-info` shipping as a plugin in this pass — file search,
  notes/reminders, system control, desktop/browser, media, and calendar are separate,
  later stages per the Codex plan's capability delivery order.
- T3/T4 real enforcement (unchanged from the existing `ConsentGate` — still honestly denied).
- Windows/macOS loader backends — Linux-first (`dlfcn.h`), per the Codex plan's assumptions.
- Hot-reload / auto-discovery on file-system changes — loading happens once, at startup.

## 2. Where this sits in the pipeline

This deepens **Orchestrate**, same as the Plugin Manager substrate before it. "Plugin" and
"capability" remain the same concept (`Capability` struct); the loader is a new *source* that
populates the same `CapabilityRegistry` a compiled-in `registerBuiltinCapabilities()` call
already populates. Downstream — dispatch, the consent gate, enable/disable — needs zero
changes: a dynamically-loaded capability flows through `CapabilityRegistry::dispatch()`
exactly like a compiled-in one, because both are just `Capability` values in the same map.

## 3. The plugin ABI

A plugin is a `.so` exporting exactly two `extern "C"` symbols. No C++ types cross the
boundary — this avoids STL-ABI/exception/RTTI mismatches between the host's compiler/library
version and a plugin possibly built separately, which is the single most common way native
plugin ABIs break in practice.

```c
// jarvis_plugin_abi.h — the ONLY header a plugin author includes. Pure C, no C++ types.
#define JARVIS_PLUGIN_ABI_VERSION 1

typedef enum {
    JARVIS_POWER_TIER_T0_READ_ONLY = 0,
    JARVIS_POWER_TIER_T1_STATEFUL_LOCAL = 1,
    JARVIS_POWER_TIER_T2_SYSTEM_AFFECTING = 2,
    JARVIS_POWER_TIER_T3_DESTRUCTIVE = 3,
    JARVIS_POWER_TIER_T4_EXTERNAL = 4,
} JarvisPowerTier;

// A capability's execute function. payload is a NUL-terminated UTF-8 string owned by the
// host (read-only, valid only for the duration of the call). The plugin returns a
// NUL-terminated UTF-8 string it allocated with malloc(); the host frees it with free()
// after copying it — malloc/free is the one allocator both sides can agree on across a
// dlopen boundary without a shared allocator library, unlike `new`/`delete` or std::string.
typedef char* (*JarvisCapabilityFn)(const char* payload);

typedef struct {
    // registerCapability: called by the plugin's jarvis_plugin_register(), once per
    // capability, during plugin load. Returns 1 on success, 0 if the host rejected it
    // (e.g. this exact intent name is already registered by another plugin).
    int (*registerCapability)(
        void* host_context,
        const char* intent_name,
        const char* description,
        JarvisPowerTier power_tier,
        JarvisCapabilityFn execute);
} JarvisPluginHost;

// Every plugin exports both of these with these exact names (checked via dlsym):

// Returns JARVIS_PLUGIN_ABI_VERSION the plugin was built against. The loader refuses to
// call jarvis_plugin_register() at all if this doesn't match the host's own
// JARVIS_PLUGIN_ABI_VERSION — an ABI mismatch is refused outright, never "best-effort" loaded.
// extern "C" int jarvis_plugin_abi_version(void);

// Called once at load time with a host_context opaque pointer (passed back into every
// registerCapability call so the host can identify which plugin is registering) and the
// JarvisPluginHost vtable. Returns 1 if registration succeeded, 0 on failure (loader then
// refuses to activate this plugin and dlcloses it immediately).
// extern "C" int jarvis_plugin_register(void* host_context, const JarvisPluginHost* host);
```

**Why not pass `ExecutionContext&` (engine/registry access) into plugins:** the reference
`system-info` plugin needs neither, and per INV-1 a capability already only needs its payload
— engine-state access is the exception (`status`), not the rule. A later plugin family that
genuinely needs engine access (rare) gets a deliberate, separately-designed ABI extension, not
a speculative one added now with nothing to test it against (YAGNI).

**Why `intent_name` only, no `CommandType`:** a dynamically loaded plugin cannot add a new
enumerator to the compile-time `CommandType` enum without a rebuild — that's exactly the
limitation the string-keyed `resolve()`/`dispatch()` path (already landed) exists to remove.
Every plugin-registered capability is intent-only; it has no `CommandType` and is never
reachable through the enum-keyed `dispatch()` overload, only the string-keyed one.

## 4. Manifest format

One `manifest.json` per plugin, colocated with its `.so` in a directory the loader scans:

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

**Parser:** `core/minimal_json.h/.cpp` — a small hand-rolled recursive-descent parser (see the
"In scope" note in §1 for why hand-rolled) producing one variant type:

```cpp
// core/minimal_json.h
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

enum class JsonType { Null, Boolean, Number, String, Array, Object };

class JsonValue {
 public:
    JsonType type = JsonType::Null;
    bool boolValue = false;
    double numberValue = 0.0;
    std::string stringValue;
    std::vector<JsonValue> arrayValue;
    std::map<std::string, JsonValue> objectValue;

    // Convenience accessors used by the manifest loader — return a sensible zero-value
    // (empty string / null JsonValue) rather than throwing when a key is absent or the
    // wrong type, so manifest validation can check "was this field present and a string"
    // explicitly instead of catching exceptions.
    const JsonValue* find(const std::string& key) const;   // Object lookup; nullptr if absent/not-object
    std::string asString(const std::string& fallback = "") const;
    bool isString() const;
    bool isArray() const;
    bool isObject() const;
};

// Returns std::nullopt on any parse error (malformed JSON) — the manifest loader treats a
// parse failure as "reject this plugin, log a warning, continue scanning others" (§1), never
// a crash or exception escaping this function.
std::optional<JsonValue> parseJson(const std::string& text);
```

Scope: standard JSON grammar (objects, arrays, strings with the common `\"`/`\\`/`\n` escapes,
numbers, `true`/`false`/`null`) — no need for `\uXXXX` unicode escape sequences (manifests are
ASCII identifiers/descriptions in practice; a `\uXXXX` sequence in a manifest is simply
unsupported and causes that manifest to fail parsing, which is an acceptable, honestly-logged
rejection rather than silently mis-parsing it). This is deliberately not a general-purpose
JSON library — it exists solely to read `manifest.json` files against the fixed schema in §4,
and its test suite (§10) should reflect that scope, not attempt full JSON-spec conformance.

Directory layout the loader expects, one subdirectory per plugin:
```
<plugin_dir>/system-info/manifest.json
<plugin_dir>/system-info/libsystem_info_plugin.so
```

**Validation order (fail before touching `dlopen` wherever possible):**
1. Parse `manifest.json`. Malformed JSON, or a missing required field (`id`, `version`,
   `abi_version`, `library`, `capabilities`) → reject this plugin directory, log a warning,
   continue scanning others. One bad plugin never aborts discovery of the rest.
2. `abi_version` must equal `JARVIS_PLUGIN_ABI_VERSION` (the loader's own compiled-in
   constant) → mismatch is rejected before `dlopen`.
3. Each declared capability's `power_tier` must be one of the five known strings → unknown
   tier string is rejected.
4. Each declared `intent` must not already be registered (by an earlier-scanned plugin, or by
   a compiled-in builtin) → duplicate intent is rejected; first-registered wins, the
   duplicate's entire plugin is refused (not just the colliding capability), since a plugin
   that names the same intent twice as another plugin is either a config mistake or hostile,
   and partial activation would be a worse failure mode than an honest full rejection.
5. Only after 1-4 pass: `dlopen(library, RTLD_NOW | RTLD_LOCAL)` the library at the manifest's
   `library` path (resolved relative to the manifest's own directory — never a bare
   filename looked up on `LD_LIBRARY_PATH`, to keep this "trusted local directories only" per
   the Codex plan's own assumption, not an implicit system-wide search).
6. `dlsym` both required symbols. Missing either → reject, `dlclose` immediately.
7. Call `jarvis_plugin_abi_version()`; must equal the manifest's own declared `abi_version`
   (a manifest lying about its plugin's actual ABI version is also rejected) and the host's
   `JARVIS_PLUGIN_ABI_VERSION`.
8. Call `jarvis_plugin_register()`. For every `registerCapability` call the plugin makes
   during this callback, cross-check the `(intent_name, power_tier)` pair the plugin actually
   registered against what the manifest declared for that intent — a mismatch (plugin
   registers a capability the manifest never declared, or registers a declared intent under a
   different tier than the manifest claims) is rejected, and any capabilities already
   registered from this plugin in this same call are rolled back (removed from the registry)
   before the plugin is `dlclose`'d. This closes the gap where a manifest could claim T0 to
   pass a lenient reviewer while the actual `.so` registers T2 — the manifest and the running
   code must agree.

## 5. `PluginLoader` C++ interface

```cpp
// core/plugin_loader.h
#pragma once

#include <string>
#include <vector>

class CapabilityRegistry;

struct PluginLoadResult {
    std::string pluginId;
    bool loaded;
    std::string reason;  // empty if loaded; human-readable rejection reason otherwise
};

class PluginLoader {
 public:
    // Scans pluginDir for one subdirectory per plugin (see manifest layout above),
    // validates and loads each one, registering its capabilities into registry. Returns one
    // PluginLoadResult per subdirectory found (loaded or rejected) — callers log/report this,
    // e.g. at startup. A directory that doesn't exist yields an empty result list, not an
    // error (same fail-open-for-discovery precedent as PluginConfig's missing-file default).
    std::vector<PluginLoadResult> loadFromDirectory(const std::string& pluginDir,
                                                      CapabilityRegistry& registry);

    // Removes a plugin's capabilities from dispatch (registry no longer resolves them) but
    // keeps its library mapped in memory. Reversible in-process only by restarting (no
    // re-enable in this pass — matches the existing PluginConfig disable semantics, which are
    // also process-lifetime, not toggleable without a restart... actually PluginConfig's
    // enable/disable IS toggleable via config; disablePlugin() here is the loader's own
    // separate, coarser "stop dispatching this whole plugin's capabilities" operation,
    // distinct from PluginConfig's per-capability enabled flag).
    bool disablePlugin(const std::string& pluginId, CapabilityRegistry& registry);

    // dlclose()s a plugin's library. Refuses (returns false) if the plugin is not already
    // disabled, or if any invocation of one of its capabilities is currently in flight
    // (tracked via a per-plugin atomic invocation counter incremented/decremented around
    // every execute() call the loader's own trampoline makes — see §6).
    bool unloadPlugin(const std::string& pluginId);

    const std::vector<std::string>& loadedPluginIds() const;

 private:
    // ... dlopen handles, per-plugin invocation counters, manifest data
};
```

## 6. Invocation tracking for safe unload

Each plugin capability's `Capability::execute` (the `std::function<std::string(const
std::string&, ExecutionContext&)>` the rest of the system already dispatches through) is not
the raw plugin function pointer — the loader wraps it in a **trampoline** closure that:
1. Increments the plugin's atomic invocation counter.
2. Calls the plugin's raw `JarvisCapabilityFn` with the payload as a C string.
3. Copies the returned `char*` into a `std::string`, then `free()`s the original — this is the
   one place `malloc`/`free` crosses back into the C++ registry's ownership model, and it
   happens entirely inside the trampoline, invisible to the rest of the codebase.
4. Decrements the invocation counter (via RAII, so a plugin capability that throws — it
   shouldn't, since the ABI is C and has no exceptions, but a bug could still crash into UB —
   doesn't leak the counter increment; wrap steps 2-3 in a scope guard).
5. Returns the `std::string` — from here on it's an ordinary `Capability::execute`, indistin-
   guishable to `CapabilityRegistry::dispatch()` from a compiled-in builtin's lambda.

`unloadPlugin()` checks the counter is `0` before calling `dlclose`; if not, it returns
`false` with no retry/wait built in for this pass (a caller wanting to unload a busy plugin
tries again later — no blocking wait, since blocking `main()`'s startup/shutdown path on an
in-flight capability call risks exactly the kind of hang INV-7 forbids).

## 7. Config: plugin directories

New flat file `config/plugin_dirs.cfg` (git-tracked, same discipline as `capabilities.cfg`),
one directory per line, comments with `#`:

```
# Directories PluginLoader scans at startup, one per line. Relative paths are resolved
# against the process's working directory (matches how config/capabilities.cfg is already
# looked up).
plugins/
```

A tiny loader (`loadPluginDirs(path) -> std::vector<std::string>`, in `core/plugin_loader.cpp`
or a small dedicated file if it doesn't fit cleanly) reads this — one line per directory, `#`
comments and blank lines skipped, identical parsing discipline to `PluginConfig`. Missing file
→ empty list (no plugin directories scanned, not an error) — matches "networked integrations
disabled until configured" spirit even though this isn't networking: nothing loads unless a
directory is actually configured to be scanned.

## 8. `system-info` becomes the bundled reference plugin

- New CMake target `system_info_plugin` (`add_library(system_info_plugin SHARED
  plugins/system-info/plugin.cpp)`), built to `build/plugins/system-info/` (a
  `LIBRARY_OUTPUT_DIRECTORY` property, or a post-build `COMMAND` copy — implementer's choice,
  documented in the plan task).
- `plugins/system-info/plugin.cpp` re-implements the exact same OS/arch/compiler/thread-count
  logic already in `core/capability_registry.cpp`'s `makeSystemInfoCapability()` (moved, not
  duplicated-and-diverged — delete `makeSystemInfoCapability()` entirely). `CommandType::SYSTEM_INFO`
  and `COMMAND_TYPE_SYSTEM_INFO` are both reverted (see §12 — nothing external depends on the
  enum value yet, so carrying a compatibility shim for it is unnecessary complexity).
- `plugins/system-info/manifest.json` — checked into git (source, not a build artifact),
  referencing the built `.so` by relative filename per §4's layout. Since the manifest lives
  in `plugins/system-info/` (source) but the `.so` builds into `build/plugins/system-info/`
  (build output), `config/plugin_dirs.cfg` points at `build/plugins/` and a CMake
  `configure_file`/`file(COPY ...)` step places a copy of `manifest.json` alongside the built
  `.so` at configure/build time — the loader always reads a manifest from the same directory
  as the library it names, per §4, so the shipped manifest and the actual built library must
  live together at runtime regardless of where their sources sit in the repo.
- `registerBuiltinCapabilities()` drops `makeSystemInfoCapability()`; `main()` and
  `grpc_server_main()` each gain a `PluginLoader` call against `loadPluginDirs("config/plugin_dirs.cfg")`'s
  directories, after `registerBuiltinCapabilities()` and before `registry.setPluginConfig()`.
- Every existing `system-info` test (CLI, gRPC, Understanding-tier classification, the 5
  GoogleTest cases from the prior commit) must still pass unchanged in *behavior* — some will
  need updating in *setup* (constructing a `PluginLoader` and loading the reference plugin
  instead of just calling `registerBuiltinCapabilities()`), since the capability no longer
  exists without going through the loader. This is the single biggest test-shape change in the
  implementation plan and needs its own dedicated task.

## 9. Error handling

- Malformed manifest JSON, missing required field, ABI mismatch, duplicate intent, or a
  plugin/manifest capability mismatch (§4 step 8): logged via `spdlog::warn` naming the
  plugin directory and the specific reason, loading continues with the next plugin directory.
- A plugin directory with no `manifest.json` at all is silently skipped (not every
  subdirectory of a scanned directory is necessarily a plugin — e.g. stray files) — only
  `spdlog::warn` if a `manifest.json` exists but fails validation.
- `dlopen`/`dlsym` failure (bad library, missing symbol) is treated identically to a manifest
  validation failure — logged, skipped, discovery continues.
- `unloadPlugin()` on a plugin with in-flight invocations returns `false` with a log line, not
  an exception or process abort.

## 10. Testing plan (INV-13)

- `tests/plugin_loader_test.cpp`: manifest parsing (valid/malformed/missing-field), ABI
  version mismatch rejection, duplicate-intent rejection (against both another plugin and a
  compiled-in builtin), the manifest-vs-actual-registration cross-check (§4 step 8) rejecting
  a deliberately mismatched test fixture plugin, `loadFromDirectory` on a missing directory
  returning an empty list, `disablePlugin`/`unloadPlugin` happy paths, and `unloadPlugin`
  refusing while an invocation is in flight (a test capability that blocks on a
  controllable condition variable until the test signals it, invoked on a background thread,
  while the main test thread calls `unloadPlugin` and asserts it returns `false` until the
  invocation is released).
- A small, deliberately-invalid **fixture plugin** built as its own CMake test target (e.g.
  `tests/fixtures/plugins/bad-abi/`, `tests/fixtures/plugins/mismatched-tier/`) — real `.so`
  files the test suite actually `dlopen`s and rejects, not just hand-written manifests with no
  backing library. This is what makes the rejection tests genuine rather than assumed.
- `tests/system_info_plugin_test.cpp` (or extend the existing `capability_registry_test.cpp`
  suite) proving the *reference* plugin loads, dispatches, and matches the exact output shape
  the previous compiled-in version had (OS/Architecture/Compiler/Hardware threads lines).
- Extend `tools/grpc_smoke_test.py` and the C++ integration path: `system-info` over gRPC must
  keep working identically post-migration — this is a regression check, not new coverage.
- A documented manual Linux smoke check (per the Codex plan's own acceptance criteria):
  build, run `jarvis`, confirm `system-info` still works, confirm `help` still lists it,
  confirm a deliberately broken plugin directory (malformed manifest) logs a warning and
  doesn't crash startup.

## 11. What doesn't change

- `CapabilityRegistry::dispatch()` (both overloads), `ConsentGate`, `PluginConfig` — untouched.
  A plugin-loaded capability is gated by `setPluginConfig()`/`ConsentGate` identically to a
  compiled-in one, since both are just `Capability` values by the time `dispatch()` sees them.
- `ai/`, `voice/` — untouched. Neither knows or cares whether an intent's capability came from
  a compiled-in builtin or a loaded plugin.
- The `echo`/`about`/`status`/`help` builtins stay compiled-in — only `system-info` migrates
  to prove the mechanism; converting the rest is explicitly not part of this pass (no reason
  to destabilize four already-stable, already-tested builtins just to prove a point already
  proven by one migration).

## 12. Bridging intent-only capabilities into the CLI and legacy gRPC paths

`system-info` losing its `CommandType` enumerator (it's dropped entirely in this pass — it
was only added in the prior, still-local, unpushed commit, so nothing external depends on it;
reverting it is simpler than carrying a wire-compat shim for a value that never shipped) means
three existing call sites that only knew how to route by enum must gain a string-intent
fallback, or `system-info` becomes unreachable outside a client that explicitly sets the new
`intent` proto field:

- **`core/command_handler.cpp`**: drop the `"system-info"` entry from `COMMAND_MAP` — there is
  no `CommandType` left to map it to. A CLI user typing `system-info` now parses to
  `CommandType::UNKNOWN` with the full input as the "unrecognised" word.
- **`core/engine.cpp`**: when `registry.dispatch(parsed.type, ...)` yields `std::nullopt` and
  `parsed.type == CommandType::UNKNOWN`, before printing `runUnknown()`, try
  `registry.dispatch(<first word of input, lowercased>, parsed.payload, context)` — a literal
  string-intent lookup. CLI-typed intent names are already lowercase-kebab
  (`system-info`), so no normalization is needed here, only a lowercase pass (reuse
  `toLower`). This makes any plugin-registered intent typeable by name from the CLI with zero
  further changes each time a new plugin ships — the fallback is generic, not
  `system-info`-specific.
- **`core/jarvis_service.cpp`**: the AI-classified re-dispatch branch (`Step 4`) currently maps
  a classifier intent string (`"STATUS"`, `"SYSTEM_INFO"`, SCREAMING_SNAKE convention) through
  `intentToCommandType()` to a `CommandType`, then dispatches by enum. Extend this: if
  `intentToCommandType()` returns `CommandType::UNKNOWN` for a given classified intent string,
  fall back to a string dispatch using a normalized form of that intent (lowercase, `_` → `-`,
  e.g. `"SYSTEM_INFO"` → `"system-info"`) against `registry.dispatch(normalized, ...)`. This is
  the one general bridge between the Understanding tier's existing SCREAMING_SNAKE intent
  labels and the registry's lowercase-kebab intent names — needed once, here, and every future
  plugin whose classifier pattern name matches its registered intent name (modulo casing)
  benefits automatically. Renaming the classifier's own convention to match the registry
  exactly is a larger, separately-scoped cleanup not undertaken in this pass (YAGNI — the
  normalization bridge is a five-line function, not worth a wider rename yet).
- **`ai/intent_classifier.py`**, **`ai/llm_backend.py`**: unchanged — `SYSTEM_INFO` stays a
  valid classifier output label; only the C++ side's *consumption* of that label changes.
- **`tools/interactive_client.py`**, **`tools/grpc_smoke_test.py`**, **`voice/voice_client.py`**:
  each has a `KNOWN_COMMANDS`-style dict mapping a typed/routed first word straight to a
  `jarvis_pb2.COMMAND_TYPE_*` enum value. Since `COMMAND_TYPE_SYSTEM_INFO` no longer exists,
  each of these three files needs a second, small `KNOWN_INTENTS` dict (`{"system-info":
  "system-info"}`) checked when the first word isn't in the enum dict, setting
  `ExecuteCommandRequest(intent=<name>, payload=...)` instead of `command=<enum>`. This is the
  first real client-side use of the `intent` proto field added in the prior commit — proving
  the "new client, no enum" story the whole migration exists for, not just adding a field
  nothing sets.
- **`proto/jarvis.proto`**: revert the `COMMAND_TYPE_SYSTEM_INFO` enumerator and its
  `internalCommandToProto`/`protoCommandToInternal`/`intentToCommandType` switch cases in
  `core/jarvis_service.cpp` and `core/command_handler.h`'s `CommandType` enum. The `intent`
  string fields on both proto messages (added in the prior commit) stay — those are the
  actual extensibility mechanism; the enum value was a false start superseded by this spec.

## 13. Deferred / explicitly not now

- Any plugin family beyond `system-info` (file search, notes, system control, desktop/browser,
  media, calendar) — each is its own separately-scoped future spec+plan, per the Codex plan's
  own capability delivery order.
- Hot-reload, plugin marketplace, signing, sandboxing.
- Windows/macOS loader backends.
- A general plugin-authoring SDK/CLI beyond this header + manifest format (e.g. a `jarvis-plugin
  new` scaffolding tool) — not needed until a second, third-party plugin actually exists.
