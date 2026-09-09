#include "plugin_loader.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <spdlog/spdlog.h>

#include "capability.h"
#include "capability_registry.h"
#include "dynamic_library.h"
#include "minimal_json.h"
#include "jarvis_plugin_abi.h"

namespace {

PowerTier toPowerTier(JarvisPowerTier tier) {
    switch (tier) {
        case JARVIS_POWER_TIER_T0_READ_ONLY: return PowerTier::T0_READ_ONLY;
        case JARVIS_POWER_TIER_T1_STATEFUL_LOCAL: return PowerTier::T1_STATEFUL_LOCAL;
        case JARVIS_POWER_TIER_T2_SYSTEM_AFFECTING: return PowerTier::T2_SYSTEM_AFFECTING;
        case JARVIS_POWER_TIER_T3_DESTRUCTIVE: return PowerTier::T3_DESTRUCTIVE;
        case JARVIS_POWER_TIER_T4_EXTERNAL: return PowerTier::T4_EXTERNAL;
    }
    return PowerTier::T4_EXTERNAL;  // unreachable; most-restrictive fallback if it ever is
}

std::optional<PowerTier> parsePowerTierName(const std::string& name) {
    if (name == "T0_READ_ONLY") return PowerTier::T0_READ_ONLY;
    if (name == "T1_STATEFUL_LOCAL") return PowerTier::T1_STATEFUL_LOCAL;
    if (name == "T2_SYSTEM_AFFECTING") return PowerTier::T2_SYSTEM_AFFECTING;
    if (name == "T3_DESTRUCTIVE") return PowerTier::T3_DESTRUCTIVE;
    if (name == "T4_EXTERNAL") return PowerTier::T4_EXTERNAL;
    return std::nullopt;
}

std::string trim(const std::string& text) {
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

struct DeclaredCapability {
    std::string intent;
    PowerTier tier;
};

struct HostContext {
    std::vector<Capability>* staged;
    std::shared_ptr<std::atomic<int>> invocationCounter;
};

int hostRegisterCapability(void* ctx, const char* intentName, const char* description,
                            JarvisPowerTier tier, JarvisCapabilityFn fn) {
    auto* hostContext = static_cast<HostContext*>(ctx);
    auto counter = hostContext->invocationCounter;

    Capability capability;
    capability.name = intentName;
    capability.intent = CommandType::UNKNOWN;
    capability.description = description;
    capability.powerTier = toPowerTier(tier);
    capability.intentName = intentName;
    capability.execute = [fn, counter](const std::string& payload, ExecutionContext&) -> std::string {
        counter->fetch_add(1, std::memory_order_relaxed);
        struct Guard {
            std::atomic<int>* counter;
            ~Guard() { counter->fetch_sub(1, std::memory_order_relaxed); }
        } guard{counter.get()};

        char* raw = fn(payload.c_str());
        std::string result = raw ? raw : "";
        if (raw) {
            std::free(raw);
        }
        return result;
    };

    hostContext->staged->push_back(std::move(capability));
    return 1;
}

}  // namespace

std::vector<std::string> loadPluginDirs(const std::string& path) {
    std::vector<std::string> dirs;
    std::ifstream file(path);
    if (!file.is_open()) {
        return dirs;
    }

    std::string line;
    while (std::getline(file, line)) {
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }
        dirs.push_back(trimmed);
    }
    return dirs;
}

