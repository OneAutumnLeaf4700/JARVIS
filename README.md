# JARVIS – Local AI Assistant

Local AI-powered personal assistant inspired by Iron Man's JARVIS.  
Designed to run fully locally with a modular plugin architecture.

**Status:** Early design phase – documentation and architecture only, no implementation yet.

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

JARVIS uses a hybrid architecture:

- **C++** for the core engine, plugin management, and system control  
- **Python** for the AI layer, LLM reasoning, speech recognition, and embeddings

Communication between layers is planned via **gRPC** (or REST / local sockets).

High-level flow:

- Microphone / CLI input  
- Speech recognition (for voice)  
- Command parsing and intent understanding  
- Plugin selection and execution  
- System actions and response (voice / text)



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

**Phase 1 – CLI Assistant**  
- Simple command-line input
- Core engine and plugin system  
- Basic plugin execution

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

For a detailed, tickable list of planned features, see `docs/features.md`.  
Use it alongside the roadmap to track progress and add new ideas as you learn.

## How to read these docs while learning

- **Start with this `README`** to understand the overall vision and phases.  
- **Read `architecture.md`** next for a deeper look at how the pieces fit together conceptually.  
- **Check `docs/roadmap.md`** to see the planned order of work and think through what each phase would require before writing any code.