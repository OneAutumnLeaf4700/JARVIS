# JARVIS Feature Checklist

This checklist breaks the project into concrete, trackable features.  
Use it to:

- [ ] Plan what to build next  
- [ ] Tick items off as you implement them  
- [ ] Add new ideas as they come up  

This checklist tracks both completed prototype milestones and upcoming work.

---

## Phase 1 – CLI Assistant

### Core Engine (C++)
- [x] Basic command-line interface (CLI) entry point
- [x] Core request/response loop (read → process → respond)
- [x] Command router to dispatch user intents
- [ ] Plugin manager for loading and unloading plugins
- [ ] Configuration system for basic settings (e.g., paths, models)

### Plugins (Initial Set)
- [ ] System control plugin (e.g., open application, shutdown, volume)
- [x] Help / introspection command support (includes `help` and `help <command>`)
- [x] Simple utility command support (`echo`, `about`, `status`)

### Reliability & UX
- [ ] Structured logging for core and plugins
- [ ] Graceful error handling and user-friendly error messages

### Current Command Set Snapshot
- [x] `echo`
- [x] `help`
- [x] `help <command>`
- [x] `about`
- [x] `status` (engine state, uptime, last command)
- [x] `exit`

---

## Phase 1b – Service Foundation (Next)

### Core Refactor
- [ ] Extract transport-agnostic engine API (`executeCommand`, `getStatus`)
- [ ] Move direct console printing out of core domain logic
- [ ] Keep CLI as a thin adapter to engine responses

### Contract Layer
- [ ] Define first protobuf schema (`ExecuteCommand`, `GetStatus`)
- [ ] Add request/response versioning notes

### C++ Service Layer
- [ ] Add local gRPC server around engine API
- [ ] Add request validation and timeout handling

### Python Integration
- [ ] Generate Python gRPC client from proto
- [ ] Build first Python worker/client calling C++ service
- [ ] Validate parity between CLI and Python-driven commands

---

## Phase 2 – Voice Assistant

### Input & Wake Word
- [ ] Microphone input capture
- [ ] Wake word detection (e.g., “Jarvis”) to start listening
- [ ] Configurable push-to-talk / always-listen mode

### Speech-to-Text (STT)
- [ ] Integrate local STT engine (e.g., Whisper / Vosk)
- [ ] Map transcribed text into the same flow as CLI commands
- [ ] Handle low-confidence transcripts gracefully (ask for clarification)

### Text-to-Speech (TTS)
- [ ] Integrate local TTS engine (e.g., Piper / Coqui)
- [ ] Configurable voice and speaking rate
- [ ] Fallback to text-only responses if audio fails

---

## Phase 3 – Plugin Expansion

### Productivity & Utilities
- [ ] Reminders and basic calendar integration
- [ ] File search plugin (by name and simple content)
- [ ] Notes / TODO plugin for quick capture and recall

### Media & System Control
- [ ] Media control plugin (e.g., play/pause, next track)
- [ ] Integration with a music service (e.g., Spotify or local player)
- [ ] Advanced system automation (scripts, shortcuts, macros)

---

## Phase 4 – Persistent Memory

### Structured Data
- [ ] Local database for structured memory (e.g., SQLite)
- [ ] Schemas for users, sessions, and saved items (notes, reminders, etc.)

### Semantic Memory
- [ ] Embedding generation for relevant items (notes, history)
- [ ] Vector store (e.g., Chroma / FAISS) for semantic search
- [ ] Retrieval of past context to improve responses

### Privacy & Control
- [ ] Clear controls for inspecting and deleting stored data
- [ ] Configurable retention policies (e.g., auto-expire old items)

---

## Phase 5 – GUI / Dashboard

### Core UI
- [ ] Local dashboard (Qt / Tauri / Web) to interact with JARVIS
- [ ] View interaction history and search past conversations
- [ ] Visualize loaded plugins and their status

### Monitoring & Settings
- [ ] Basic system status (CPU, memory, active tasks)
- [ ] Settings panel for models, audio devices, and privacy options

---

## AI Layer (Cross-Cutting)

These features span multiple phases and rely on the Python AI layer.

- [ ] Local LLM integration (e.g., Ollama / llama.cpp)
- [ ] Natural language command parsing and intent detection
- [ ] Simple multi-turn context handling (remember recent exchanges)
- [ ] Safe-guardrails for potentially harmful commands (confirmation prompts)

---

## Your Ideas & Stretch Goals

Use this space to record extra ideas as you learn and build:

- [ ] Multi-device support (e.g., mobile companion)
- [ ] Home automation integrations (e.g., Home Assistant)
- [ ] Advanced scheduling and routines
- [ ] Custom wake word training

