#include <iostream>
#include <string>

#include "capability_registry.h"
#include "engine.h"
#include "plugin_config.h"

namespace {

// The only interactive consent surface (per the design spec §3.4) — CLI-only, since it's the
// one surface with a real terminal attached. Returns the process exit code.
int runGrantFlow(const std::string& capabilityName, const CapabilityRegistry& registry) {
    const Capability* capability = nullptr;
    for (const auto& [intent, cap] : registry.all()) {
        if (cap.name == capabilityName) {
            capability = &cap;
            break;
        }
    }

    if (capability == nullptr) {
        std::cout << "Unknown capability: " << capabilityName << "\n";
        std::cout << "Registered capabilities:\n";
        for (const auto& [intent, cap] : registry.all()) {
            std::cout << "  - " << cap.name << "\n";
        }
        return 1;
    }

    if (capability->powerTier == PowerTier::T3_DESTRUCTIVE ||
        capability->powerTier == PowerTier::T4_EXTERNAL) {
        std::cout << "'" << capabilityName << "' is power tier T3/T4 — enforcement isn't "
                     "implemented yet, so it cannot be granted.\n";
        return 1;
    }

    if (capability->powerTier != PowerTier::T2_SYSTEM_AFFECTING) {
        std::cout << "'" << capabilityName << "' is power tier T0/T1 — it doesn't require a "
                     "consent grant.\n";
        return 0;
    }

    std::cout << "Grant consent for '" << capabilityName << "' (power tier T2)? [y/n] ";
    std::string answer;
    std::getline(std::cin, answer);
    if (answer != "y" && answer != "Y") {
        std::cout << "Not granted.\n";
        return 0;
    }

    PluginConfig config = PluginConfig::load("config/capabilities.cfg", "config/consent_grants.cfg");
    config.grant(capabilityName);
    std::cout << "Granted.\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Engine engine;
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    if (argc == 3 && std::string(argv[1]) == "--grant") {
        return runGrantFlow(argv[2], registry);
    }

    PluginConfig pluginConfig = PluginConfig::load("config/capabilities.cfg", "config/consent_grants.cfg");
    registry.setPluginConfig(&pluginConfig);

    engine.run(registry);

    return 0;
}
