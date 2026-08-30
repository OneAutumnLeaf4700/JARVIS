#include <gtest/gtest.h>

#include "capability_registry.h"
#include "command_handler.h"
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

TEST(StatusCapabilityTest, ReflectsLiveEngineState) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    Engine engine;
    ExecutionContext context{engine, registry};
    std::optional<std::string> result = registry.dispatch(CommandType::STATUS, "", context);

    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->find("Engine: running"), std::string::npos);
    EXPECT_NE(result->find("Last command: none"), std::string::npos);
}

TEST(StatusCapabilityTest, IsPowerTierT0) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    const Capability* status = registry.resolve(CommandType::STATUS);
    ASSERT_NE(status, nullptr);
    EXPECT_EQ(status->powerTier, PowerTier::T0_READ_ONLY);
}

TEST(BuiltinCapabilitiesTest, RegistersExactlyFourExpectedCapabilities) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    EXPECT_EQ(registry.all().size(), 4u);
    EXPECT_NE(registry.resolve(CommandType::ECHO), nullptr);
    EXPECT_NE(registry.resolve(CommandType::STATUS), nullptr);
    EXPECT_NE(registry.resolve(CommandType::ABOUT), nullptr);
    EXPECT_NE(registry.resolve(CommandType::HELP), nullptr);
    EXPECT_EQ(registry.resolve(CommandType::UNKNOWN), nullptr);
}

TEST(HelpCapabilityTest, IsPowerTierT0) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    const Capability* help = registry.resolve(CommandType::HELP);
    ASSERT_NE(help, nullptr);
    EXPECT_EQ(help->powerTier, PowerTier::T0_READ_ONLY);
}

TEST(HelpCapabilityTest, ListsAllFourBuiltinsWithoutHardcodingThem) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    Engine engine;
    ExecutionContext context{engine, registry};
    std::optional<std::string> result = registry.dispatch(CommandType::HELP, "", context);

    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->find("echo"), std::string::npos);
    EXPECT_NE(result->find("status"), std::string::npos);
    EXPECT_NE(result->find("about"), std::string::npos);
    EXPECT_NE(result->find("help"), std::string::npos);
}

TEST(HelpCapabilityTest, DoesNotListUnknown) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    Engine engine;
    ExecutionContext context{engine, registry};
    std::optional<std::string> result = registry.dispatch(CommandType::HELP, "", context);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->find("- unknown:"), std::string::npos);
}

TEST(HelpCapabilityTest, ListsExitEvenThoughItIsNotACapability) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    Engine engine;
    ExecutionContext context{engine, registry};
    std::optional<std::string> result = registry.dispatch(CommandType::HELP, "", context);

    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->find("exit"), std::string::npos);
}

TEST(HelpCapabilityTest, SpecificCommandLookupFindsRegisteredCapability) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    Engine engine;
    ExecutionContext context{engine, registry};
    std::optional<std::string> result = registry.dispatch(CommandType::HELP, "echo", context);

    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->find("echo"), std::string::npos);
    EXPECT_NE(result->find("Echoes"), std::string::npos);
}

TEST(HelpCapabilityTest, SpecificCommandLookupReportsNotFoundForUnregistered) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    Engine engine;
    ExecutionContext context{engine, registry};
    std::optional<std::string> result = registry.dispatch(CommandType::HELP, "bananas", context);

    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->find("Command not found"), std::string::npos);
}

TEST(CapabilityRegistryTest, RegisteredCapabilityNamesMatchParser) {
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    for (const auto& [cmdType, capability] : registry.all()) {
        ParsedCommand parsed = parseCommand(capability.name);
        EXPECT_EQ(parsed.type, capability.intent)
            << "Capability '" << capability.name << "' does not parse to its declared intent";
    }
}
