# Plugin Manager (S2 completion) — Design Spec

**Status:** approved (blanket approval given in-conversation, 2026-08-30 — no per-section
sign-off required).
**Phase:** Phase 4 (Plugins, Desktop Control & Integrations) — prerequisite piece only. Builds
on S2 (Capability Registry, already merged) toward completing it per
`docs/architecture-blueprint.md` §S2, which calls for "a plugin lifecycle (discover / load /
unload / dispatch); formalisation of the **consent gate** as an explicit orchestration step."

## 1. Scope

**In scope:**
- Config-driven enable/disable for registered capabilities (no rebuild needed to turn one off).
- A consent gate: T0 capabilities dispatch freely; T1 dispatches freely (future: gets logged
  distinctly, not built here since structured C++ logging of this kind already exists via
  spdlog and needs no new mechanism); T2 requires a one-time granted permission, checked before
  dispatch, stored locally; T3/T4 are explicitly *not* supported yet — a capability declaring
  either tier is denied by default with an honest "not yet supported" message, rather than
  pretending a real confirmation-every-time (T3) or opt-in+auth (T4) flow exists when it
  doesn't.
- A minimal, purpose-built config surface for exactly this (capability enable/disable, T2
  consent grants) — not the general-purpose JARVIS config system blueprint §4.2 describes.
  That's real, separate future work; this spec deliberately doesn't build it.
- A way to grant T2 consent (a CLI flag on the existing `jarvis` binary).

**Out of scope (deliberately):**
- Any new plugin/capability (no file search, no media control, no system control — the user
  wants to decide what to build later; this spec builds only the substrate).
- True dynamic `.so` loading at runtime. Per the earlier Capability Registry review, this needs
  the compile-time `CommandType` enum replaced with string-keyed intents — real work with no
  payoff yet, since nothing here needs a third-party/runtime-added plugin. Capabilities stay
  compiled into the binary; "load/unload" here means "active per config," not "loaded from
  disk."
- T3 (confirmation-every-time) and T4 (opt-in+auth) enforcement mechanisms. Declaring a
  capability at those tiers is supported (the `PowerTier` enum already has them); *enforcing*
  them is not — denied by default, honestly labeled as unimplemented.
- The general-purpose JARVIS configuration system (ports, model names, thresholds — blueprint
  §4.2). This spec's config surface is narrowly scoped to enable/disable + T2 consent, the same
  discipline `voice_config.yaml` used for voice-only settings.
- Interactive consent prompts bubbling through gRPC. There is no terminal on the other end of a
  gRPC call (voice/remote clients) to prompt interactively. Consent granting happens out-of-band
  via the CLI, not inline during a gRPC-dispatched request.

## 2. Where this sits in the pipeline

This deepens **Orchestrate** (project rule §1: deepen a stage, never fork the pipeline) — the
same stage `CapabilityRegistry::dispatch()` already occupies. It does not introduce a new
concept parallel to `Capability`; "plugin" and "capability" are the same thing in this codebase
already (the `Capability` struct from S2 already carries a `PowerTier`). This spec adds two
checks *before* a capability's `execute()` runs, inside the existing dispatch path:

1. **Enabled check** — is this capability turned on in config? If not, treated the same as an
   unregistered capability (falls through to the existing `UNKNOWN` handling).
2. **Consent check** — does this capability's declared tier clear the gate? T0/T1 always do; T2
   requires a stored grant; T3/T4 are always denied (not yet supported).

Both checks live in `CapabilityRegistry::dispatch()` (or a small helper it calls), not inside
any capability's own `execute()` — capabilities never invent their own guardrails (INV-9).

## 3. Components

### 3.1 `core/plugin_config.h/.cpp`
Loads two flat, line-based `key=value` config files — no YAML/JSON library dependency, since
the data is genuinely this simple (a purpose-built minimal parser, consistent with INV-10's
"no library replacing a core concept"):

- `config/capabilities.cfg` (**checked into git**, ships with sensible defaults — all existing
  builtins enabled): `<capability_name>.enabled=true|false`
- `config/consent_grants.cfg` (**gitignored**, per-machine, starts empty/absent): `<capability_name>.granted=true`

```cpp
class PluginConfig {
public:
    static PluginConfig load(const std::string& capabilitiesPath, const std::string& grantsPath);
    bool isEnabled(const std::string& capabilityName) const;   // default true if unlisted
    bool isGranted(const std::string& capabilityName) const;   // default false if unlisted
    void grant(const std::string& capabilityName);              // writes to consent_grants.cfg
private:
    std::unordered_map<std::string, bool> enabled_;
    std::unordered_map<std::string, bool> granted_;
    std::string grantsPath_;
};
```

