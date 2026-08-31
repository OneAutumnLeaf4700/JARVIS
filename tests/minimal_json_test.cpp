#include <gtest/gtest.h>

#include "minimal_json.h"

TEST(MinimalJsonTest, ParsesEmptyObject) {
    auto result = parseJson("{}");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->isObject());
    EXPECT_EQ(result->objectValue.size(), 0u);
}

TEST(MinimalJsonTest, ParsesStringField) {
    auto result = parseJson(R"({"id": "system-info"})");
    ASSERT_TRUE(result.has_value());
    const JsonValue* id = result->find("id");
    ASSERT_NE(id, nullptr);
    EXPECT_EQ(id->asString(), "system-info");
}

TEST(MinimalJsonTest, ParsesNumberField) {
    auto result = parseJson(R"({"abi_version": 1})");
    ASSERT_TRUE(result.has_value());
    const JsonValue* abi = result->find("abi_version");
    ASSERT_NE(abi, nullptr);
    EXPECT_EQ(abi->type, JsonType::Number);
    EXPECT_DOUBLE_EQ(abi->numberValue, 1.0);
}

TEST(MinimalJsonTest, ParsesNestedArrayOfObjects) {
    auto result = parseJson(R"({
        "capabilities": [
            {"intent": "system-info", "description": "desc", "power_tier": "T0_READ_ONLY"}
        ]
    })");
    ASSERT_TRUE(result.has_value());
    const JsonValue* caps = result->find("capabilities");
    ASSERT_NE(caps, nullptr);
    ASSERT_TRUE(caps->isArray());
    ASSERT_EQ(caps->arrayValue.size(), 1u);
    const JsonValue& first = caps->arrayValue[0];
    ASSERT_TRUE(first.isObject());
    EXPECT_EQ(first.find("intent")->asString(), "system-info");
    EXPECT_EQ(first.find("power_tier")->asString(), "T0_READ_ONLY");
}

TEST(MinimalJsonTest, ParsesBooleansAndNull) {
    auto result = parseJson(R"({"a": true, "b": false, "c": null})");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->find("a")->type, JsonType::Boolean);
    EXPECT_TRUE(result->find("a")->boolValue);
    EXPECT_EQ(result->find("b")->type, JsonType::Boolean);
    EXPECT_FALSE(result->find("b")->boolValue);
    EXPECT_EQ(result->find("c")->type, JsonType::Null);
}

TEST(MinimalJsonTest, ParsesEscapedStringCharacters) {
    auto result = parseJson(R"({"text": "line1\nline2\ttabbed\\backslash\"quoted\""})");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->find("text")->asString(), "line1\nline2\ttabbed\\backslash\"quoted\"");
}

TEST(MinimalJsonTest, ParsesNegativeAndFractionalNumbers) {
    auto result = parseJson(R"({"a": -3.5, "b": 42})");
    ASSERT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ(result->find("a")->numberValue, -3.5);
    EXPECT_DOUBLE_EQ(result->find("b")->numberValue, 42.0);
}

TEST(MinimalJsonTest, RejectsMalformedJson) {
    EXPECT_FALSE(parseJson("{").has_value());
    EXPECT_FALSE(parseJson("{\"a\":}").has_value());
    EXPECT_FALSE(parseJson("not json at all").has_value());
    EXPECT_FALSE(parseJson("").has_value());
    EXPECT_FALSE(parseJson(R"({"a": 1)").has_value());
}

TEST(MinimalJsonTest, RejectsTrailingGarbage) {
    EXPECT_FALSE(parseJson("{} garbage").has_value());
}

TEST(MinimalJsonTest, FindReturnsNullptrForMissingOrNonObjectKey) {
    auto result = parseJson(R"({"a": 1})");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->find("missing"), nullptr);

    auto arrayResult = parseJson("[1, 2, 3]");
    ASSERT_TRUE(arrayResult.has_value());
    EXPECT_EQ(arrayResult->find("anything"), nullptr);
}

TEST(MinimalJsonTest, AsStringReturnsFallbackForNonString) {
    auto result = parseJson(R"({"a": 1})");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->find("a")->asString("fallback"), "fallback");
}
