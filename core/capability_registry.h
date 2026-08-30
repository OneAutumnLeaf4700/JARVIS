#pragma once

#include <optional>
#include <string>
#include <unordered_map>

#include "capability.h"

// Resolves an intent (CommandType) to the capability that handles it. Replaces the old
// hardcoded COMMAND_DISPATCH/COMMAND_DESCRIPTIONS maps in command_handler.cpp — capabilities
// register themselves here instead of being hand-added to a central table.
class CapabilityRegistry {
 public:
    void registerCapability(Capability capability);

    // Returns nullptr if no capability answers this intent. CommandType::UNKNOWN is never
    // registered, so resolving it always returns nullptr — "no match" stays a real, distinct
    // outcome, not a capability.
    const Capability* resolve(CommandType intent) const;

    // resolve() + execute() in one call. std::nullopt means resolve() would have returned
    // nullptr, so callers can tell "ran, returned this" apart from "nothing to run".
    std::optional<std::string> dispatch(CommandType intent, const std::string& payload,
                                         ExecutionContext& context) const;

    // For `help` to enumerate what's registered, and for tests.
    const std::unordered_map<CommandType, Capability>& all() const;

 private:
    std::unordered_map<CommandType, Capability> capabilities_;
};

// One explicit call site registers every built-in capability. Adding a new one means writing
// it and adding one line here — nothing else in the dispatch path changes.
void registerBuiltinCapabilities(CapabilityRegistry& registry);

// Builtin capability factories — exposed for direct testing (see tests/capability_registry_test.cpp).
Capability makeEchoCapability();
Capability makeAboutCapability();
Capability makeStatusCapability();
Capability makeHelpCapability();
