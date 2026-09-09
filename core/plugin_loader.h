#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class CapabilityRegistry;

struct PluginLoadResult {
    std::string pluginId;
    bool loaded;
    std::string reason;  // empty if loaded; human-readable rejection reason otherwise
};

// Discovers, validates, and loads (via dynlib::open — dlopen on POSIX, LoadLibrary on Windows)
// plugins from configured trusted local directories. See
// docs/superpowers/specs/2026-08-31-plugin-sdk-loader-design.md for the full manifest
// validation order and safe-unload design.
class PluginLoader {
 public:
    ~PluginLoader();

    // Scans pluginDir for one subdirectory per plugin, validates and loads each, registering
    // its capabilities into registry. Returns one result per subdirectory containing a
    // manifest.json (loaded or rejected). A missing pluginDir yields an empty list, not an
    // error.
    std::vector<PluginLoadResult> loadFromDirectory(const std::string& pluginDir,
                                                      CapabilityRegistry& registry);

    // Removes this plugin's capabilities from string-intent dispatch (registry.dispatch(intent, ...)
    // will no longer resolve them) but keeps its library mapped. Returns false if pluginId is
    // unknown or already disabled.
    bool disablePlugin(const std::string& pluginId, CapabilityRegistry& registry);

    // Unloads a plugin's library (via dynlib::close — dlclose on POSIX, FreeLibrary on
    // Windows). Returns false if the plugin is unknown, not yet disabled, or has an
    // invocation currently in flight.
    bool unloadPlugin(const std::string& pluginId);

    const std::vector<std::string>& loadedPluginIds() const;

 private:
    struct LoadedPlugin {
        void* handle = nullptr;
        std::vector<std::string> intents;
        std::shared_ptr<std::atomic<int>> invocationCount;
        bool disabled = false;
    };

    std::unordered_map<std::string, LoadedPlugin> plugins_;
    std::vector<std::string> loadedPluginIds_;
};

// Reads a flat, one-directory-per-line file (see config/plugin_dirs.cfg) — "#" comments and
// blank lines skipped. A missing file yields an empty list, not an error, matching
// PluginConfig's own missing-file discipline.
std::vector<std::string> loadPluginDirs(const std::string& path);
