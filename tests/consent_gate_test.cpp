#include <cstdio>
#include <gtest/gtest.h>

#include "capability.h"
#include "consent_gate.h"
#include "plugin_config.h"

namespace {

Capability makeCapability(const std::string& name, PowerTier tier) {
    return Capability{
        name, CommandType::ECHO, "test capability", tier,
        [](const std::string& payload, ExecutionContext&) { return payload; }
    };
}

}  // namespace

TEST(ConsentGateTest, T0AlwaysAllowedRegardlessOfConfig) {
    PluginConfig config = PluginConfig::load("does_not_exist.cfg", "does_not_exist.cfg");
    ConsentGate gate(config);

    ConsentResult result = gate.check(makeCapability("echo", PowerTier::T0_READ_ONLY));

    EXPECT_TRUE(result.allowed);
    EXPECT_TRUE(result.reason.empty());
}

TEST(ConsentGateTest, T1AlwaysAllowedRegardlessOfConfig) {
    PluginConfig config = PluginConfig::load("does_not_exist.cfg", "does_not_exist.cfg");
    ConsentGate gate(config);

    ConsentResult result = gate.check(makeCapability("some_stateful_thing", PowerTier::T1_STATEFUL_LOCAL));

    EXPECT_TRUE(result.allowed);
}

TEST(ConsentGateTest, T2DeniedWithoutGrant) {
    PluginConfig config = PluginConfig::load("does_not_exist.cfg", "does_not_exist.cfg");
    ConsentGate gate(config);

    ConsentResult result = gate.check(makeCapability("volume_control", PowerTier::T2_SYSTEM_AFFECTING));

    EXPECT_FALSE(result.allowed);
    EXPECT_NE(result.reason.find("volume_control"), std::string::npos);
    EXPECT_NE(result.reason.find("--grant"), std::string::npos);
}

// Shared fixture for tests that write grant files. Each test instance gets a filename unique
// to the actual running test (via GTest's current_test_info(), not __FUNCTION__ — inside
// SetUp() that macro always expands to the literal "SetUp", not the test's name), cleaned up
// unconditionally in TearDown() so a failed assertion can't leak the file into later runs.
class ConsentGateFileTest : public ::testing::Test {
 protected:
    std::string grants_file_;

    void SetUp() override {
        const auto* test_info = ::testing::UnitTest::GetInstance()->current_test_info();
        grants_file_ = "temp_grants_" + std::string(test_info->test_suite_name()) + "_" +
                        std::string(test_info->name()) + ".cfg";
    }

    void TearDown() override {
        std::remove(grants_file_.c_str());
    }
};

TEST_F(ConsentGateFileTest, T2AllowedWithGrant) {
    PluginConfig config = PluginConfig::load("does_not_exist.cfg", grants_file_);
    config.grant("volume_control");
    ConsentGate gate(config);

    ConsentResult result = gate.check(makeCapability("volume_control", PowerTier::T2_SYSTEM_AFFECTING));

    EXPECT_TRUE(result.allowed);
    EXPECT_TRUE(result.reason.empty());
}

TEST_F(ConsentGateFileTest, T3AlwaysDeniedEvenIfSomehowGranted) {
    PluginConfig config = PluginConfig::load("does_not_exist.cfg", grants_file_);
    config.grant("delete_files");  // granting is meaningless for T3 — gate must ignore it
    ConsentGate gate(config);

    ConsentResult result = gate.check(makeCapability("delete_files", PowerTier::T3_DESTRUCTIVE));

    EXPECT_FALSE(result.allowed);
    EXPECT_NE(result.reason.find("not yet implemented"), std::string::npos);
}

TEST_F(ConsentGateFileTest, T4AlwaysDeniedEvenIfSomehowGranted) {
    PluginConfig config = PluginConfig::load("does_not_exist.cfg", grants_file_);
    config.grant("call_external_api");
    ConsentGate gate(config);

    ConsentResult result = gate.check(makeCapability("call_external_api", PowerTier::T4_EXTERNAL));

    EXPECT_FALSE(result.allowed);
    EXPECT_NE(result.reason.find("not yet implemented"), std::string::npos);
}
