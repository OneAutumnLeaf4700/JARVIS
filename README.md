# JARVIS – Local AI Assistant

Local AI-powered personal assistant inspired by Iron Man's JARVIS.  
Designed to run fully locally with a modular plugin architecture.

**Status:** Active prototype phase. C++ CLI core is implemented and being migrated toward a service-oriented hybrid architecture.

---

## Features

- Voice interaction (planned)
- Natural language commands
- Local LLM integration
- System automation
- File search and productivity tools
- Plugin-based modular architecture
- Persistent memory for contextual recall

---

## Architecture

JARVIS uses a hybrid architecture with a long-term service-first direction:

- **C++ Core Service** for engine state, command execution, policy, and performance-critical workflows
- **Python Worker Layer** for AI orchestration, NLP/LLM tasks, and rapidly evolving integrations
- **Contract Layer (Protobuf/gRPC)** for typed, versioned communication between C++ and Python

Communication between layers is planned primarily via **gRPC**.

High-level flow:

- Client input (CLI/voice/UI)  
- Typed request into C++ service API  
- C++ executes directly or delegates async work to Python workers  
- Result/events returned to client layer



---

## Technologies

- Programming Languages: **C++**, **Python**
- Local LLM: Ollama / llama.cpp
- Speech Recognition: Whisper, Vosk
- Text-to-Speech: Piper TTS, Coqui TTS
- Memory Storage: SQLite, Chroma / FAISS (for embeddings)
- Communication: gRPC / REST
- GUI (future): Qt / Tauri / Web dashboard

---

## Roadmap

**Phase 1 – Service Foundations (Current next target)**  
- Keep current CLI prototype running while introducing service boundaries
- Refactor core logic so CLI is only one client of the engine
- Define first protobuf contracts (`ExecuteCommand`, `GetStatus`)
- Add initial local gRPC interface in C++
- Add first Python client/worker that talks to C++ over gRPC

**Phase 2 – Voice Assistant**  
- Microphone input
- Wake word detection
- Speech-to-text
- Text-to-speech

**Phase 3 – Plugin Expansion**  
- Reminders
- File search
- System automation
- Media control

**Phase 4 – Persistent Memory**  
- Store user data and context
- Vector embeddings for semantic recall

**Phase 5 – GUI / Dashboard**  
- Optional visual interface
- Interaction history
- System monitoring

---

## Project Structure

Planned layout:

```text
jarvis/
├─ core/      # C++ engine and plugin manager
├─ ai/        # Python AI layer (LLM, memory, embeddings)
├─ voice/     # Speech-to-text, text-to-speech, wake word
├─ plugins/   # Modular plugins (system control, reminders, search)
├─ ui/        # Future GUI / dashboard
├─ docs/      # Architecture, roadmap, ideas
├─ tools/     # Scripts and utility tools
├─ README.md
├─ LICENSE
```

---

## Goals

- Fully local AI assistant
- Modular and extensible plugin system
- Voice-first interaction model
- Intelligent automation and personal productivity tools

## Feature Checklist

For a detailed, tickable list of planned features, see [docs/features.md](docs/features.md).  
Use it alongside the roadmap to track progress and add new ideas as you learn.

## How to read these docs while learning

- **Start with this README** to understand the overall vision and current status.  
- **Read [docs/architecture.md](docs/architecture.md)** next for a deeper look at service boundaries and C++/Python role split.  
- **Check [docs/roadmap.md](docs/roadmap.md)** for the detailed Phase 1 implementation and learning sequence.