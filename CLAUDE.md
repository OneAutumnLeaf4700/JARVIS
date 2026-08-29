# JARVIS — Project Rules (must be obeyed)

This file is the **binding architectural contract** for JARVIS. It is the enforceable
distillation of [`docs/architecture-blueprint.md`](docs/architecture-blueprint.md) — read the
blueprint for the *why*; obey this file for the *what*. When they disagree, this file wins and
the blueprint gets corrected.

These rules are JARVIS-specific and sit **on top of** the global `~/.claude/CLAUDE.md` (subagent
strategy, code standards, response style) — they do not repeat it. Where a rule below cites
`INV-n`, that is an invariant defined in the blueprint §3.

---

## 0. What JARVIS is (never lose this framing)

JARVIS is a **local-first, hybrid C++/Python personal assistant built from primitives** — a
distributed-systems learning project that happens to end in an assistant, **not** an AI project
with C++ glue, and **never** a thin wrapper around an LLM. Every tier exists to teach a concrete
concept (parsers, dispatch maps, service boundaries, RPC contracts, schema versioning). If a
proposed change would make JARVIS *simpler by hiding the concept*, it is wrong for this project
even if it is "better" engineering in the abstract. **This is INV-10 and it overrides
convenience.**

---

## 1. The pipeline is fixed — deepen a stage, never fork the pipeline

Every request flows through one seven-stage pipeline: **Capture → Ingress → Understand →
Orchestrate → Execute → Synthesise → Render.** New capability is *always* "make one stage
smarter." You may **never** introduce a parallel path that bypasses stages (e.g. a surface that
calls a capability directly, or a "quick" LLM call wired around the contract). Before writing
code, state which stage you are deepening.

---

## 2. Prime Directives (MUST / MUST NOT)

1. **Transport-agnostic core (INV-1).** Business logic returns data; it MUST NOT do I/O to a
   specific surface or learn which transport called it. `runCMD()` returning a string is the
   template — preserve it. No `std::cout`/`print`/render call inside a capability.

2. **Contract-first seams (INV-2).** Every cross-process / cross-language boundary MUST be a
   versioned `.proto` in `proto/`. No ad-hoc text or JSON across a process boundary. Proto field
   numbers are **append-only** — renaming is fine, renumbering/reuse is forbidden; new fields
   take the next free number. After any `.proto` edit, regenerate stubs; **never hand-edit
   anything in `generated/`.**

3. **Thin surfaces (INV-3).** A surface (CLI, voice, gesture, UI, mobile) MUST be a thin adapter
   that only captures input or renders a canonical `Response`. Adding a surface MUST add zero
   business logic and MUST NOT edit `core/` dispatch.

4. **Adapters at boundaries (INV-4).** Representation translation (proto↔internal,
   modality↔`Request`, result↔`Response`) lives in a dedicated adapter. `jarvis_service.cpp` and
   `ai_client.cpp` are the pattern — follow it.

5. **Understanding sits above execution (INV-5).** Deciding *which* capability a fuzzy request
   means belongs to the Understanding tier (`ai/`), never the C++ parser or engine. If you are
   tempted to add keywords to the C++ parser to "understand" phrasing — **stop**; that fix
   belongs one layer up.

6. **Register, don't hardcode (INV-6).** Capabilities self-register against the intents they
   serve; the orchestrator resolves intent→capability through the registry and MUST stay ignorant
   of specific capabilities. **Adding a capability MUST NOT require editing the dispatcher.**

7. **Graceful degradation (INV-7).** Every cross-boundary call MUST have a deadline and a defined
   fallback (the 5s AI deadline is the reference). A dead/slow downstream MUST NOT block the core
   or an unrelated capability. When JARVIS truly can't serve a request, it says so honestly — it
   never hangs and never fails silently.

8. **Tiered intelligence, stable interface (INV-8).** Understanding tries fast/deterministic
   first and escalates to the LLM only when needed, behind a fixed `resolve()`/`classify()`
   interface. Swapping the backend (rules ↔ Ollama) MUST change nothing upstream or downstream.

