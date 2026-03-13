# JARVIS Roadmap

This roadmap focuses on the next architecture step: moving from a CLI-only prototype to a service-oriented hybrid foundation.

## Phase 1 - Service Foundation (Detailed)

Phase 1 objective:

- Keep the current CLI working.
- Extract engine logic so it can be called without terminal coupling.
- Introduce first typed API boundary between C++ and Python.

### Step 0 - Baseline and safety checks

Goal:

- Freeze known-good behavior before refactoring.

Tasks:

1. Record current commands and expected outputs.
2. Add a small manual test checklist for `echo`, `help`, `about`, `status`, `exit`.
3. Confirm clean build before every major change.

Learning outcome:

- Understand regression prevention during architecture changes.

### Step 1 - Define engine-facing data model in C++

Goal:

- Replace terminal-oriented assumptions with explicit request/response data.

Tasks:

1. Introduce a command request model (`command`, optional payload/metadata).
2. Introduce a command response model (`success`, `message`, optional structured fields).
3. Keep existing parser but route to data models internally.

Learning outcome:

- Learn how typed interfaces reduce hidden coupling.

### Step 2 - Refactor engine to be transport-agnostic

Goal:

- Make core engine callable from CLI, gRPC, tests, and future UI with the same methods.

Tasks:

1. Add engine API methods such as `executeCommand(...)` and `getStatus()`.
2. Move business logic out of direct `std::cout` usage and return responses instead.
3. Keep CLI as thin adapter that prints returned responses.

Learning outcome:

- Learn separation between domain logic and I/O adapters.

### Step 3 - Define first protobuf contract

Goal:

- Create language-neutral API schema.

Tasks:

1. Create proto service with minimal RPCs:
	- `ExecuteCommand`
	- `GetStatus`
2. Define message fields with explicit types and comments.
3. Add versioning note for backward compatibility (v1 namespace/package).

Learning outcome:

- Learn schema-first API design and compatibility discipline.

### Step 4 - Add C++ gRPC server adapter

Goal:

- Expose engine API over gRPC without changing engine internals.

Tasks:

1. Implement gRPC service class that calls engine methods.
2. Map proto requests to engine request objects.
3. Map engine responses back to proto responses.
4. Start server locally and verify endpoint reachability.

Learning outcome:

- Learn adapter pattern between transport layer and core logic.

### Step 5 - Add first Python client/worker

Goal:

- Validate C++/Python interoperability through the contract layer.

Tasks:

1. Generate Python stubs from the same proto file.
2. Implement Python client that calls `ExecuteCommand` and `GetStatus`.
3. Compare Python-driven outputs with CLI outputs for parity.

Learning outcome:

- Learn cross-language integration via shared contracts.

### Step 6 - Add observability baseline

Goal:

- Make hybrid debugging practical from day one.

Tasks:

1. Add structured logs for request id, command, latency, and result.
2. Add basic counters for successful and failed commands.
3. Add a simple health endpoint or heartbeat log.

Learning outcome:

- Learn why visibility is mandatory in multi-layer systems.

### Step 7 - Add hardening guardrails

Goal:

- Prepare foundation for network/device growth.

Tasks:

1. Add request validation (empty/invalid commands, payload limits).
2. Add per-request timeout handling.
3. Add clear error codes and user-friendly error messages.

Learning outcome:

- Learn reliability patterns used in production service design.

## Does existing code need drastic changes?

Short answer: no.

What stays valuable:

1. Command parsing strategy.
2. Command enum and dispatch map pattern.
3. Engine loop and status tracking concepts.

What needs refactoring:

1. Move direct output concerns out of core logic.
2. Expose command execution through methods that return typed responses.
3. Add a service adapter layer rather than invoking logic from terminal-only flows.

## Later phases (high-level)

1. Phase 2: Async event bus and worker orchestration.
2. Phase 3: Device adapters and network control.
3. Phase 4: Persistent memory and retrieval.
4. Phase 5: Voice and UI clients as API consumers.