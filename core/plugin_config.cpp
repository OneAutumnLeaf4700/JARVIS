#include "plugin_config.h"

#include <fstream>
#include <functional>

#include <spdlog/spdlog.h>

namespace {

// Parses "<name>.<field>=<value>" lines from path, calling onEntry(name, field, value) for each
// well-formed one. Malformed lines are skipped with a warning, never fatal — a typo in a
// hand-edited config file shouldn't crash startup. A missing file yields no entries at all,
// letting the caller's own defaults apply untouched.
void parseKeyValueFile(
    const std::string& path,
    const std::function<void(const std::string&, const std::string&, const std::string&)>& onEntry) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }

        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            spdlog::warn("PluginConfig: skipping malformed line in {}: '{}'", path, line);
            continue;
        }

        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);

        const auto dot = key.rfind('.');
        if (dot == std::string::npos || dot == 0 || dot == key.size() - 1) {
            spdlog::warn("PluginConfig: skipping malformed line in {}: '{}'", path, line);
            continue;
        }

        onEntry(key.substr(0, dot), key.substr(dot + 1), value);
    }
}

bool parseBool(const std::string& value) {
    return value == "true";
}

}  // namespace

PluginConfig PluginConfig::load(const std::string& capabilitiesPath, const std::string& grantsPath) {
    PluginConfig config;
    config.grantsPath_ = grantsPath;

    parseKeyValueFile(capabilitiesPath,
        [&config, &capabilitiesPath](const std::string& name, const std::string& field, const std::string& value) {
            if (field == "enabled") {
                config.enabled_[name] = parseBool(value);
            } else {
                spdlog::warn("PluginConfig: unknown field '{}' for '{}' in {}", field, name, capabilitiesPath);
            }
        });

    parseKeyValueFile(grantsPath,
        [&config, &grantsPath](const std::string& name, const std::string& field, const std::string& value) {
            if (field == "granted") {
                config.granted_[name] = parseBool(value);
            } else {
                spdlog::warn("PluginConfig: unknown field '{}' for '{}' in {}", field, name, grantsPath);
            }
        });

    return config;
}

bool PluginConfig::isEnabled(const std::string& capabilityName) const {
    const auto it = enabled_.find(capabilityName);
    if (it == enabled_.end()) {
        return true;
    }
    return it->second;
}

bool PluginConfig::isGranted(const std::string& capabilityName) const {
    const auto it = granted_.find(capabilityName);
    if (it == granted_.end()) {
        return false;
    }
    return it->second;
}

void PluginConfig::grant(const std::string& capabilityName) {
    granted_[capabilityName] = true;

    std::ofstream file(grantsPath_, std::ios::app);
    file << capabilityName << ".granted=true\n";
}