9. **Consent scales with power (INV-9).** Every capability declares a **power tier** (T0
   read-only → T4 external/networked; see blueprint §4.3). The **orchestrator** enforces the
   matching consent gate *before* execution. A capability MUST NOT invent its own ad-hoc
   guardrail or check consent internally — it declares a tier and inherits the gate. T3
   (destructive) and T4 (external) actions require explicit confirmation regardless of prior
   permissions.

10. **Build from primitives (INV-10).** No architectural tier (parser, dispatch, service
    boundary, contract, registry) may be replaced by a library that hides its concept. Mature
    libraries are allowed **only at leaf capabilities** (STT, TTS, vision, device protocols) and
    **only behind the capability contract**. Never punch a library through the spine.

11. **Local-first & private (INV-11).** The default path runs entirely locally (Ollama, local
    STT/TTS, local memory). Any external/network call is opt-in, consent-gated, and never the
    default. User data stays local, inspectable, exportable, deletable.

12. **Growth is additive (INV-12).** Reaching a higher capability MUST NOT rewrite a lower tier.
    If a change forces the C++ core contract to break, **stop** — either the design is wrong or a
    deliberate, versioned `v2` migration is needed. Decide that explicitly; never break a
    contract as a side effect.

13. **Every seam is verifiable (INV-13).** Testing/evaluation is first-class, not an afterthought.
    Every contract seam keeps a parity/contract test (extend `tools/grpc_smoke_test.py`, don't
    orphan it); every capability is independently testable because it returns data; the
    Understanding tier ships an evaluation harness measuring the trade-offs it makes (accuracy,
    latency, resource cost across deterministic / LLM / hybrid). **Never claim "it works" without
    running the verification and reporting the actual output** — evidence before assertions.
    (This is also the academic 13% Evaluation component — see §6.)

---

## 3. Where new code goes (placement rules)

| You are adding… | It goes in… | And it MUST… |
| --- | --- | --- |
| A new surface (voice, UI, mobile) | `voice/`, `ui/`, or a client | be a thin adapter (INV-3); touch no `core/` logic |
| Understanding / intent logic | `ai/` | keep the `resolve()` interface stable (INV-8); emit structured `Intent` |
| A new capability / plugin | `plugins/` | self-register against intents (INV-6); declare a power tier (INV-9) |
| A cross-boundary message | `proto/` + regenerate | be append-only (INV-2); never hand-edit `generated/` |
| Hot-path execution / engine | `core/` | stay transport-agnostic (INV-1) |
| A runtime tunable | `config/` (once it exists) | not be hardcoded in a source file |
| Durable/semantic memory | `memory/` | expose an interface; no cross-tier storage reach-in (INV-11) |

---

## 4. Process rules

- **The hard gate is satisfied.** `docs/vision.md` required a full architectural planning pass
  before further implementation. That pass produced the blueprint + this file. Implementation may
  now resume — but every step obeys them.
- **Plan before code.** For anything beyond a single focused change, produce a step-by-step plan
  (in `roadmap.md` for a scheduled phase; in-conversation otherwise) *before* writing code, and
  state which pipeline stage and which invariants it touches. (Reinforces the global config.)
- **Respect architectural dependency, not just phase number.** The Capability Registry (blueprint
  S2) is prior to most capability growth even though Voice (Phase 3) is scheduled earlier. Don't
  build capability sprawl on a missing registry.
- **Observability is part of "done" (blueprint §4.4).** A new tier isn't complete until every
  cross-boundary call logs request-id, operation, latency, and outcome — on **both** the C++ and
  Python sides.
- **After every feature, update the feature checklist. Not optional.** `docs/features.md` is the
  single source of truth for "what's built / in progress / to do." The moment a feature is
  finished, started, or descoped, update its status there (✅ Done · 🚧 In progress · 📋 To do)
  in the same change — and add/adjust the step detail in `roadmap.md` if it's the active phase.
  A change that ships a feature but leaves the checklist stale is **not complete**. When you
  finish work, re-read `features.md` and reconcile it with reality before declaring done.
- **Keep the docs coherent.** `architecture-blueprint.md` = the architecture (invariants +
  spine); `features.md` = the canonical checklist; `roadmap.md` = the step-by-step schedule for
  the active phase; `vision.md` = the parking lot. When a design decision is made, fold it back
  into the blueprint so it stays current rather than rotting into a snapshot.
