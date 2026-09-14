// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsxroo/cli/Commands.h"

#include <rapidjson/document.h>

#include <gtest/gtest.h>

namespace
{
	bool Build(std::vector<std::string> argv, PcsxrooCommands::Request& out, std::string& error,
		const PcsxrooArgs::Global& global = {})
	{
		return PcsxrooCommands::Build(std::move(argv), global, out, error);
	}

	// The args object of a built request.
	const rapidjson::Value& Args(rapidjson::Document& doc, const PcsxrooCommands::Request& request)
	{
		doc.Parse(request.json.c_str());
		EXPECT_FALSE(doc.HasParseError()) << request.json;
		return doc["args"];
	}
} // namespace

TEST(PcsxrooCommands, BuildsABreakpointRequest)
{
	PcsxrooCommands::Request request;
	std::string error;
	ASSERT_TRUE(Build({"bp", "add", "0x12BBD0", "--cond", "a0 == 2"}, request, error)) << error;

	rapidjson::Document doc;
	const rapidjson::Value& args = Args(doc, request);
	EXPECT_EQ(request.cmd, "bp.add");
	EXPECT_STREQ(args["addr"].GetString(), "0x12BBD0");
	EXPECT_STREQ(args["condition"].GetString(), "a0 == 2");
}

// A mistyped option used to fall through as a positional, and became the breakpoint address.
TEST(PcsxrooCommands, AMistypedOptionIsAnErrorNotAnArgument)
{
	PcsxrooCommands::Request request;
	std::string error;
	EXPECT_FALSE(Build({"bp", "add", "--condd", "x", "0x100"}, request, error));
	EXPECT_NE(error.find("--condd"), std::string::npos) << error;
}

TEST(PcsxrooCommands, ExtraArgumentsAreRejected)
{
	PcsxrooCommands::Request request;
	std::string error;
	EXPECT_FALSE(Build({"status", "now"}, request, error));
	EXPECT_FALSE(Build({"eval", "a0", "==", "2"}, request, error)) << "an unquoted expression is not silently truncated";
	EXPECT_FALSE(Build({"patch", "reload", "twice"}, request, error));
}

TEST(PcsxrooCommands, StepModeIsValidated)
{
	PcsxrooCommands::Request request;
	std::string error;
	EXPECT_FALSE(Build({"step", "sideways"}, request, error));
	EXPECT_TRUE(Build({"step", "over"}, request, error)) << error;
}

TEST(PcsxrooCommands, StickAxesMustBeNumbersInRange)
{
	PcsxrooCommands::Request request;
	std::string error;
	EXPECT_FALSE(Build({"input", "set", "--left-stick", "2,0"}, request, error));
	EXPECT_FALSE(Build({"input", "set", "--left-stick", "abc,0"}, request, error));
	EXPECT_FALSE(Build({"input", "set", "--left-stick", "0.5"}, request, error));

	ASSERT_TRUE(Build({"input", "set", "Cross", "--left-stick", "0.5,-1"}, request, error)) << error;
	rapidjson::Document doc;
	const rapidjson::Value& args = Args(doc, request);
	EXPECT_DOUBLE_EQ(args["analog"]["left"]["x"].GetDouble(), 0.5);
	EXPECT_DOUBLE_EQ(args["analog"]["left"]["y"].GetDouble(), -1.0);
	EXPECT_STREQ(args["buttons"][0].GetString(), "Cross");
}

TEST(PcsxrooCommands, SavestateTakesExactlyOneOfSlotAndPath)
{
	PcsxrooCommands::Request request;
	std::string error;
	EXPECT_FALSE(Build({"savestate", "--slot", "1", "--path", "a.p2s"}, request, error));
	EXPECT_FALSE(Build({"loadstate"}, request, error));
	EXPECT_TRUE(Build({"savestate", "--slot", "1", "--wait-flush"}, request, error)) << error;
}

TEST(PcsxrooCommands, BootTakesExactlyOneSource)
{
	PcsxrooCommands::Request request;
	std::string error;
	EXPECT_FALSE(Build({"boot"}, request, error));
	EXPECT_FALSE(Build({"boot", "--bios", "game.iso"}, request, error));
	EXPECT_TRUE(Build({"boot", "--elf", "game.elf", "--pause-on-entry"}, request, error)) << error;
}

TEST(PcsxrooCommands, Base64AppliesOnlyToReadAndWrite)
{
	PcsxrooCommands::Request request;
	std::string error;
	EXPECT_FALSE(Build({"mem", "fill", "0x100000", "4", "00", "--base64"}, request, error));
	EXPECT_TRUE(Build({"mem", "read", "0x100000", "16", "--base64"}, request, error)) << error;
}

// Only unsigned integers used to parse, so negative and float searches could not be expressed.
TEST(PcsxrooCommands, MemSearchAcceptsNegativeAndRealValues)
{
	PcsxrooCommands::Request request;
	std::string error;
	rapidjson::Document doc;

	ASSERT_TRUE(Build({"mem", "search", "--range", "0x100000:0x200000", "--type", "i32", "--eq", "-1"}, request, error))
		<< error;
	EXPECT_EQ(Args(doc, request)["value"].GetInt64(), -1);

	ASSERT_TRUE(Build({"mem", "search", "--session", "3", "--type", "f32", "--gt", "1.5"}, request, error)) << error;
	EXPECT_DOUBLE_EQ(Args(doc, request)["value"].GetDouble(), 1.5);

	EXPECT_FALSE(Build({"mem", "search", "--range", "0x100000:0x200000", "--eq", "two"}, request, error));
	EXPECT_FALSE(Build({"mem", "search", "--range", "0x100000:0x200000", "--eq", "1", "--gt", "2"}, request, error))
		<< "a second comparison is not silently dropped";
}

// The socket timeout is derived from this, so a command the server is still finishing is not
// cut off by the client.
TEST(PcsxrooCommands, CommandsThatWaitCarryTheirOwnTimeout)
{
	PcsxrooCommands::Request request;
	std::string error;

	ASSERT_TRUE(Build({"wait"}, request, error)) << error;
	EXPECT_EQ(request.command_timeout_ms, 60000u);

	ASSERT_TRUE(Build({"frame-advance", "3"}, request, error)) << error;
	EXPECT_EQ(request.command_timeout_ms, 10000u);

	PcsxrooArgs::Global global;
	global.timeout_ms = 1234;
	global.timeout_explicit = true;
	ASSERT_TRUE(Build({"step", "into"}, request, error, global)) << error;
	EXPECT_EQ(request.command_timeout_ms, 1234u);
	rapidjson::Document doc;
	EXPECT_EQ(Args(doc, request)["timeout_ms"].GetUint(), 1234u);

	PcsxrooCommands::Request status;
	ASSERT_TRUE(Build({"status"}, status, error)) << error;
	EXPECT_EQ(status.command_timeout_ms, 0u);
}
