# JARVIS Architecture Blueprint

**Status:** Foundational spec. This is the output of the architectural planning pass that
`docs/vision.md` gated all further implementation behind. Everything JARVIS builds from here
on obeys this document. It describes the **whole architecture, from the minimal core we have
today to the theoretical fully-scaled assistant**, as a single design that grows by accretion.
It absorbs and replaces the former `docs/architecture.md` — the still-relevant reasoning from
that doc (why hybrid, why gRPC over the alternatives) is preserved in **Appendix A**.

The enforceable, session-loaded distillation of this blueprint lives in the root
[`CLAUDE.md`](../CLAUDE.md). When the two ever disagree, `CLAUDE.md` is the rule and this
document is the reasoning — fix whichever is wrong.

---

## 0. How to read this document

The blueprint has three parts:

1. **The Spine** (§1–§3) — the canonical pipeline and the invariants that must hold at
   *every* scale, from today's core to the fully-scaled system. This is the part that does
   not change. If a future feature can't attach without violating the spine, the feature is
   wrong, not the spine.
2. **The Cross-Cutting Subsystems** (§4) — config, observability, security/consent, memory,
   contracts. Present at every scale; they grow in sophistication, not in shape.
3. **The Evolutionary Stages** (§5) — S0 (the minimal core, roughly where we are) through S7
   (the frontier). Each stage is machinery *added onto the spine*, never a rewrite of what's
   below it. Each stage names what it adds, which invariant governs it, and what it must
   leave untouched.

A map of §5 onto the existing phase docs is in §6, so this blueprint and
`docs/roadmap.md` / `docs/features.md` stay coherent rather than competing.

---

## 1. The mental model — the canonical pipeline

Every request JARVIS will ever serve — a typed command today, a spoken multi-step task in the
fully-scaled system — flows through the **same seven-stage pipeline**. New capability is
always "make one of these stages smarter," never "add a parallel pipeline."

```
  ┌───────────┐   ┌───────────┐   ┌──────────────┐   ┌───────────────┐   ┌────────────┐   ┌────────────┐   ┌───────────┐
  │ 1. CAPTURE│──▶│2. INGRESS │──▶│3. UNDERSTAND │──▶│4. ORCHESTRATE │──▶│5. EXECUTE  │──▶│6. SYNTHESE │──▶│7. RENDER  │
  │  surfaces │   │ normalise │   │   → Intent   │   │  → Capability │   │ capability │   │  Response  │   │ to surface│
  └───────────┘   └───────────┘   └──────────────┘   └───────────────┘   └────────────┘   └────────────┘   └───────────┘
        ▲                                  │                  │                  │                                  │
        │                                  ▼                  ▼                  ▼                                  ▼
        └──────────────────────  CROSS-CUTTING SPINE: Contracts · Config · Observability · Security/Consent · Memory  ──┘
```

**Stage by stage:**

1. **Capture (surfaces).** Where input originates: CLI, gRPC client, voice (mic + wake word),
   gesture, UI, mobile, ambient trigger. A surface is a *thin adapter* — it captures raw input
   and nothing more. It owns zero business logic.
2. **Ingress / normalisation.** Convert any modality into one canonical `Request`: the text
   (or structured payload), plus metadata — session id, modality, source confidence,
   timestamp, correlation/request id. After this stage nothing downstream cares *how* the
   input arrived.
3. **Understand → Intent.** Resolve the canonical `Request` into a structured `Intent`
   (`{name, slots/args, confidence}`). This is a **tiered resolver** with a fallback ladder
   (deterministic parse → rule classifier → LLM reasoner). Understanding lives *one layer
   above* execution and never inside the parser or the engine.
4. **Orchestrate → Capability.** Resolve `Intent` → a concrete `Capability` via the
   **Capability Registry**. Apply policy/consent gates *here, before anything runs*. For
   multi-step requests, produce a plan (an ordered set of capability calls), not a single
   dispatch.
5. **Execute capability.** Run the work: an engine builtin, a plugin, a desktop action, an
   external integration. Execution returns *data*, never rendered output for a specific
   surface. Hot-path work stays in C++; fuzzy/slow/experimental work is delegated across a
   contract to a worker.
6. **Synthesise Response.** Turn capability results into one canonical `Response`
   (`{success, content, error, artefacts, follow-ups}`).
