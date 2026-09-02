#include <gtest/gtest.h>

#include "../plugins/system-control/plugin_internal.h"

TEST(ParseVolumeArgumentTest, GetReturnsNullopt) {
    EXPECT_EQ(parseVolumeArgument("get"), std::nullopt);
}

TEST(ParseVolumeArgumentTest, SetWithValidLevelReturnsThatLevel) {
    EXPECT_EQ(parseVolumeArgument("set 42"), std::optional<int>(42));
}

TEST(ParseVolumeArgumentTest, SetWithZeroIsValid) {
    EXPECT_EQ(parseVolumeArgument("set 0"), std::optional<int>(0));
}

TEST(ParseVolumeArgumentTest, SetWithOneHundredIsValid) {
    EXPECT_EQ(parseVolumeArgument("set 100"), std::optional<int>(100));
}

TEST(ParseVolumeArgumentTest, SetAboveOneHundredIsRejected) {
    EXPECT_EQ(parseVolumeArgument("set 101"), std::nullopt);
}

TEST(ParseVolumeArgumentTest, SetBelowZeroIsRejected) {
    EXPECT_EQ(parseVolumeArgument("set -1"), std::nullopt);
}

TEST(ParseVolumeArgumentTest, SetWithNonNumericLevelIsRejected) {
    EXPECT_EQ(parseVolumeArgument("set loud"), std::nullopt);
}

TEST(ParseVolumeArgumentTest, EmptyPayloadIsRejected) {
    EXPECT_EQ(parseVolumeArgument(""), std::nullopt);
}

TEST(ParseVolumeArgumentTest, UnknownVerbIsRejected) {
    EXPECT_EQ(parseVolumeArgument("mute"), std::nullopt);
}

TEST(BuildVolumeArgvTest, ProducesPactlSetSinkVolumeCommand) {
    std::vector<std::string> argv = buildVolumeArgv(50);

    ASSERT_EQ(argv.size(), 4u);
    EXPECT_EQ(argv[0], "pactl");
    EXPECT_EQ(argv[1], "set-sink-volume");
    EXPECT_EQ(argv[2], "@DEFAULT_SINK@");
    EXPECT_EQ(argv[3], "50%");
}
