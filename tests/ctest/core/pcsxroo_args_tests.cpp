// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsxroo-cli/Args.h"

#include <gtest/gtest.h>

TEST(PcsxrooArgs, DefaultsMatchTheProtocol)
{
	std::vector<std::string> argv{"status"};
	PcsxrooArgs::Global global;
	std::string error;
	ASSERT_TRUE(PcsxrooArgs::ParseGlobal(argv, global, error)) << error;

	EXPECT_EQ(global.host, "127.0.0.1");
	EXPECT_EQ(global.cpu, "ee");
	EXPECT_FALSE(global.json);
	EXPECT_EQ(global.timeout_ms, 5000u);
	EXPECT_FALSE(global.timeout_explicit);

	ASSERT_EQ(argv.size(), 1u) << "global flags are consumed, the rest is left alone";
	EXPECT_EQ(argv[0], "status");
}

TEST(PcsxrooArgs, ConsumesGlobalFlagsFromAnywhere)
{
	std::vector<std::string> argv{"--port", "9000", "bp", "add", "--json", "0x100"};
	PcsxrooArgs::Global global;
	std::string error;
	ASSERT_TRUE(PcsxrooArgs::ParseGlobal(argv, global, error)) << error;

	EXPECT_EQ(global.port, 9000);
	EXPECT_TRUE(global.json);

	ASSERT_EQ(argv.size(), 3u);
	EXPECT_EQ(argv[0], "bp");
	EXPECT_EQ(argv[1], "add");
	EXPECT_EQ(argv[2], "0x100");
}

TEST(PcsxrooArgs, RejectsABadPort)
{
	std::vector<std::string> argv{"--port", "notaport", "status"};
	PcsxrooArgs::Global global;
	std::string error;
	EXPECT_FALSE(PcsxrooArgs::ParseGlobal(argv, global, error));
	EXPECT_FALSE(error.empty());
}

TEST(PcsxrooArgs, RejectsAFlagWithNoValue)
{
	std::vector<std::string> argv{"status", "--port"};
	PcsxrooArgs::Global global;
	std::string error;
	EXPECT_FALSE(PcsxrooArgs::ParseGlobal(argv, global, error));
	EXPECT_FALSE(error.empty());
}

TEST(PcsxrooArgs, RejectsAnUnknownCpu)
{
	std::vector<std::string> argv{"--cpu", "vu0", "status"};
	PcsxrooArgs::Global global;
	std::string error;
	EXPECT_FALSE(PcsxrooArgs::ParseGlobal(argv, global, error));
}

// An explicit --timeout has to be distinguishable from the default, because wait and step
// use a much longer default of their own and must not silently override the user's value.
TEST(PcsxrooArgs, RecordsWhetherTimeoutWasGivenExplicitly)
{
	std::vector<std::string> argv{"--timeout", "1234", "wait"};
	PcsxrooArgs::Global global;
	std::string error;
	ASSERT_TRUE(PcsxrooArgs::ParseGlobal(argv, global, error)) << error;

	EXPECT_EQ(global.timeout_ms, 1234u);
	EXPECT_TRUE(global.timeout_explicit);
}

TEST(PcsxrooArgs, ParsesEveryNumberFormTheProtocolAccepts)
{
	u64 value = 0;
	ASSERT_TRUE(PcsxrooArgs::ParseNumber("0x12BBD0", value));
	EXPECT_EQ(value, 0x12BBD0u);

	ASSERT_TRUE(PcsxrooArgs::ParseNumber("1227728", value));
	EXPECT_EQ(value, 1227728u);

	EXPECT_FALSE(PcsxrooArgs::ParseNumber("main+4", value)) << "expressions pass through to the server";
	EXPECT_FALSE(PcsxrooArgs::ParseNumber("", value));
}

TEST(PcsxrooArgs, TakeOptionRemovesBothTokens)
{
	std::vector<std::string> argv{"bp", "add", "--cond", "$a0 == 2", "0x100"};
	std::string value;
	std::string error;

	ASSERT_TRUE(PcsxrooArgs::TakeOption(argv, "--cond", value, error));
	EXPECT_EQ(value, "$a0 == 2");
	ASSERT_EQ(argv.size(), 3u);
	EXPECT_EQ(argv[2], "0x100");
}

TEST(PcsxrooArgs, TakeFlagRemovesOnlyTheFlag)
{
	std::vector<std::string> argv{"bp", "list", "--include-temp"};
	EXPECT_TRUE(PcsxrooArgs::TakeFlag(argv, "--include-temp"));
	EXPECT_EQ(argv.size(), 2u);
	EXPECT_FALSE(PcsxrooArgs::TakeFlag(argv, "--include-temp"));
}
