# JARVIS Architecture

## Why hybrid (C++ and Python)

JARVIS is designed as a hybrid system because each language is best at different jobs:

- C++ is best for long-running engine logic, low-latency command handling, and performance-critical runtime paths.
- Python is best for AI tooling, fast-moving integrations, and rapid experimentation.

The goal is not to mix them randomly. The goal is to split responsibilities cleanly and connect them through a stable contract.

## Current state and target state

Current state:

- Working C++ CLI prototype exists in the core layer.
- Command parsing and dispatch are implemented.
- Basic command set exists (`echo`, `help`, `about`, `status`, `exit`).

Target state:

- C++ becomes an always-on core service.
- Python runs as one or more worker/client processes.
- Both layers communicate through typed gRPC contracts.
- CLI, voice, and future UI become clients of the same service API.

## Why not direct function calls between C++ and Python

C++ and Python use different runtimes, memory models, and type systems. They do not directly call each other without a bridge.

Common bridge options:

1. Subprocess text I/O (stdin/stdout)
2. In-process bindings (for example pybind11)
3. Embedded Python interpreter in C++
4. RPC/service boundary (gRPC)

Chosen long-term approach: RPC/service boundary with gRPC.

Reason:

- It scales better to multi-process and multi-device systems.
- It gives explicit contracts and versioning.
- It supports strong observability and operational control.
- It allows each layer to evolve independently.

## Layered design

1. Core Service (C++)
- Owns runtime state and command execution.
- Enforces policy, auth checks, and safety guards.
- Publishes status/events.

2. Python Intelligence Layer
- Runs AI/NLP workflows.
- Handles external APIs and fast-changing business logic.
- Operates as workers that call C++ APIs and consume events.

3. Contract Layer (Protobuf/gRPC)
- Defines request/response schemas.
- Defines service APIs.
- Provides versioned compatibility between C++ and Python.

4. Client Layer (CLI/Voice/UI)
- Accepts user input.
- Sends structured requests to core service.
- Renders responses.

## High-level request flow

1. Client sends structured command to C++ service.
2. C++ either executes synchronously or delegates async work.
3. Python worker performs AI/integration tasks when needed.
4. Result is returned and optionally emitted as an event.

## Performance and reliability principles

- Keep hot paths in C++.
- Prefer binary serialization (protobuf) over ad-hoc text parsing across process boundaries.
- Use long-lived service connections, not per-command process startup.
- Use async queues with backpressure for non-blocking workloads.
- Add structured logging, metrics, and tracing early.

## Migration guidance from current code

The existing code does not need a drastic rewrite. It needs controlled refactoring.

Keep:

- Command enum and parse flow concepts.
- Dispatch model and command handlers.
- Engine state tracking patterns (for example uptime and status).

Refactor next:

- Separate pure engine logic from direct terminal I/O.
- Introduce a service-facing API (for example executeCommand/getStatus methods).
- Keep CLI as a thin client that calls that API.

This preserves your current progress while moving toward a scalable architecture.