#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "capability_registry.h"
#include "engine.h"
#include "plugin_loader.h"

TEST(LoadPluginDirsTest, MissingFileReturnsEmptyList) {
    std::vector<std::string> dirs = loadPluginDirs("does_not_exist_plugin_dirs.cfg");
    EXPECT_TRUE(dirs.empty());
}

TEST(LoadPluginDirsTest, ParsesOneDirectoryPerLineSkippingCommentsAndBlanks) {
    {
        std::ofstream file("test_plugin_dirs.cfg");
        file << "# a comment\n";
        file << "\n";
        file << "build/plugins/\n";
        file << "another/dir\n";
    }
    std::vector<std::string> dirs = loadPluginDirs("test_plugin_dirs.cfg");
    std::remove("test_plugin_dirs.cfg");

    ASSERT_EQ(dirs.size(), 2u);
    EXPECT_EQ(dirs[0], "build/plugins/");
    EXPECT_EQ(dirs[1], "another/dir");
}

TEST(PluginLoaderTest, LoadFromMissingDirectoryReturnsEmptyResultList) {
    CapabilityRegistry registry;
    PluginLoader loader;

    std::vector<PluginLoadResult> results = loader.loadFromDirectory("does_not_exist_dir", registry);

    EXPECT_TRUE(results.empty());
    EXPECT_TRUE(loader.loadedPluginIds().empty());
}

TEST(PluginLoaderTest, SkipsSubdirectoryWithNoManifest) {
    std::filesystem::create_directories("test_plugins_no_manifest/not-a-plugin");
    CapabilityRegistry registry;
    PluginLoader loader;

    std::vector<PluginLoadResult> results = loader.loadFromDirectory("test_plugins_no_manifest", registry);

    std::filesystem::remove_all("test_plugins_no_manifest");
    EXPECT_TRUE(results.empty());
}

TEST(PluginLoaderTest, RejectsMalformedManifestJson) {
    std::filesystem::create_directories("test_plugins_bad_json/broken");
    {
        std::ofstream file("test_plugins_bad_json/broken/manifest.json");
        file << "{ this is not valid json";
    }
    CapabilityRegistry registry;
    PluginLoader loader;

    std::vector<PluginLoadResult> results = loader.loadFromDirectory("test_plugins_bad_json", registry);

    std::filesystem::remove_all("test_plugins_bad_json");
    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results[0].loaded);
    EXPECT_TRUE(registry.allByIntent().empty());
}

TEST(PluginLoaderTest, RejectsManifestMissingRequiredField) {
    std::filesystem::create_directories("test_plugins_missing_field/incomplete");
    {
        std::ofstream file("test_plugins_missing_field/incomplete/manifest.json");
        file << R"({"id": "incomplete", "version": "1.0.0"})";
    }
    CapabilityRegistry registry;
    PluginLoader loader;

    std::vector<PluginLoadResult> results = loader.loadFromDirectory("test_plugins_missing_field", registry);

    std::filesystem::remove_all("test_plugins_missing_field");
    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results[0].loaded);
}

TEST(PluginLoaderTest, RejectsAbiVersionMismatchBeforeDlopen) {
    std::filesystem::create_directories("test_plugins_bad_abi/mismatched");
    {
        std::ofstream file("test_plugins_bad_abi/mismatched/manifest.json");
        file << R"({
            "id": "mismatched",
            "version": "1.0.0",
            "abi_version": 999,
            "library": "does_not_exist.so",
            "capabilities": []
        })";
    }
    CapabilityRegistry registry;
    PluginLoader loader;

    std::vector<PluginLoadResult> results = loader.loadFromDirectory("test_plugins_bad_abi", registry);

    std::filesystem::remove_all("test_plugins_bad_abi");
    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results[0].loaded);
    // Proves rejection happened before dlopen was ever attempted: the library path in the
    // manifest doesn't exist, so a dlopen attempt would also fail, but for a DIFFERENT
    // reason — the loader must never get that far for an ABI mismatch.
    EXPECT_NE(results[0].reason.find("ABI"), std::string::npos);
}

TEST(PluginLoaderTest, RejectsDuplicateIntentAgainstExistingBuiltin) {
    std::filesystem::create_directories("test_plugins_dup_intent/dup");
    {
        std::ofstream file("test_plugins_dup_intent/dup/manifest.json");
        file << R"({
            "id": "dup",
            "version": "1.0.0",
            "abi_version": 1,
            "library": "does_not_exist.so",
            "capabilities": [
                {"intent": "echo", "description": "d", "power_tier": "T0_READ_ONLY"}
            ]
        })";
    }
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);  // registers "echo" already
    PluginLoader loader;

    std::vector<PluginLoadResult> results = loader.loadFromDirectory("test_plugins_dup_intent", registry);

    std::filesystem::remove_all("test_plugins_dup_intent");
    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results[0].loaded);
    EXPECT_NE(results[0].reason.find("already registered"), std::string::npos);
}

TEST(PluginLoaderTest, DisableAndUnloadOnUnknownPluginIdReturnFalse) {
    CapabilityRegistry registry;
    PluginLoader loader;

    EXPECT_FALSE(loader.disablePlugin("nonexistent", registry));
    EXPECT_FALSE(loader.unloadPlugin("nonexistent"));
}