std::vector<PluginLoadResult> PluginLoader::loadFromDirectory(const std::string& pluginDir,
                                                                CapabilityRegistry& registry) {
    std::vector<PluginLoadResult> results;

    std::error_code ec;
    if (!std::filesystem::is_directory(pluginDir, ec)) {
        return results;
    }

    for (const auto& entry : std::filesystem::directory_iterator(pluginDir, ec)) {
        if (!entry.is_directory()) {
            continue;
        }

        const std::string pluginPath = entry.path().string();
        const std::string manifestPath = pluginPath + "/manifest.json";
        const std::string pluginId = entry.path().filename().string();

        if (!std::filesystem::exists(manifestPath)) {
            continue;  // not every subdirectory is necessarily a plugin — silent skip
        }

        std::ifstream manifestFile(manifestPath);
        std::stringstream buffer;
        buffer << manifestFile.rdbuf();

        auto parsed = parseJson(buffer.str());
        if (!parsed || !parsed->isObject()) {
            spdlog::warn("PluginLoader: malformed manifest JSON at {}", manifestPath);
            results.push_back({pluginId, false, "malformed manifest JSON"});
            continue;
        }

        const JsonValue& manifest = *parsed;
        const JsonValue* idField = manifest.find("id");
        const JsonValue* versionField = manifest.find("version");
        const JsonValue* abiField = manifest.find("abi_version");
        const JsonValue* libraryField = manifest.find("library");
        const JsonValue* capsField = manifest.find("capabilities");

        if (!idField || !idField->isString() || !versionField || !versionField->isString() ||
            !abiField || abiField->type != JsonType::Number ||
            !libraryField || !libraryField->isString() ||
            !capsField || !capsField->isArray()) {
            spdlog::warn("PluginLoader: manifest at {} is missing a required field", manifestPath);
            results.push_back({pluginId, false, "missing required manifest field"});
            continue;
        }

        const std::string manifestId = idField->asString();
        const int declaredAbiVersion = static_cast<int>(abiField->numberValue);
        const std::string libraryFile = libraryField->asString();

        if (declaredAbiVersion != JARVIS_PLUGIN_ABI_VERSION) {
            spdlog::warn("PluginLoader: {} declares abi_version {} but host is {}",
                manifestPath, declaredAbiVersion, JARVIS_PLUGIN_ABI_VERSION);
            results.push_back({manifestId, false, "ABI version mismatch"});
            continue;
        }

        std::vector<DeclaredCapability> declared;
        bool manifestValid = true;
        std::string invalidReason;

        for (const JsonValue& capValue : capsField->arrayValue) {
            const JsonValue* intentField = capValue.find("intent");
            const JsonValue* descField = capValue.find("description");
            const JsonValue* tierField = capValue.find("power_tier");
            if (!intentField || !intentField->isString() || !descField || !descField->isString() ||
                !tierField || !tierField->isString()) {
                manifestValid = false;
                invalidReason = "malformed capability entry";
                break;
            }

            std::optional<PowerTier> tier = parsePowerTierName(tierField->asString());
            if (!tier) {
                manifestValid = false;
                invalidReason = "unknown power_tier '" + tierField->asString() + "'";
                break;
            }

            if (registry.resolve(intentField->asString()) != nullptr) {
                manifestValid = false;
                invalidReason = "intent '" + intentField->asString() + "' already registered";
                break;
            }

            const bool duplicateWithinManifest = std::any_of(declared.begin(), declared.end(),
                [&intentField](const DeclaredCapability& d) {
                    return d.intent == intentField->asString();
                });
            if (duplicateWithinManifest) {
                manifestValid = false;
                invalidReason = "duplicate intent '" + intentField->asString() + "' within manifest";
                break;
            }

            declared.push_back({intentField->asString(), *tier});
        }

        if (!manifestValid) {
            spdlog::warn("PluginLoader: {} rejected: {}", manifestPath, invalidReason);
            results.push_back({manifestId, false, invalidReason});
            continue;
        }

        const std::string libraryPath = pluginPath + "/" + dynlib::platformLibraryFilename(libraryFile);
        dynlib::Handle handle = dynlib::open(libraryPath);
        if (!handle) {
            const std::string reason = std::string("failed to load library: ") + dynlib::lastError();
            spdlog::warn("PluginLoader: {}: {}", manifestPath, reason);
            results.push_back({manifestId, false, reason});
            continue;
        }

        using AbiVersionFn = int (*)();
        using RegisterFn = int (*)(void*, const JarvisPluginHost*);

        auto abiVersionFn = reinterpret_cast<AbiVersionFn>(dynlib::symbol(handle, "jarvis_plugin_abi_version"));
        auto registerFn = reinterpret_cast<RegisterFn>(dynlib::symbol(handle, "jarvis_plugin_register"));

        if (!abiVersionFn || !registerFn) {
            spdlog::warn("PluginLoader: {} is missing a required ABI symbol", libraryPath);
            dynlib::close(handle);
            results.push_back({manifestId, false, "missing ABI symbol"});
            continue;
        }

        if (abiVersionFn() != JARVIS_PLUGIN_ABI_VERSION) {
            spdlog::warn("PluginLoader: {} reports a different ABI version at runtime than declared", libraryPath);
            dynlib::close(handle);
            results.push_back({manifestId, false, "runtime ABI version mismatch"});
            continue;
        }

        std::vector<Capability> staged;
        auto invocationCounter = std::make_shared<std::atomic<int>>(0);
        HostContext hostContext{&staged, invocationCounter};

        JarvisPluginHost host;
        host.registerCapability = &hostRegisterCapability;

        if (!registerFn(&hostContext, &host)) {
            spdlog::warn("PluginLoader: {}'s jarvis_plugin_register() returned failure", libraryPath);
            dynlib::close(handle);
            results.push_back({manifestId, false, "plugin registration failed"});
            continue;
        }

        bool crossCheckOk = staged.size() == declared.size();
        if (crossCheckOk) {
            std::vector<DeclaredCapability> unmatched = declared;
            for (const Capability& capability : staged) {
                auto match = std::find_if(unmatched.begin(), unmatched.end(),
                    [&capability](const DeclaredCapability& d) {
                        return d.intent == capability.intentName && d.tier == capability.powerTier;
                    });
                if (match == unmatched.end()) {
                    crossCheckOk = false;
                    break;
                }
                unmatched.erase(match);
            }
        }

        if (!crossCheckOk) {
            spdlog::warn("PluginLoader: {}: registered capabilities do not match its manifest", libraryPath);
            dynlib::close(handle);
            results.push_back({manifestId, false, "registered capabilities do not match manifest"});
            continue;
        }

        std::vector<std::string> intents;
        for (Capability& capability : staged) {
            intents.push_back(capability.intentName);
            registry.registerCapability(std::move(capability));
        }

        LoadedPlugin loaded;
        loaded.handle = handle;
        loaded.intents = std::move(intents);
        loaded.invocationCount = invocationCounter;
        loaded.disabled = false;

        plugins_[manifestId] = std::move(loaded);
        loadedPluginIds_.push_back(manifestId);
        results.push_back({manifestId, true, ""});
        spdlog::info("PluginLoader: loaded plugin '{}' from {}", manifestId, pluginPath);
    }

    return results;
}