- **Cite invariants in review.** When rejecting or flagging a change, name the invariant
  (e.g. "violates INV-6 — edits the dispatcher instead of registering"). It keeps the rules
  operative rather than decorative.

---

## 5. Build / run quick reference

- Build C++: `cmake -S . -B build && cmake --build build` → `build/jarvis` (CLI),
  `build/jarvis_grpc_server` (gRPC :50051).
- Full stack: start `python3 ai/jarvis_ai_server.py` (:50052) first, then
  `build/jarvis_grpc_server`, then `python3 tools/grpc_smoke_test.py`.
- Regenerate stubs only when `proto/*.proto` changes (commands in `README.md`).
- The CLI (`jarvis`) and the server (`jarvis_grpc_server`) are **separate binaries** sharing
  `engine`/`command_handler` sources — `exit` only means anything in the CLI.

---

## 6. University project — this is a QMUL final-year project, not only a personal one

JARVIS is a submitted **BSc EECS Final Year Project** (worth 15% of the degree). That imposes
rules that override convenience, and a documentation duty that runs alongside implementation.

### 6.1 `qmul/` is off-limits by default — HARD RULE

The `qmul/` directory holds university coursework (handbooks, notes, project-definition drafts,
the Gen AI log). It is gitignored and academic-integrity-sensitive.

- **Do NOT read, edit, move, create, or reference anything under `qmul/` unless the user's
  current request explicitly asks you to.** A general task ("resume Phase 2", "refactor the
  engine") is **not** permission to touch `qmul/`.
- The **only** standing exception is the contemporaneous documentation duty in §6.2 — and only
  the two specific files/dirs named there, nothing else in `qmul/`.

### 6.2 Documentation that MUST accompany implementation (the standing exception)

The handbook mandates *contemporaneous* records (they cannot be reconstructed in April and are
source material for the report and viva). Whenever you do implementation or design work on
JARVIS, in the same session:

1. **Gen AI usage log** — append a dated entry to `qmul/notes/genai-usage-log.md` using that
   file's template. Any AI involvement in repo code is **Category C (highest risk)** and must
   record how it was verified and whether it is defensible. This log exists to document AI use
   honestly; keeping it current is required for the accountability statement submitted with the
   report. Add the entry the **same day**; newest last.
2. **Engineering logbook** — append a **factual** entry to `qmul/logbook/` (see its README):
   what was built/changed, the decisions and *why*, problems hit and how you routed around them,
   and how it was verified. This feeds the assessed 10% "troubleshooting & resourcefulness"
   narrative. Keep it factual and technical — the *reflective* framing and the final report
   prose are the student's own to write (§6.3).

These two are the extent of the standing exception. Do not create other files under `qmul/`.

### 6.3 Academic-integrity guardrails — never cross these

- **Defensibility.** The viva probes implementation to verify authorship: *anything in the repo
  the student cannot explain and defend at a whiteboard, to a non-expert examiner, is a
  liability.* Prefer clarity the student can own over cleverness they can't. If a change would
  leave the student unable to explain it, flag that explicitly rather than shipping it quietly.
- **No submittable prose.** Do not write report/project-definition/showcase text for submission.
  Provide structure, scaffolds, and the student's own technical facts — mark every substantive
  section `[TO WRITE]`. Submitted content "should not be the product of a tool."
- **Never fabricate references or citations.** Generating *search terms* is permitted; inventing
  *citations* is academic misconduct. Do not output a citation as though verified.

### 6.4 Why the engineering rules also earn marks (context, not extra work)

The invariants already pull in the same direction as the rubric — worth knowing so quality
isn't treated as optional: clean, phased, well-messaged **commits** are cited as assessable
engagement evidence (5%); **INV-13** testing/evaluation is the 13% Evaluation component;
**architecture diagrams** are marked artefacts in the 12% Writing component; the
rule-classifier-vs-local-LLM **confidence-deferral** design is the measurable core contribution
carrying the 35% Achievement + 10% Ambition; and **INV-11** (local-first/privacy) plus energy
cost is rich material for the LSES/sustainability strand. Building it right *is* the grade.

---

**Read the blueprint before designing anything structural:**
[`docs/architecture-blueprint.md`](docs/architecture-blueprint.md).
