# JARVIS Vision & Milestone Brainstorm

**This is a brainstorming/parking-lot document, not a plan.** Nothing here is scheduled or committed. When development on a new stage resumes, this is the raw material to feed into Claude Code **Plan Mode** to derive an actual timeline — not something to implement directly from.

> **Hard gate — ✅ SATISFIED.** Before any further implementation work resumes on JARVIS, we MUST run a full architectural / system design planning pass to confirm the current design is the right foundation for everything below. **That pass is done:** its output is [`architecture-blueprint.md`](architecture-blueprint.md) (the full core→scaled design + twelve invariants) and the root [`CLAUDE.md`](../CLAUDE.md) (the enforceable rules). Implementation may now resume — but every step obeys those two documents. This vision doc remains the parking lot upstream of them.

Ideas get added here in whatever shape they arrive — full paragraphs, half-formed bullets, one-liners. Cleanup/structuring happens later, during the planning pass, not at capture time.

---

## Core interaction loop (the mental model)

1. **Input capture** — multiple modalities, not just typed prompts:
   - Text prompt (what exists today)
   - Voice recognition (hardware/wake-word driven)
   - Gesture recognition
   - Others TBD as they come up
2. **Parsing / understanding pipeline** — the input gets converted into something actionable. Multiple strategies depending on what's online:
   - **Basic mode** — only lightweight/local capability available (e.g. voice hardware online, backend LLM not running). Falls back to natural-language *mapping* (pattern/keyword-driven, no model) to find the closest known command.
   - **Full mode** — backend model available (via Ollama). User input is translated into a suitable prompt (or passed through more directly) to the LLM, which breaks the request down and maps it onto a concrete JARVIS command/service.
   - There should be a registry/mapping system that connects natural-language intents/answers to their own complementary JARVIS services — i.e. a formal way to bind "what the LLM decided the user wants" to "the actual service/plugin that fulfils it." This is the same shape as the intent-classifier work in `roadmap.md`, just described end-to-end here.
3. **Execution** — the mapped command runs through the existing C++ engine / plugin path.
4. **Response** — output via whatever's appropriate (speakers, text, GUI, etc.)

### Graceful "I can't do that yet" path
When a command/request has no mapped capability:
- JARVIS should say so out loud/visibly ("I don't currently support the ability to ...") rather than failing silently or erroring.
- It should be able to *smartly suggest* adding the missing capability to its own development roadmap.
- Stretch: JARVIS could trigger a secondary pipeline that starts drafting a plan for the missing feature using a development tool of its own — i.e. JARVIS participating in its own feature development loop. This is a genuinely novel/ambitious idea worth its own design spike later, not a small feature.

---

## Near-term capability targets (once resumed)

These are the "next tier up from CLI" capabilities — closer to reality than the smart-home stuff below:

- **Desktop interaction**
  - Open a browser and execute a given command/task in it
  - Take a screenshot of the desktop on command
  - Explain what's happening on screen / in the background (screen understanding, likely vision-model-backed)
- These effectively make JARVIS an OS-level agent, not just a command dispatcher — implies new plugin categories (desktop automation, screen capture + vision) beyond what's currently scoped in `features.md` Phase 4.

## Longer-term / smart environment targets

- Smart home integration — temperature control, lighting, etc. (aligns with the existing "smart environment" stretch entry in `features.md`, but called out here as a real target, not just a maybe)
- Broader ambient control as more devices/services get bound into the same intent → service mapping system described above

---

## Open questions to resolve during the architecture pass

- How does the natural-language → service mapping registry actually get defined? (static config, plugin self-registration, LLM-generated mapping suggestions?)
- Where does "basic mode" pattern-matching live relative to the full LLM path — same interface, different backend? (Likely yes, given the existing C++/Python split, but worth confirming against the Phase 2 intelligence-layer plan in `roadmap.md`.)
- What's the security/consent model for desktop control and smart-home control specifically — these are qualitatively more sensitive than CLI plugins and probably deserve their own guardrail tier.
- Does "JARVIS drafts its own feature plans" become an actual pipeline, or stay a novelty idea? Needs a scoping decision before it's anything more than this note.

---

## Relationship to existing docs

- [`architecture-blueprint.md`](architecture-blueprint.md) — the binding architecture (pipeline, invariants, core→scaled)
- [`roadmap.md`](roadmap.md) — detailed step-by-step execution plan for the current + next phase
- [`features.md`](features.md) — full phase-by-phase checklist, done → stretch goals, with status indicators

This document sits *above* all of those — it's where raw ideas land before they're structured enough to belong in `features.md` or promoted into a concrete phase in `roadmap.md`. Keep adding to it freely; don't self-edit into roadmap format at capture time.
