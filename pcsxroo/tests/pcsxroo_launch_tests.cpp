// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsxroo/cli/Launch.h"

#include <gtest/gtest.h>

namespace
{
	bool Parse(std::vector<std::string> argv, PcsxrooLaunch::Options& options, std::string& error)
	{
		return PcsxrooLaunch::ParseOptions(argv, 28110, options, error);
	}
} // namespace

TEST(PcsxrooLaunch, AcceptsAGameAndItsFlags)
{
	PcsxrooLaunch::Options options;
	std::string error;
	ASSERT_TRUE(Parse({"--pause-on-entry", "game.iso", "--ready-timeout", "120000"}, options, error)) << error;

	EXPECT_EQ(options.game, "game.iso");
	EXPECT_TRUE(options.pause_on_entry);
	EXPECT_EQ(options.ready_timeout_ms, 120000u);
}

TEST(PcsxrooLaunch, AnEmptyCommandLineStartsAnIdleEmulator)
{
	PcsxrooLaunch::Options options;
	std::string error;
	ASSERT_TRUE(Parse({}, options, error)) << error;
	EXPECT_TRUE(options.game.empty());
	EXPECT_FALSE(options.boot_bios);
}

// A mistyped flag used to become the game path, and the emulator was then asked to boot it.
TEST(PcsxrooLaunch, RejectsAnUnknownOption)
{
	PcsxrooLaunch::Options options;
	std::string error;
	EXPECT_FALSE(Parse({"--pause-on-entyr", "game.iso"}, options, error));
	EXPECT_NE(error.find("--pause-on-entyr"), std::string::npos) << error;
}

TEST(PcsxrooLaunch, RejectsASecondGame)
{
	PcsxrooLaunch::Options options;
	std::string error;
	EXPECT_FALSE(Parse({"one.iso", "two.iso"}, options, error));
}

TEST(PcsxrooLaunch, RejectsBiosTogetherWithAGame)
{
	PcsxrooLaunch::Options options;
	std::string error;
	EXPECT_FALSE(Parse({"--bios", "game.iso"}, options, error));
}

// The value used to be cast straight to 32 bits, so a large one wrapped to a tiny timeout.
TEST(PcsxrooLaunch, RejectsAReadyTimeoutOutOfRange)
{
	PcsxrooLaunch::Options options;
	std::string error;
	EXPECT_FALSE(Parse({"--ready-timeout", "4294967297"}, options, error));
	EXPECT_FALSE(Parse({"--ready-timeout", "0"}, options, error));
}