A missing `capabilities.cfg` is not an error — every capability defaults to enabled (fail-open
for *availability*, matching how the system works today with zero config). A missing
`consent_grants.cfg` is not an error either — every T2 capability simply defaults to
not-yet-granted (fail-closed for *consent*, per INV-9's intent).

### 3.2 `core/consent_gate.h/.cpp`
```cpp
struct ConsentResult {
    bool allowed;
    std::string reason;  // empty if allowed; human-readable if denied
};

class ConsentGate {
public:
    explicit ConsentGate(const PluginConfig& config);
    ConsentResult check(const Capability& capability) const;
private:
    const PluginConfig& config_;
};
```
Logic: `T0`/`T1` → always `{true, ""}`. `T2` → `{config_.isGranted(name), "..."}` with the
denial reason naming the exact CLI command to grant it. `T3`/`T4` → always `{false, "Power
tier T3/T4 enforcement is not yet implemented — this capability cannot run yet."}` — an honest
statement of a real gap, not a guess dressed up as a decision.

### 3.3 `CapabilityRegistry::dispatch()` changes
Reads current signature/behavior from the already-merged Capability Registry code
(`core/capability_registry.h/.cpp`) before editing — this integrates with, not replaces, the
existing dispatch path. New behavior, in order: resolve capability by intent (existing) → if
`!pluginConfig.isEnabled(name)`, treat as unregistered (existing `nullopt`/fallthrough path,
unchanged) → if `PowerTier != T0` and consent gate denies, return the denial reason as the
dispatch result instead of calling `execute()` → otherwise call `execute()` as today.

### 3.4 CLI grant flag
`jarvis --grant <capability_name>` (a startup flag on the existing CLI binary, checked before
entering the normal interactive loop): loads `PluginConfig`, confirms the named capability
exists in the registry, prints its declared tier, asks for a plain `y/n` confirmation on stdin,
and on `y` calls `PluginConfig::grant()` (writing `consent_grants.cfg`) then exits. This is the
only interactive consent surface — deliberately CLI-only, since it's the one surface with a
real terminal attached.

## 4. Error handling

- Malformed lines in either config file (not matching `key=value`) are skipped with a logged
  warning (spdlog), not a fatal error — a typo in a hand-edited config file shouldn't crash
  startup.
- `--grant <name>` for an unknown capability name prints the list of currently registered
  capability names and exits non-zero, rather than silently writing a grant for something that
  doesn't exist.

## 5. Testing plan (INV-13)

- `tests/plugin_config_test.cpp`: parsing valid/malformed `key=value` lines, missing-file
  defaults (enabled-by-default, granted-by-default-false), `grant()` actually persisting to disk
  and being re-read correctly on a fresh `PluginConfig::load()`.
- `tests/consent_gate_test.cpp`: T0/T1 always allowed regardless of config state; T2 allowed iff
  granted, with the correct denial reason string when not; T3/T4 always denied with the correct
  "not yet implemented" reason, regardless of any grant state (since granting isn't even
  possible for those tiers via `--grant` — Step 3.4's tool should refuse to grant a T3/T4
  capability, treating that as a "not yet supported" case too, not a bypassable no-op).
- `tests/capability_registry_test.cpp` (extend, don't replace): a disabled capability (via a
  test-injected `PluginConfig`) is dispatched exactly like an unregistered one; a T2 capability
  without a grant returns the denial message instead of executing; a T2 capability *with* a
  grant executes normally. Uses synthetic test capabilities at each tier (the existing test file
  already builds ad-hoc test `Capability` instances for registry tests) — no real T2 capability
  exists yet, and none is needed to prove the gate works.

## 6. What doesn't change

- `ai/` — untouched. Understanding still only emits an intent; it doesn't know about tiers,
  config, or consent (per S2's own "leaves untouched" note).
- `voice/` — untouched. Nothing here is voice-specific.
- The existing four builtins (echo/status/about/help) — all ship enabled-by-default in
  `capabilities.cfg`, all remain T0, so this change is invisible to them at runtime; their
  behavior is provably unchanged (a registry test parametrized over "does dispatch still work
  for all four builtins with the new gate in place" closes this).

## 7. Deferred / explicitly not now

- Any actual new plugin (file search, media control, system control, desktop interaction,
  reminders, calendar) — the user's own explicit call, revisit once decided.
- True dynamic `.so` loading (needs the `CommandType` → string-keyed-intent migration).
- T3/T4 real enforcement (confirmation-every-time UI, opt-in+auth flows).
- The general-purpose JARVIS config system (ports, model names, thresholds, memory paths).
- Structured per-tier logging distinctions beyond what spdlog already provides.
