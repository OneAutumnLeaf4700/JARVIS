#pragma once

#include <functional>
#include <string>

#include "command_handler.h"

// A capability's declared risk level. Every capability today is T0 — nothing above T0 exists
// yet, so there's nothing to build/test real gate-enforcement logic against (see the design
// spec §1 for why enforcement is deliberately deferred).
enum class PowerTier {
    T0_READ_ONLY,
    T1_STATEFUL_LOCAL,
    T2_SYSTEM_AFFECTING,
    T3_DESTRUCTIVE,
    T4_EXTERNAL
};

class Engine;
class CapabilityRegistry;

// What a capability's execute() function can reach beyond its own payload. Most capabilities
// (e.g. echo) ignore this entirely; status reads engine, help reads registry.
struct ExecutionContext {
    Engine& engine;
    const CapabilityRegistry& registry;
};

// One thing JARVIS can do — a self-contained bundle of what it's called, what intent it
// answers to, what it does (for `help`), how risky it is, and the function that does the work.
struct Capability {
    std::string name;
    CommandType intent;
    std::string description;
    PowerTier powerTier;
    std::function<std::string(const std::string& payload, ExecutionContext& context)> execute;
};
