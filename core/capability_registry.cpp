#include "capability_registry.h"

#include <iomanip>
#include <sstream>
#include <thread>

#include "command_handler.h"
#include "consent_gate.h"
#include "engine.h"

static const std::string kExitDescription =
    "Terminates the JARVIS Core Engine. Usage: exit";

namespace {

std::string operatingSystemName() {
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#else
    return "Unknown";
#endif
}

std::string architectureName() {
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#else
    return "Unknown";
#endif
}

std::string compilerName() {
#if defined(__clang__)
    return "Clang " + std::to_string(__clang_major__) + "." + std::to_string(__clang_minor__);
#elif defined(__GNUC__)
    return "GCC " + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__);
#elif defined(_MSC_VER)
    return "MSVC " + std::to_string(_MSC_VER);
#else
    return "Unknown";
#endif
}

}  // namespace

void CapabilityRegistry::registerCapability(Capability capability) {
    if (capability.intentName.empty()) {
        capability.intentName = capability.name;
    }

    namedCapabilities_[capability.intentName] = capability;
    if (capability.intent != CommandType::UNKNOWN) {
        capabilities_[capability.intent] = std::move(capability);
    }
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

const Capability* CapabilityRegistry::resolve(const std::string& intentName) const {
    auto it = namedCapabilities_.find(intentName);
    if (it == namedCapabilities_.end()) {
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

std::optional<std::string> CapabilityRegistry::dispatch(
    const std::string& intentName, const std::string& payload, ExecutionContext& context) const {
    const Capability* capability = resolve(intentName);
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

const std::unordered_map<std::string, Capability>& CapabilityRegistry::allByIntent() const {
    return namedCapabilities_;
}

const PluginConfig* CapabilityRegistry::pluginConfig() const {
    return pluginConfig_;
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

Capability makeSystemInfoCapability() {
    return Capability{
        "system-info",
        CommandType::SYSTEM_INFO,
        "Shows local OS, architecture, compiler, and hardware-thread information. Usage: system-info",
        PowerTier::T0_READ_ONLY,
        [](const std::string& /*payload*/, ExecutionContext& /*context*/) -> std::string {
            std::ostringstream out;
            out << "System information:\n";
            out << "OS: " << operatingSystemName() << "\n";
            out << "Architecture: " << architectureName() << "\n";
            out << "Compiler: " << compilerName() << "\n";
            out << "C++ standard: " << __cplusplus << "\n";
            out << "Hardware threads: ";
            const unsigned int threadCount = std::thread::hardware_concurrency();
            if (threadCount == 0) {
                out << "unavailable";
            } else {
                out << threadCount;
            }
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

            const PluginConfig* pluginConfig = context.registry.pluginConfig();
            auto isDisabled = [pluginConfig](const Capability& capability) {
                return pluginConfig != nullptr && !pluginConfig->isEnabled(capability.name);
            };

            if (payload.empty()) {
                out << "Available commands:\n";
                for (const auto& [intent, capability] : context.registry.all()) {
                    if (isDisabled(capability)) {
                        continue;
                    }
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
                if (capability.name == commandName && !isDisabled(capability)) {
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
    registry.registerCapability(makeSystemInfoCapability());
}
