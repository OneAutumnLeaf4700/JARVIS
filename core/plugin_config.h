#pragma once

#include <string>
#include <unordered_map>

// Loads two flat, line-based "key=value" config files — no YAML/JSON dependency, the data is
// genuinely this simple (INV-10). Lines look like "<capability_name>.enabled=true" or
// "<capability_name>.granted=true". A missing file is not an error: every lookup just falls
// back to its documented default.
class PluginConfig {
 public:
    static PluginConfig load(const std::string& capabilitiesPath, const std::string& grantsPath);

    // Default true if capabilityName is unlisted — fail-open for availability, matching how
    // the system behaves today with zero config.
    bool isEnabled(const std::string& capabilityName) const;

    // Default false if capabilityName is unlisted — fail-closed for consent (INV-9's intent).
    bool isGranted(const std::string& capabilityName) const;

    // Records consent in memory and appends it to the grants file passed to load().
    void grant(const std::string& capabilityName);

 private:
    std::unordered_map<std::string, bool> enabled_;
    std::unordered_map<std::string, bool> granted_;
    std::string grantsPath_;
};
