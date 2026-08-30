#include "capability_registry.h"

#include <iomanip>
#include <sstream>

#include "command_handler.h"
#include "consent_gate.h"
#include "engine.h"

static const std::string kExitDescription =
    "Terminates the JARVIS Core Engine. Usage: exit";

void CapabilityRegistry::registerCapability(Capability capability) {
    capabilities_[capability.intent] = std::move(capability);
}

void CapabilityRegistry::setPluginConfig(const PluginConfig* pluginConfig) {
    pluginConfig_ = pluginConfig;
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

    if (pluginConfig_ != nullptr) {
        if (!pluginConfig_->isEnabled(capability->name)) {
            return std::nullopt;
        }

        ConsentGate gate(*pluginConfig_);
        ConsentResult consent = gate.check(*capability);
        if (!consent.allowed) {
            return consent.reason;
        }
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

Capability makeStatusCapability() {
    return Capability{
        "status",
        CommandType::STATUS,
        "Shows engine state, uptime, and last command. Usage: status",
        PowerTier::T0_READ_ONLY,
        [](const std::string& /*payload*/, ExecutionContext& context) -> std::string {
            StatusInfo info = context.engine.getStatusInfo();

            const long hours   = info.uptimeSeconds / 3600;
            const long minutes = (info.uptimeSeconds % 3600) / 60;
            const long seconds = info.uptimeSeconds % 60;

            std::ostringstream out;
            out << "Engine: " << (info.running ? "running" : "stopped") << "\n";
            out << "Uptime: "
                << std::setfill('0') << std::setw(2) << hours   << ":"
                << std::setw(2)      << minutes << ":"
                << std::setw(2)      << seconds << "\n";
            out << "Last command: " << info.lastCommand;
            return out.str();
        }
    };
}

Capability makeHelpCapability() {
    return Capability{
        "help",
        CommandType::HELP,
        "Provides information about available capabilities. Usage: help [command]",
        PowerTier::T0_READ_ONLY,
        [](const std::string& payload, ExecutionContext& context) -> std::string {
            std::ostringstream out;

            if (payload.empty()) {
                out << "Available commands:\n";
                for (const auto& [intent, capability] : context.registry.all()) {
                    out << "  - " << capability.name << ": " << capability.description << "\n";
                }
                // exit stays outside the registry (engine-lifecycle control, not a
                // capability) but is still a real command the user can type — listed here
                // as one hardcoded line rather than being invented as a fake capability.
                out << "  - exit: " << kExitDescription << "\n";
                return out.str();
            }

            std::string commandName = toLower(payload);

            if (commandName == "exit") {
                return "exit: " + kExitDescription;
            }

            for (const auto& [intent, capability] : context.registry.all()) {
                if (capability.name == commandName) {
                    out << capability.name << ": " << capability.description;
                    return out.str();
                }
            }

            out << "Command not found: " << commandName << "\n";
            out << "Type 'help' to see all available commands.";
            return out.str();
        }
    };
}

void registerBuiltinCapabilities(CapabilityRegistry& registry) {
    registry.registerCapability(makeEchoCapability());
    registry.registerCapability(makeAboutCapability());
    registry.registerCapability(makeStatusCapability());
    registry.registerCapability(makeHelpCapability());
}
