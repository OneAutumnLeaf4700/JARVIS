#pragma once

#include <functional>
#include <string>

#include "command_handler.h"

// A capability's declared risk level. T0/T1 always run unconditionally; T2 is enforced via
// ConsentGate (requires a recorded grant, see core/consent_gate.h); T3/T4 enforcement is not
// yet implemented — ConsentGate denies them unconditionally until it lands.
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
    // Stable, extensible identifier used by plugins and new clients. Existing built-ins leave
    // this empty; CapabilityRegistry assigns their capability name as the intent on registration.
    std::string intentName;
};
