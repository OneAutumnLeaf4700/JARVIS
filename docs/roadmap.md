# JARVIS Roadmap

This roadmap outlines the **planned phases** for JARVIS.  
Use it to reason about design and dependencies between features before writing any code.

## Phase 1 – CLI Assistant
- Implement core engine (C++)
- Add command router
- Add plugin manager
- Create first plugin (system control)
- Test CLI commands end-to-end

## Phase 2 – Voice Assistant
- Integrate microphone input
- Implement wake word detection
- Connect speech-to-text
- Add text-to-speech responses

## Phase 3 – Plugin Expansion
- Reminders and calendar
- File search
- Media control (Spotify, music)
- System automation

## Phase 4 – Persistent Memory
- SQLite for structured data
- Vector embeddings for semantic memory
- Enable context-based responses

## Phase 5 – GUI / Dashboard
- Optional visual interface (Qt/Tauri/Web)
- Display interaction history
- Visualize system status and plugins

## Learning with the roadmap

- **Before each phase**, sketch the data flow and responsibilities between modules.  
- **Identify dependencies** (e.g., plugins depend on the core engine) and note what must exist first.  
- **Write down questions** you have about each phase; answering them is part of your learning process before you start implementing.