7. **Render to surface.** The originating surface's adapter renders the canonical `Response`
   into its medium — stdout, a TTS utterance, a GUI update. Same `Response`, many renderers.

The single most important property: **a stage never knows the identity of the stages two hops
away.** Execution doesn't know which surface called it. A surface doesn't know how an intent
was resolved. This is what lets any stage be upgraded — rule classifier → LLM, CLI → voice —
without touching the others.

---

## 2. The current code, mapped onto the pipeline

The pipeline is not aspirational — the existing Phase-1 code already *is* a (thin) instance of
it. Naming the mapping keeps future work honest:

| Pipeline stage | Today's implementation | File |
| --- | --- | --- |
| 1. Capture | CLI read loop; gRPC client | `core/engine.cpp`, `tools/grpc_smoke_test.py` |
| 2. Ingress | `ExecuteCommandRequest` (proto) / raw stdin line | `proto/jarvis.proto`, `core/engine.cpp` |
| 3. Understand | `parseCommand()` deterministic parse; UNKNOWN → Python echo | `core/command_handler.cpp`, `ai/jarvis_ai_server.py` |
| 4. Orchestrate | `COMMAND_DISPATCH` map; UNKNOWN forward | `core/command_handler.cpp`, `core/jarvis_service.cpp` |
| 5. Execute | `runEcho/runHelp/runAbout`; engine state for STATUS | `core/command_handler.cpp`, `core/engine.cpp` |
| 6. Synthesise | `ExecuteCommandResponse` fill | `core/jarvis_service.cpp` |
| 7. Render | `std::cout` (CLI) / proto response (gRPC) | `core/command_handler.cpp`, client |
| Cross-cut: Contract | `jarvis.proto`, `ai.proto` | `proto/` |
| Cross-cut: Observability | spdlog structured logs | `core/*.cpp` |
| Cross-cut: Degradation | 5s AI deadline, `[AI unavailable]` | `core/ai_client.cpp` |

What's missing today is not *shape* — it's depth: stage 3 is a keyword parser with a
placeholder behind it, stage 4 has no registry, and the cross-cutting Config / Consent /
Memory services barely exist. §5 is how that depth gets added without bending the shape.

---

## 3. The Invariants — the spine that holds at every scale

These are the load-bearing rules. They are numbered so `CLAUDE.md` and code reviews can cite
them (e.g. "this violates **INV-6**"). They are the real deliverable of this blueprint: the
architecture *is* these invariants; §5 is just their consequences over time.

- **INV-1 — Transport-agnostic core.** Business logic returns data; it never performs I/O to a
  specific surface and never learns which transport invoked it. `runCMD()` returns a string
  today for exactly this reason. Every future capability preserves it.

- **INV-2 — Contract-first seams.** Every cross-process or cross-language boundary is a
  versioned Protobuf/gRPC contract. No ad-hoc text or JSON crosses a process boundary. Proto
  field numbers are append-only; renaming is allowed, renumbering and reuse are forbidden.
  New wire fields take the next free number.

- **INV-3 — Thin surfaces.** Every input/output surface (CLI, voice, gesture, UI, mobile) is a
  thin adapter over the same service API. A surface captures or renders; it never decides,
  dispatches, or executes. Adding a surface adds zero business logic.

- **INV-4 — Adapters at every boundary.** Translation between representations (proto ↔ internal
  enum, modality ↔ canonical `Request`, capability result ↔ `Response`) lives in a dedicated
  adapter, isolated from the logic on either side. `jarvis_service.cpp` and `ai_client.cpp`
  are the template.

- **INV-5 — Understanding sits above execution.** Deciding *which* capability a fuzzy request
  means is the Understanding tier's job (stage 3), never the parser's or the engine's. The
  engine executes typed, already-resolved intents. No amount of keyword tuning at the C++
  parser level is ever the fix for an understanding problem.

- **INV-6 — Capabilities register against intents; the dispatcher stays ignorant.** The
  orchestrator resolves intent → capability through the **registry**. It never hardcodes
  knowledge of a specific capability. New capabilities self-register (by manifest or
  registration call) keyed by the intents they satisfy. Adding a capability never edits the
  dispatcher.

