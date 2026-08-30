#include "capability_registry.h"

void CapabilityRegistry::registerCapability(Capability capability) {
    capabilities_[capability.intent] = std::move(capability);
}

const Capability* CapabilityRegistry::resolve(CommandType intent) const {
    auto it = capabilities_.find(intent);
    if (it == capabilities_.end()) {
        return nullptr;
    }
    return &it->second;
}

std::optional<std::string> CapabilityRegistry::dispatch(
    CommandType intent, const std::string& payload, ExecutionContext& context) const {
    const Capability* capability = resolve(intent);
    if (!capability) {
        return std::nullopt;
    }
    return capability->execute(payload, context);
}

const std::unordered_map<CommandType, Capability>& CapabilityRegistry::all() const {
    return capabilities_;
}

// Real body added in Task 2 — declared in capability_registry.h, defined here so this file
// compiles and links standalone. Empty is correct for this task: no capability exists yet.
void registerBuiltinCapabilities(CapabilityRegistry& /*registry*/) {
}
