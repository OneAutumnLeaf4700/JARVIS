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