- **INV-7 — Blast-radius isolation / graceful degradation.** A slow, missing, or failing
  downstream (the AI worker, a plugin, the network, an external API) must never block the core
  or an unrelated capability. Every cross-boundary call has a deadline and a defined fallback.
  When a capability genuinely can't be served, JARVIS says so honestly (the "I can't do that
  yet" path) — it never hangs and never fails silently.

- **INV-8 — Tiered intelligence behind a stable interface.** Understanding always tries the
  fast/cheap/deterministic path first and escalates to the slow/smart/LLM path only when
  needed. The resolver's interface (`resolve(request) -> Intent`, today's `classify()`) is
  fixed; which backend answers is an implementation detail. Swapping rule-classifier for an
  LLM changes nothing upstream or downstream.

- **INV-9 — Consent scales with power.** Every capability declares a **power tier** (see §4.3).
  The orchestrator enforces the matching consent gate *before* execution. The more damage a
  capability can do (write, delete, control the desktop, touch a device, reach the network,
  move money), the stronger the gate. Consent is never checked inside the capability — it's
  enforced at orchestration, uniformly.

- **INV-10 — Build from primitives; every tier must teach.** This is the founding ethos, now
  binding. No architectural tier may be replaced by a library that hides the concept the tier
  exists to teach (the parser, the dispatch map, the service boundary, the contract, the
  registry). Mature third-party libraries are permitted **only at leaf capabilities** where
  reinventing teaches nothing (STT, TTS, vision inference, a smart-home protocol) — and even
  then only *behind* the capability contract, never punching through the spine. JARVIS is
  never allowed to collapse into a thin wrapper around an LLM call.

- **INV-11 — Local-first and private by default.** The default path runs entirely on the local
  machine: local LLM (Ollama), local STT/TTS, local memory store. Any external network call is
  opt-in, consent-gated (INV-9), and never on the default path. User data stays local and is
  inspectable, exportable, and deletable.

- **INV-12 — Growth is additive; the core contract stays stable.** Reaching a higher stage in
  §5 must not require rewriting a lower one. Adding intelligence, capabilities, surfaces,
  memory, or devices is accretion onto the spine. If a change forces the C++ core contract to
  break, stop — either the change is mis-designed or the contract needs a deliberate,
  versioned `v2` migration, decided explicitly, never as a side effect.

- **INV-13 — Every seam is verifiable.** Testing and evaluation are first-class architecture,
  not an afterthought bolted on at the end. Every contract seam has a **contract/parity test**
  (the existing `grpc_smoke_test.py` is the seed — it verifies the C++↔Python round-trip and
  that gRPC output matches CLI output); every capability is independently testable because it
  returns data (INV-1); and the Understanding tier ships with an **evaluation harness** that
  measures the metrics its design trades off (accuracy, latency, resource cost across
  deterministic / LLM / hybrid configurations). A tier is not "done" until its behaviour can be
  reproduced and its claims backed by evidence, with documentation detailed enough for someone
  else to replicate the tests. Verification precedes any "it works" claim.

---

## 4. Cross-cutting subsystems (the vertical spine)

These six exist at every scale and are consumed by every pipeline stage. They grow in
capability; their role never changes.

### 4.1 Contract layer (Protobuf / gRPC)

The seams between processes and languages. The typed nervous system of the whole design.

- **Today:** `jarvis.proto` (client ↔ C++ core), `ai.proto` (C++ core ↔ Python understanding).
- **Grows to carry:** structured `Intent` + confidence; a **Capability manifest/contract**
  proto (how a capability declares its intents, args, and power tier); session/correlation ids;
  memory-query messages; auth metadata for the multi-device stage.
- **Discipline (INV-2):** one proto is the single source of truth for each seam; both language
  stubs are generated from it (`generated/`). Version with package namespaces (`jarvis.v1`,
  `jarvis.ai.v1`, later `.v2`). Field numbers are immutable once shipped. Regenerate stubs on
  every proto change; never hand-edit generated code.

### 4.2 Configuration

The single source of runtime settings. **Currently a real gap** (flagged in `features.md`) —
ports, model names, thresholds, and paths are hardcoded or implicit.