bool PluginLoader::disablePlugin(const std::string& pluginId, CapabilityRegistry& registry) {
    auto it = plugins_.find(pluginId);
    if (it == plugins_.end() || it->second.disabled) {
        return false;
    }
    for (const std::string& intent : it->second.intents) {
        registry.unregisterCapability(intent);
    }
    it->second.disabled = true;
    return true;
}

bool PluginLoader::unloadPlugin(const std::string& pluginId) {
    auto it = plugins_.find(pluginId);
    if (it == plugins_.end() || !it->second.disabled) {
        return false;
    }
    if (it->second.invocationCount->load(std::memory_order_relaxed) != 0) {
        return false;
    }

    dynlib::close(it->second.handle);
    plugins_.erase(it);

    auto idIt = std::find(loadedPluginIds_.begin(), loadedPluginIds_.end(), pluginId);
    if (idIt != loadedPluginIds_.end()) {
        loadedPluginIds_.erase(idIt);
    }
    return true;
}

const std::vector<std::string>& PluginLoader::loadedPluginIds() const {
    return loadedPluginIds_;
}

PluginLoader::~PluginLoader() {
    // By the time this destructor runs, the CapabilityRegistry that held these plugins'
    // Capability::execute closures must already be destroyed — main()/grpc_server_main()
    // (Task 6) declare `PluginLoader loader;` BEFORE `CapabilityRegistry registry;`, so C++'s
    // reverse-construction-order destruction runs registry's destructor first, then this
    // one. dlclose()-ing here is then safe: nothing still references these libraries.
    for (auto& [id, plugin] : plugins_) {
        if (plugin.handle) {
            dynlib::close(plugin.handle);
        }
    }
}
