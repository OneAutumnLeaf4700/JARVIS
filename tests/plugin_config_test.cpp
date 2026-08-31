#include <gtest/gtest.h>

#include <fstream>

#include "plugin_config.h"

namespace {

std::string writeTempFile(const std::string& path, const std::string& contents) {
    std::ofstream file(path);
    file << contents;
    file.close();
    return path;
}

}  // namespace

TEST(PluginConfigTest, MissingCapabilitiesFileDefaultsEverythingEnabled) {
    PluginConfig config = PluginConfig::load(
        "does_not_exist_capabilities.cfg", "does_not_exist_grants.cfg");

    EXPECT_TRUE(config.isEnabled("echo"));
    EXPECT_TRUE(config.isEnabled("anything_unlisted"));
}

TEST(PluginConfigTest, MissingGrantsFileDefaultsEverythingNotGranted) {
    PluginConfig config = PluginConfig::load(
        "does_not_exist_capabilities.cfg", "does_not_exist_grants.cfg");

    EXPECT_FALSE(config.isGranted("volume_control"));
}

TEST(PluginConfigTest, ParsesExplicitEnabledFalse) {
    writeTempFile("test_capabilities_disabled.cfg", "echo.enabled=false\n");

    PluginConfig config = PluginConfig::load(
        "test_capabilities_disabled.cfg", "does_not_exist_grants.cfg");

    EXPECT_FALSE(config.isEnabled("echo"));
    EXPECT_TRUE(config.isEnabled("status"));  // unlisted stays enabled
}

TEST(PluginConfigTest, ParsesExplicitGrantedTrue) {
    writeTempFile("test_grants_present.cfg", "volume_control.granted=true\n");

    PluginConfig config = PluginConfig::load(
        "does_not_exist_capabilities.cfg", "test_grants_present.cfg");

    EXPECT_TRUE(config.isGranted("volume_control"));
    EXPECT_FALSE(config.isGranted("other_capability"));
}

TEST(PluginConfigTest, MalformedLinesAreSkippedNotFatal) {
    writeTempFile("test_capabilities_malformed.cfg",
        "this line has no equals sign\n"
        "echo.enabled=false\n"
        "=noname\n"
        "novalue.=true\n");

    PluginConfig config = PluginConfig::load(
        "test_capabilities_malformed.cfg", "does_not_exist_grants.cfg");

    // The one well-formed line still took effect; the malformed ones were skipped, not fatal.
    EXPECT_FALSE(config.isEnabled("echo"));
}

TEST(PluginConfigTest, GrantPersistsToDiskAndIsReReadOnFreshLoad) {
    // Start from a clean grants file for this test.
    writeTempFile("test_grants_roundtrip.cfg", "");

    PluginConfig config = PluginConfig::load(
        "does_not_exist_capabilities.cfg", "test_grants_roundtrip.cfg");
    EXPECT_FALSE(config.isGranted("volume_control"));

    EXPECT_TRUE(config.grant("volume_control"));  // persists successfully
    EXPECT_TRUE(config.isGranted("volume_control"));  // in-memory takes effect immediately

    PluginConfig reloaded = PluginConfig::load(
        "does_not_exist_capabilities.cfg", "test_grants_roundtrip.cfg");
    EXPECT_TRUE(reloaded.isGranted("volume_control"));  // and survives a fresh load()
}
