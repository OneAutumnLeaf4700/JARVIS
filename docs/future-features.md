# Future Features — Long-Horizon Plan

This document is the long view. `roadmap.md` and `phase2-roadmap.md` are detailed *next-step* plans; `features.md` is a tickable checklist; this doc is the narrative connecting them — what each future feature actually does, what it depends on, and what concept it exists to teach.

The ordering is roughly dependency-aware: things higher up unblock things lower down. It is not a strict timeline.

---

## Tier 1 — Intelligence layer (Phase 2 and 2.5)

### Rule-based intent classification (next)
Detailed in [`phase2-roadmap.md`](phase2-roadmap.md). The point isn't the classifier itself — it's establishing the structured-AI-response pattern that everything later plugs into.

### Local LLM integration
**What:** Replace (or fall back from) the rule classifier with a local model via Ollama or llama.cpp.
**Depends on:** Phase 2 complete — the `classify()` interface and structured response shape are already in place.
**Concept taught:** Strategy pattern across runtimes; latency-vs-accuracy trade-offs; why a fast deterministic path in front of a slow probabilistic one is the standard production layout.

### Multi-turn context
**What:** Maintain a per-session conversation history on the Python side so follow-ups like "and what about yesterday?" resolve correctly.
**Depends on:** LLM integration; some form of session id propagated from C++.
**Concept taught:** State management across stateless RPC calls; session affinity; why the contract layer needs to carry identity even when the service itself is stateless.

### Safety guardrails
**What:** Confirmation prompts before potentially destructive intents (delete file, shut down, etc.); allowlists of safe operations.
**Depends on:** A real plugin doing destructive things (Tier 3).
**Concept taught:** Defence-in-depth — guards in the contract, the dispatcher, and the plugin itself, not just one layer.

---

## Tier 2 — Voice I/O (Phase 3)

### Microphone capture + wake word
**What:** Always-listening loop that fires only on a wake word ("Jarvis").
**Depends on:** A wake-word library (Porcupine / openWakeWord) and a clean way for the voice process to call the C++ service.
**Concept taught:** Real-time audio handling, the cost of always-on processes, why wake words exist (cheap gate in front of an expensive STT).

### Speech-to-text (Whisper / Vosk)
**What:** Convert detected speech into text and send it as the payload of a `COMMAND_TYPE_UNKNOWN` request — the existing pipeline handles the rest.
**Depends on:** Wake-word capture.
**Concept taught:** How a new input modality plugs into an existing service contract without changing the contract — the value of designing for this from day one.

### Text-to-speech (Piper / Coqui)
**What:** Speak responses out loud instead of printing them.
**Depends on:** Nothing — orthogonal to STT.
**Concept taught:** Output adapters; why the response shape (`message: string`) was chosen so it works for any renderer (terminal, voice, GUI).

### Push-to-talk vs. always-listen
**What:** Configurable mode switching.
**Depends on:** Microphone capture.
**Concept taught:** Configuration-driven behaviour; runtime modes vs. compile-time modes.

---

## Tier 3 — Plugins and integrations (Phase 4)

The plugin manager itself is a prerequisite for everything in this tier.

### Plugin manager
**What:** Discover, load, and dispatch to plugins at runtime — each plugin registers one or more intent handlers.
**Depends on:** Phase 2 intent classifier (so plugins can register against intents, not raw command strings).
**Concept taught:** Dynamic registration patterns; why most extensible systems are essentially "intent → handler" maps with a discovery layer on top.

### System control plugin
**What:** Open applications, control volume, lock screen, shutdown.
**Depends on:** Plugin manager + safety guardrails.
**Concept taught:** Crossing OS boundaries safely; why every "shutdown" command needs a confirmation step even if the user asked nicely.

### File search plugin
**What:** Find files by name and (later) by content.
**Depends on:** Plugin manager.
**Concept taught:** Local indexing trade-offs (live walk vs. cached index); why search is rarely a one-shot operation.

### Reminders / notes / TODO plugin
**What:** Capture and recall short pieces of information.
**Depends on:** Plugin manager + structured persistence (Tier 4).
**Concept taught:** The split between transient state (in memory) and durable state (in storage) — and why that split tends to leak unless designed for.

### Media control plugin
**What:** Play/pause, next track, volume; eventually integration with a service like Spotify.
**Depends on:** Plugin manager.
**Concept taught:** OS-level media APIs vs. service-level APIs; how authentication tokens get handled in a local-first system.

### Calendar / scheduling
**What:** Read and create calendar events, possibly via CalDAV or a provider API.
**Depends on:** Plugin manager + a credential store.
**Concept taught:** External API integration patterns — rate limits, retry backoff, token refresh.

---

## Tier 4 — Persistent memory (Phase 5)

### Local SQLite store
**What:** A schema for users, sessions, captured items (notes, reminders, history).
**Depends on:** Plugin manager (so plugins have a place to persist).
**Concept taught:** Schema design; migration discipline; why even a tiny SQLite database benefits from a migration tool.

### Embedding generation
**What:** Compute embeddings for stored notes, conversation history, and other text.
**Depends on:** Local SQLite store + LLM/embedding model integration.
**Concept taught:** Vector representations of text; embedding model choice trade-offs (size vs. quality vs. inference cost).

### Vector store + semantic recall
**What:** Chroma or FAISS index over the embeddings; "remind me what we talked about regarding X".
**Depends on:** Embedding generation.
**Concept taught:** Approximate nearest-neighbour search; why semantic search complements but doesn't replace keyword search.

### Privacy controls
**What:** Inspect, export, and delete stored data; configurable retention.
**Depends on:** SQLite store.
**Concept taught:** Privacy-by-design; the difference between deleting a row and actually erasing data (think embeddings derived from it).

---

## Tier 5 — UI and dashboard (Phase 6)

### Local dashboard
**What:** A web or desktop UI showing interaction history, plugin status, system stats.
**Depends on:** Nothing critical — the gRPC contract is already enough to build a UI against.
**Concept taught:** Yet another client of the same service contract — proves the architecture wasn't an over-engineered abstraction.

### Settings panel
**What:** Switch models, audio devices, intent thresholds.
**Depends on:** A configuration system in the C++ core.
**Concept taught:** Live-reloading config vs. restart-required config; the cost of each.

### System monitoring
**What:** CPU, memory, active task display.
**Depends on:** Local dashboard.
**Concept taught:** Lightweight metrics collection; why pulling metrics is often easier than pushing them.

---

## Tier 6 — Stretch / experimental

These are explicitly *might never happen* — they exist to mark where curiosity could pull the project later.

### Multi-device support
A mobile companion that talks to the C++ service over the network. Forces real authentication and TLS, which the local-only setup avoids.

### Home automation integrations
Home Assistant bridge — JARVIS becomes a voice front-end for an existing automation graph. Concept: when not to reinvent.

### Custom wake-word training
Train a small model on your own voice saying "Jarvis". Concept: the data side of ML, not just the inference side.

### Advanced routines
Time-based and event-based action chains ("every weekday at 8am, summarise my calendar"). Concept: scheduling primitives and event sourcing.

---

## How to use this document

When you finish a roadmap phase and aren't sure what to pull off the shelf next:

1. Look at what's freshly unblocked — anything in a tier whose dependencies just turned green.
2. Pick whichever item has the strongest learning outcome for the concept you most want to learn next, not necessarily the most "useful" feature.
3. Promote it into a real roadmap doc (`phaseN-roadmap.md`) with the same per-step structure as `roadmap.md` and `phase2-roadmap.md`.

Don't treat this list as a backlog to grind through — treat it as a menu of well-shaped learning opportunities.
