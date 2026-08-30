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

Capability makeEchoCapability() {
    return Capability{
        "echo",
        CommandType::ECHO,
        "Echoes the input back to the user. Usage: echo [text]",
        PowerTier::T0_READ_ONLY,
        [](const std::string& payload, ExecutionContext& /*context*/) -> std::string {
            return payload;
        }
    };
}

Capability makeAboutCapability() {
    return Capability{
        "about",
        CommandType::ABOUT,
        "Provides information about JARVIS. Usage: about",
        PowerTier::T0_READ_ONLY,
        [](const std::string& /*payload*/, ExecutionContext& /*context*/) -> std::string {
            return "JARVIS Core Engine v1.0\nDeveloped by Rayyan.";
        }
    };
}

void registerBuiltinCapabilities(CapabilityRegistry& registry) {
    registry.registerCapability(makeEchoCapability());
    registry.registerCapability(makeAboutCapability());
}
