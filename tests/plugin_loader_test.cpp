#include <gtest/gtest.h>

#include <cstdlib>
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

TEST(PluginLoaderTest, RejectsDuplicateIntentWithinSameManifest) {
    std::filesystem::create_directories("test_plugins_dup_intent_self/dup_self");
    {
        std::ofstream file("test_plugins_dup_intent_self/dup_self/manifest.json");
        file << R"({
            "id": "dup_self",
            "version": "1.0.0",
            "abi_version": 1,
            "library": "does_not_exist.so",
            "capabilities": [
                {"intent": "foo", "description": "d", "power_tier": "T0_READ_ONLY"},
                {"intent": "foo", "description": "d again", "power_tier": "T0_READ_ONLY"}
            ]
        })";
    }
    CapabilityRegistry registry;
    PluginLoader loader;

    std::vector<PluginLoadResult> results = loader.loadFromDirectory("test_plugins_dup_intent_self", registry);

    std::filesystem::remove_all("test_plugins_dup_intent_self");
    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results[0].loaded);
    // Proves rejection happened at manifest-validation time (before dlopen was ever
    // attempted): "library" points at a nonexistent .so, so a dlopen attempt would also
    // fail, but for a DIFFERENT reason ("dlopen failed: ...") — the reason here must be
    // about the duplicate intent instead.
    EXPECT_NE(results[0].reason.find("duplicate intent"), std::string::npos);
    EXPECT_EQ(results[0].reason.find("dlopen"), std::string::npos);
}

namespace {

// Compiles a tiny self-contained fixture plugin .so whose jarvis_plugin_register()
// double-registers ONE capability instead of registering the two distinct capabilities its
// manifest declares. This is a real compiled .so (not a mock) so the test exercises the actual
// dlopen/dlsym path, proving the staged-vs-declared cross-check is a true bijection rather than
// a same-size "any match" check. The ABI struct/enum layout is duplicated by hand here (matching
// plugin_sdk/jarvis_plugin_abi.h's JARVIS_PLUGIN_ABI_VERSION 1 layout) so this test has no
// include-path dependency on plugin_sdk/.
bool CompileDoubleRegisterFixturePlugin(const std::string& outputSoPath) {
    const std::string sourcePath = outputSoPath + ".c";
    {
        std::ofstream src(sourcePath);
        src << R"C(
typedef enum {
    JARVIS_POWER_TIER_T0_READ_ONLY = 0,
    JARVIS_POWER_TIER_T1_STATEFUL_LOCAL = 1,
    JARVIS_POWER_TIER_T2_SYSTEM_AFFECTING = 2,
    JARVIS_POWER_TIER_T3_DESTRUCTIVE = 3,
    JARVIS_POWER_TIER_T4_EXTERNAL = 4
} JarvisPowerTier;

typedef char* (*JarvisCapabilityFn)(const char* payload);

typedef struct {
    int (*registerCapability)(void* host_context, const char* intent_name,
        const char* description, JarvisPowerTier power_tier, JarvisCapabilityFn execute);
} JarvisPluginHost;

static char* handle_call(const char* payload) { (void)payload; return 0; }

int jarvis_plugin_abi_version(void) { return 1; }

int jarvis_plugin_register(void* host_context, const JarvisPluginHost* host) {
    // Manifest declares "cap-a" and "cap-b" but this plugin only ever registers "cap-a",
    // twice — Bug 1's reproduction case.
    int ok1 = host->registerCapability(host_context, "cap-a", "first", JARVIS_POWER_TIER_T0_READ_ONLY, handle_call);
    int ok2 = host->registerCapability(host_context, "cap-a", "second", JARVIS_POWER_TIER_T0_READ_ONLY, handle_call);
    return ok1 && ok2;
}
)C";
    }

    const std::string command = "cc -shared -fPIC -o " + outputSoPath + " " + sourcePath + " 2>/dev/null";
    const int status = std::system(command.c_str());
    std::remove(sourcePath.c_str());
    return status == 0 && std::filesystem::exists(outputSoPath);
}

}  // namespace

TEST(PluginLoaderTest, RejectsPluginThatDoubleRegistersOneDeclaredCapabilityInsteadOfBoth) {
    std::filesystem::create_directories("test_plugins_double_register/dblreg");
    const std::string soPath = "test_plugins_double_register/dblreg/plugin.so";
    ASSERT_TRUE(CompileDoubleRegisterFixturePlugin(soPath))
        << "could not compile fixture plugin .so with 'cc' — is a C compiler installed?";
    {
        std::ofstream file("test_plugins_double_register/dblreg/manifest.json");
        file << R"({
            "id": "dblreg",
            "version": "1.0.0",
            "abi_version": 1,
            "library": "plugin.so",
            "capabilities": [
                {"intent": "cap-a", "description": "a", "power_tier": "T0_READ_ONLY"},
                {"intent": "cap-b", "description": "b", "power_tier": "T0_READ_ONLY"}
            ]
        })";
    }
    CapabilityRegistry registry;
    PluginLoader loader;

    std::vector<PluginLoadResult> results = loader.loadFromDirectory("test_plugins_double_register", registry);

    std::filesystem::remove_all("test_plugins_double_register");
    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results[0].loaded) << "reason: " << results[0].reason;
    EXPECT_NE(results[0].reason.find("do not match manifest"), std::string::npos);
    EXPECT_TRUE(loader.loadedPluginIds().empty());
    EXPECT_EQ(registry.resolve("cap-a"), nullptr);
    EXPECT_EQ(registry.resolve("cap-b"), nullptr);
}

TEST(PluginLoaderTest, DisableAndUnloadOnUnknownPluginIdReturnFalse) {
    CapabilityRegistry registry;
    PluginLoader loader;

    EXPECT_FALSE(loader.disablePlugin("nonexistent", registry));
    EXPECT_FALSE(loader.unloadPlugin("nonexistent"));
}