- **Add early** (it's a prerequisite for almost everything downstream): one typed config
  surface — ports, AI backend selection + model, confidence thresholds, timeouts/deadlines,
  memory paths, per-capability enable/disable, consent policy.
- **Grows to:** per-tier config, environment overrides, runtime-reloadable settings, a settings
  UI (a Phase-6 client of the same config).
- **Rule:** no new tunable constant gets hardcoded in a source file once the config surface
  exists — it goes through config.

### 4.3 Security & consent

Enforced at the **orchestration** stage (INV-9), never inside capabilities.

Every capability declares a **power tier**:

| Tier | Meaning | Example | Gate |
| --- | --- | --- | --- |
| **T0 Read-only** | No side effects | `status`, `about`, screen-*read* | none |
| **T1 Stateful-local** | Writes local state JARVIS owns | notes, reminders, memory writes | logged; revocable |
| **T2 System-affecting** | Acts on the user's machine | open app, volume, files, desktop automation | explicit consent gate |
| **T3 Destructive/irreversible** | Hard to undo | delete files, shutdown | confirmation prompt every time |
| **T4 External / networked** | Leaves the machine | web actions, smart-home, mobile, any API | opt-in + consent + auth (INV-11) |

The consent model is uniform: the orchestrator reads the capability's declared tier, applies
the configured gate, and only then permits execution. Higher stages (desktop control,
smart-home) do **not** invent their own ad-hoc guardrails — they declare a tier and inherit the
gate. Allowlists of known-safe operations live in config (4.2).

### 4.4 Observability

Structured logs, metrics, tracing — added early, per the architecture doc's own principle.

- **Today:** spdlog structured logs on the C++ side; `print()` on the Python side (a known
  debt).
- **Grows to:** structured logging on *both* sides; a **correlation/request id threaded through
  every hop** (surface → core → understanding → capability) so one user action is one traceable
  story across processes; latency + outcome metrics per stage; opt-in tracing.
- **Rule:** every cross-boundary call logs request id, the operation, latency, and outcome.
  A new tier isn't "done" until it's observable.

### 4.5 Memory & state

State grows from ephemeral to durable to semantic.

- **Today:** in-process engine state (running flag, uptime, last command).
- **Grows through:** per-**session** conversation context (threaded via session id) → durable
  local store (SQLite: users, sessions, captured items) → embeddings + vector recall
  (semantic memory) → privacy controls (inspect / export / delete, retention policy — INV-11).
- **Rule:** capabilities and the understanding tier *read* memory through a defined interface;
  they never reach into another tier's storage directly. Memory is a service on the spine, not
  a shared global.

### 4.6 Concurrency & execution model

How work actually runs. Under-specifying this is fine at S0 (one request at a time) but becomes
load-bearing the moment always-listening voice (S3) and multi-step plans (S5) arrive.

- **Long-lived services, not per-request process spawn.** Each tier is an always-on service with
  a persistent connection (the C++ core, the Python worker, later the memory service). A request
  never pays process-startup cost. This is already true and is a hard rule — it is the reason the
  gRPC boundary was chosen over subprocess text I/O (see Appendix A).
- **The core never blocks on a slow tier (INV-7).** Any call that can be slow (LLM inference, a
  T4 external API, a long-running plan step) is issued with a deadline and, where it isn't needed
  synchronously, handed to a worker/queue rather than run on the request thread. The 5s AI
  deadline is the seed of this discipline.
- **Backpressure over unbounded queues.** When slow work is queued (batch classification, a plan
  of capability calls, streamed audio), the queue is bounded and sheds or defers under load
  rather than growing without limit. An assistant that silently accretes a backlog is worse than
  one that says "I'm busy."
- **Streaming where latency is perceived.** Voice in/out (S3) and long LLM replies are streamed,
  not buffered to completion — the pipeline's canonical `Response` must therefore tolerate being
  produced incrementally, not only as one final blob.
- **Rule:** picking the concurrency primitive (thread pool, async event loop, work queue) is a
  per-tier implementation choice, but the *contract* between tiers stays synchronous-looking and
  deadline-bounded (INV-2, INV-7). Concurrency lives inside a tier; it never leaks into the
  contract as an ordering assumption another tier must honour.

---

## 5. The evolutionary stages (core → fully-scaled)

Each stage is machinery added onto the spine. For each: **Adds**, **Governing invariants**,
**Leaves untouched**. The stages are architectural strata, not a schedule — the schedule lives
in `roadmap.md`/`features.md` (mapping in §6).

### S0 — The Core (the irreducible spine) · *≈ where we are*

The minimal end-to-end pipeline: a surface, a contract, and a core that
parse → dispatch → execute → respond. One deterministic understanding step, a fixed dispatch
map, builtin capabilities, two surfaces (CLI in-process, gRPC networked).

- **Adds:** nothing — this *is* the baseline. Every invariant is already visible here in
  miniature. S0 is the proof that the spine works before any depth is added.
- **Governing invariants:** INV-1, 2, 3, 4, 7 (all already satisfied by Phase-1 code).
- **Leaves untouched:** n/a.

> **The rule S0 sets:** every later stage must degrade back to *something like S0* when the
> smarter tiers are unavailable — the CLI must still run known commands with the AI process
> dead, "basic mode" must exist. S0 is the floor, never removed.

### S1 — The Understanding Tier · *intelligence, stage 3 gets real*

Replace the placeholder echo with a genuine resolver, and teach the core to **act** on
structured intent instead of forwarding reply strings.

- **Adds:** a structured `Intent` (`{name, args, confidence}`) on the `ai.proto` contract; a
  tiered resolver — rule-based classifier first, LLM (Ollama) as the escalation path — behind a
  single stable `resolve()`/`classify()` interface; core-side re-dispatch (a resolved,
  high-confidence intent that maps to a known capability re-enters stage 4 as if it had been
  typed directly); structured logging on the Python side.
- **Governing invariants:** INV-5 (understanding is above execution), INV-8 (fallback ladder,
  stable interface), INV-7 (LLM down ⇒ fall back to rules ⇒ fall back to "can't do that yet"),
  INV-11 (Ollama is local), INV-13 (this is the tier whose design trades accuracy against
  latency and cost — so it ships with the evaluation harness that *measures* those trade-offs
  across deterministic / LLM / hybrid configurations; the measurement is part of the tier).
- **Leaves untouched:** the C++ execution stage, the `jarvis.proto` client contract, every
  surface. Only stages 3–4 deepen.

### S2 — Capability Registry & Plugin Substrate · *stage 4 generalises*

Generalise "dispatch to a hardcoded builtin" into "dispatch to any registered capability." This
is the **keystone stage** — nearly everything above it depends on the registry existing.

- **Adds:** a **Capability contract** (manifest: the intents a capability serves, its args
  schema, its power tier — 4.3); a **registry** the orchestrator consults to resolve
  intent → capability (INV-6); a plugin lifecycle (discover / load / unload / dispatch);
  formalisation of the **consent gate** as an explicit orchestration step; the
  intent → service binding that `vision.md` calls for. Builtins (echo/help/about/status) are
  re-expressed as registered capabilities so there is exactly one dispatch path.
- **Governing invariants:** INV-6 (registration, not hardcoding), INV-9 (declared power tiers),
  INV-4 (a plugin is an adapter to some underlying work), INV-10 (a plugin may use libraries
  internally but only behind the capability contract).
- **Leaves untouched:** the understanding tier (it still emits an `Intent`; it neither knows
  nor cares which capabilities are registered), surfaces, the client contract.

### S3 — Multi-Modal Surfaces · *stages 1–2 and 6–7 widen*

Add voice, then gesture, then a UI/dashboard — all as thin adapters at the ends of the pipeline.

- **Adds:** voice-in (wake word + STT) producing a canonical `Request` identical in shape to a
  typed one; voice-out (TTS) rendering the canonical `Response`; a UI/dashboard that is "just
  another gRPC client"; push-to-talk vs. always-listen as config (4.2). "Basic mode vs full
  mode" is expressed as *which resolver backends are online* (INV-8) — **not** a second
  pipeline.
- **Governing invariants:** INV-3 (surfaces stay thin — a transcript is just a `Request`
  payload, so it flows through the *existing* pipeline unchanged), INV-1, INV-7 (low-confidence
  STT ⇒ ask to clarify, don't guess).
- **Leaves untouched:** understanding, orchestration, execution, memory. A new surface adds no
  business logic (INV-3) — this is the payoff of S0–S2.

### S4 — Stateful Memory Tier · *cross-cut 4.5 deepens*

Give JARVIS durable, then semantic, memory.

- **Adds:** per-session context threaded by session id (multi-turn follow-ups resolve
  correctly); a durable local store (SQLite); embeddings + vector recall for semantic lookup;
  privacy controls (inspect / export / delete, retention).
- **Governing invariants:** INV-11 (local, inspectable, deletable), INV-2 (memory queries cross
  the boundary as typed messages), INV-12 (memory is additive — the stateless path still works).
- **Leaves untouched:** the pipeline shape. Understanding and capabilities gain a memory
  *interface* to consult; they don't restructure around it.

### S5 — Agentic Capabilities · *stages 4–5 gain planning*

The high-power capabilities that make JARVIS an OS-level agent rather than a command
dispatcher: browser/desktop automation, screenshotting, vision-backed screen understanding —
and multi-step **plans** rather than single dispatches.

- **Adds:** T2/T3 capabilities behind the strongest consent gates (4.3); a vision capability;
  an orchestrator that can execute an *ordered plan* of capability calls (produced by the LLM
  resolver) with per-step consent, not just one intent → one capability.
- **Governing invariants:** INV-9 (these are the highest-power tiers — every step gated), INV-6
  (each agentic action is still a registered capability), INV-7 (a failed step degrades
  gracefully, never leaves the system wedged), INV-10 (mature automation/vision libs allowed,
  but behind the capability contract).
- **Leaves untouched:** the contract spine, surfaces, memory interfaces. Planning is a richer
  *use* of stage 4, not a bypass of it.

### S6 — Ambient & Multi-Device · *the spine goes multi-node*

Smart-home control, a mobile companion, the core service exposed beyond localhost.

- **Adds:** a smart-home bridge (Home Assistant rather than reinventing device protocols); a
  mobile/remote client; **real authentication + TLS** on the contract layer (networked exposure
  forces it); multi-device session/identity.
- **Governing invariants:** INV-11 (external + networked ⇒ T4, opt-in, authenticated), INV-2
  (auth is contract metadata, done properly), INV-9 (device/home actions are power-tiered),
  INV-3 (a phone is just another thin surface).
- **Leaves untouched:** everything below — this is why auth was designed as contract metadata
  rather than bolted onto business logic.

### S7 — Self-Extending JARVIS · *the frontier (design spike, not a feature)*

The `vision.md` novelty: when JARVIS hits a request with no registered capability, it not only
says "I can't do that yet" (the INV-7 honesty path) but can *draft its own development plan*
for the missing capability.

- **Adds:** a meta-capability that reads the registry, detects the gap, and produces a
  structured feature plan (feeding the same planning discipline in §6). It **drafts**; it never
  self-executes changes to itself without explicit human consent.
- **Governing invariants:** INV-9 (self-modification is the highest-power action there is —
  hard human gate, always), INV-6 (it reasons over the registry, the same source of truth
  everything else uses), INV-10 (this is the ultimate expression of "every tier teaches" — but
  it earns its place only after its own design spike, never bolted on).
- **Leaves untouched:** it consumes the architecture; it does not privilege itself above the
  invariants. Explicitly gated as experimental until scoped on its own.

---

## 6. Relationship to the phase docs

This blueprint is the **architecture** (the invariant spine + how depth attaches). The phase
docs are the **schedule** (what to build next and why it teaches something). They must stay
coherent:

| Blueprint stage | Phase doc |
| --- | --- |
| S0 — Core spine | Phase 1 — Service Foundation ✅ |
| S1 — Understanding tier | Phase 2 — Intelligence Layer 🚧 (+ 2.5 LLM) |
| S2 — Registry & plugins | Phase 4 prerequisite (plugin manager) + the intent→service registry |
| S3 — Multi-modal surfaces | Phase 3 — Voice I/O, Phase 6 — UI & Dashboard |
| S4 — Memory tier | Phase 5 — Persistent Memory |
| S5 — Agentic capabilities | Phase 4 — Desktop control / vision (ambitious end) |
| S6 — Ambient & multi-device | Smart-environment + multi-device stretch |
| S7 — Self-extending | The `vision.md` self-development stretch idea |

Note the ordering nuance the phase docs already imply: **S2 (the registry) is architecturally
prior to most of S3–S5 even though "voice" (Phase 3) is scheduled earlier.** Voice can land as
a thin surface before the full registry, but any *capability growth* (Phase 4+) depends on S2.
When sequencing real work, respect the architectural dependency, not just the phase number.

When a phase's design decisions are made, fold the concrete choices back here (or into a linked
sub-doc) so this blueprint stays the current source of architectural truth — not a snapshot
that rots.

---

## 7. Target topology (directory intent)

Where each tier lives as the system scales. Present dirs keep their meaning; empty scaffolding
dirs (`plugins/`, `voice/`, `ui/`) are placeholders whose eventual role is fixed here.

```
JARVIS/
├── core/          # C++ hot path: engine, dispatch, gRPC service adapter, AI client  (S0, stages 4–5)
├── ai/            # Python understanding tier: resolver ladder, LLM/Ollama backend    (S1, stage 3)
├── proto/         # the contract spine — one proto per seam                            (cross-cut 4.1)
├── generated/     # generated stubs, never hand-edited                                 (cross-cut 4.1)
├── plugins/       # registered capabilities discovered at runtime                      (S2, stage 5)
├── voice/         # STT/TTS surface adapters                                           (S3, stages 1/7)
├── ui/            # dashboard client — a thin gRPC client, no backend logic            (S3, stages 1/7)
├── memory/        # (new) durable + semantic store service                             (S4, cross-cut 4.5)
├── config/        # (new) single typed runtime-config surface                          (cross-cut 4.2)
├── tools/         # smoke tests, dev utilities                                          (test surface)
└── docs/          # architecture-blueprint.md (this) · roadmap · features · vision
```

The rule embodied here (INV-3/INV-6): a new surface goes in a surface dir as a thin client; a
new capability goes in `plugins/` and self-registers; neither edits `core/` dispatch logic.

---

## 8. One-paragraph summary

JARVIS is a **seven-stage, contract-first, transport-agnostic pipeline** with a fixed spine and
thirteen invariants, plus six cross-cutting subsystems (contracts, config, observability,
security/consent, memory, concurrency). It grows from the minimal core (S0) to a self-extending
ambient agent (S7) purely by adding depth to individual stages — never by rewriting the spine or
breaking the core contract. Fast/deterministic understanding falls back to LLM reasoning;
capabilities self-register against intents; power-tiered consent is enforced at orchestration;
every seam is verifiable; everything runs local-first; and every tier is built from primitives
that teach, never collapsed into a thin wrapper. The enforceable form of all of this is
[`CLAUDE.md`](../CLAUDE.md).

---

## Appendix A — Foundational decisions (why hybrid, why gRPC)

Preserved from the retired `docs/architecture.md`. These are the decisions the whole spine
rests on; they are settled, and re-opening one means re-opening the architecture.

### A.1 Why hybrid C++ / Python

Each language is kept for what it is best at, and the split is deliberate, not incidental:

- **C++ owns the hot path** — long-running engine logic, low-latency command handling,
  performance-critical runtime paths, structured logging. It is the always-on core that must
  never block.
- **Python owns intelligence** — AI/NLP workflows, fast-moving integrations, rapid
  experimentation. It moves fast and can be rewritten without touching the core.

The goal was never to mix them randomly but to **split responsibilities cleanly and connect
them through a stable contract** (INV-2). This is what lets the Python side grow from
placeholder → rule classifier → LLM-backed reasoning with zero changes on the C++ side.

### A.2 Why a service boundary (gRPC) and not the alternatives

C++ and Python have different runtimes, memory models, and type systems; they cannot call each
other directly without a bridge. The options considered:

1. **Subprocess text I/O (stdin/stdout)** — brittle, unversioned, pays process-startup cost per
   call, and degrades into ad-hoc text parsing (exactly what INV-2 forbids).
2. **In-process bindings (e.g. pybind11)** — tightly couples the two runtimes in one process;
   no service boundary, no independent scaling or failure isolation.
3. **Embedded Python interpreter in C++** — same coupling, plus it drags the Python GIL and
   lifecycle into the hot path.
4. **RPC / service boundary (gRPC)** — chosen.

gRPC was chosen because it: scales to multi-process and, later, multi-device systems (S6);
gives explicit, versioned contracts between the layers; supports strong observability and
operational control; and lets each layer evolve independently. The cost — a serialization
boundary and a running second process — is precisely the cost the concurrency model (§4.6) is
built to absorb (long-lived connections, deadlines, graceful degradation).

### A.3 What this bought, in one line

A **transport-agnostic core** (INV-1) behind a **versioned contract** (INV-2), so that
intelligence, surfaces, memory, and devices can all be added later as accretion (INV-12)
without the core ever needing a rewrite. Every other invariant is a way of protecting that
property.
```
