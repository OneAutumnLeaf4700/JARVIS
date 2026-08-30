#include <gtest/gtest.h>

#include "capability_registry.h"
#include "engine.h"

TEST(CapabilityRegistryTest, ResolveReturnsNullForUnregisteredIntent) {
    CapabilityRegistry registry;
    EXPECT_EQ(registry.resolve(CommandType::ECHO), nullptr);
}

TEST(CapabilityRegistryTest, ResolveReturnsRegisteredCapability) {
    CapabilityRegistry registry;
    registry.registerCapability(Capability{
        "echo", CommandType::ECHO, "test echo", PowerTier::T0_READ_ONLY,
        [](const std::string& payload, ExecutionContext&) { return payload; }
    });

    const Capability* found = registry.resolve(CommandType::ECHO);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->name, "echo");
}

TEST(CapabilityRegistryTest, DispatchInvokesRegisteredCapability) {
    CapabilityRegistry registry;
    registry.registerCapability(Capability{
        "echo", CommandType::ECHO, "test echo", PowerTier::T0_READ_ONLY,
        [](const std::string& payload, ExecutionContext&) { return "echoed: " + payload; }
    });

    Engine engine;
    ExecutionContext context{engine, registry};
    std::optional<std::string> result = registry.dispatch(CommandType::ECHO, "hello", context);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "echoed: hello");
}

TEST(CapabilityRegistryTest, DispatchReturnsNulloptForUnregisteredIntent) {
    CapabilityRegistry registry;
    Engine engine;
    ExecutionContext context{engine, registry};

    std::optional<std::string> result = registry.dispatch(CommandType::UNKNOWN, "anything", context);
    EXPECT_FALSE(result.has_value());
}

TEST(BuiltinCapabilitiesTest, EchoReturnsPayloadUnchanged) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    Engine engine;
    ExecutionContext context{engine, registry};
    std::optional<std::string> result = registry.dispatch(CommandType::ECHO, "hello there", context);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "hello there");
}

TEST(BuiltinCapabilitiesTest, AboutReturnsFixedDescription) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    Engine engine;
    ExecutionContext context{engine, registry};
    std::optional<std::string> result = registry.dispatch(CommandType::ABOUT, "", context);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "JARVIS Core Engine v1.0\nDeveloped by Rayyan.");
}

TEST(BuiltinCapabilitiesTest, EchoAndAboutAreBothPowerTierT0) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    const Capability* echo = registry.resolve(CommandType::ECHO);
    const Capability* about = registry.resolve(CommandType::ABOUT);
    ASSERT_NE(echo, nullptr);
    ASSERT_NE(about, nullptr);
    EXPECT_EQ(echo->powerTier, PowerTier::T0_READ_ONLY);
    EXPECT_EQ(about->powerTier, PowerTier::T0_READ_ONLY);
}
