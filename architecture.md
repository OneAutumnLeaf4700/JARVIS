# JARVIS Architecture

## Overview

JARVIS is designed as a hybrid system:

- **C++ Core Engine**: Handles system control, plugin management, command routing, and performance-critical tasks.  
- **Python AI Layer**: Handles LLM reasoning, natural language processing, speech recognition, and embeddings.

This document describes the **conceptual architecture only**. It is meant to guide design and learning before any implementation work begins.

---

## Module Responsibilities

**Core Engine (C++):**
- Plugin manager
- Command router
- OS integration
- System-level execution

**AI Layer (Python):**
- Local LLM interactions
- Speech recognition (Whisper/Vosk)
- Text-to-speech (Piper/Coqui)
- Memory management and embeddings

**Plugins:**
- Modular commands and actions
- Examples: system control, reminders, search, music

**Voice Module:**
- Microphone input
- Wake word detection
- Speech-to-text and text-to-speech

**Memory System:**
- Store and retrieve user information
- Semantic search via embeddings

---

## Communication Flow

High-level request/response flow:

User voice / CLI input  
↓  
Speech recognition (for voice input)  
↓  
Command parsing and intent understanding  
↓  
Plugin selection and execution  
↓  
System actions and response (voice / text)


Communication between C++ core and Python AI layer is via **gRPC** (or REST / local sockets).

---

## Plugin Interface

Each plugin follows a standard conceptual interface:

```text
plugin:
  name
  description
  commands
  execute(command, context) -> result
```

At a high level:

- `name`: Human-readable identifier for the plugin.  
- `description`: Short summary of what the plugin can do.  
- `commands`: List of supported commands/intents the plugin understands.  
- `execute(...)`: Entry point called by the core engine when a matching command is routed to this plugin.

Plugins are planned to be dynamically loaded by the core engine so that new capabilities can be added without changing the core.