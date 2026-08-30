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

// Fixture for tests that write grant files. Each test gets a unique temp file that is cleaned
// up after the test to maintain hermetic isolation.
class ConsentGateGrantTest : public ::testing::Test {
 protected:
    std::string grants_file_;

    void SetUp() override {
        // Create a unique temporary grants file for this test instance.
        grants_file_ = "temp_grants_" + std::string(__FUNCTION__) + ".cfg";
    }

    void TearDown() override {
        // Clean up the grants file after the test.
        std::remove(grants_file_.c_str());
    }
};

TEST_F(ConsentGateGrantTest, T2AllowedWithGrant) {
    PluginConfig config = PluginConfig::load("does_not_exist.cfg", grants_file_);
    config.grant("volume_control");
    ConsentGate gate(config);

    ConsentResult result = gate.check(makeCapability("volume_control", PowerTier::T2_SYSTEM_AFFECTING));

    EXPECT_TRUE(result.allowed);
    EXPECT_TRUE(result.reason.empty());
}

class ConsentGateT3Test : public ::testing::Test {
 protected:
    std::string grants_file_;

    void SetUp() override {
        grants_file_ = "temp_grants_" + std::string(__FUNCTION__) + ".cfg";
    }

    void TearDown() override {
        std::remove(grants_file_.c_str());
    }
};

TEST_F(ConsentGateT3Test, T3AlwaysDeniedEvenIfSomehowGranted) {
    PluginConfig config = PluginConfig::load("does_not_exist.cfg", grants_file_);
    config.grant("delete_files");  // granting is meaningless for T3 — gate must ignore it
    ConsentGate gate(config);

    ConsentResult result = gate.check(makeCapability("delete_files", PowerTier::T3_DESTRUCTIVE));

    EXPECT_FALSE(result.allowed);
    EXPECT_NE(result.reason.find("not yet implemented"), std::string::npos);
}

class ConsentGateT4Test : public ::testing::Test {
 protected:
    std::string grants_file_;

    void SetUp() override {
        grants_file_ = "temp_grants_" + std::string(__FUNCTION__) + ".cfg";
    }

    void TearDown() override {
        std::remove(grants_file_.c_str());
    }
};

TEST_F(ConsentGateT4Test, T4AlwaysDeniedEvenIfSomehowGranted) {
    PluginConfig config = PluginConfig::load("does_not_exist.cfg", grants_file_);
    config.grant("call_external_api");
    ConsentGate gate(config);

    ConsentResult result = gate.check(makeCapability("call_external_api", PowerTier::T4_EXTERNAL));

    EXPECT_FALSE(result.allowed);
    EXPECT_NE(result.reason.find("not yet implemented"), std::string::npos);
}
