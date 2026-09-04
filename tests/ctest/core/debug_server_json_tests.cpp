// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "DebugServer/DebugServerJson.h"

#include <gtest/gtest.h>

#include <cstring>

TEST(DebugServerJson, ParsesAWellFormedRequest)
{
	DebugServerRequest request;
	std::string error;
	ASSERT_TRUE(DebugServerJson::ParseRequest(
		R"({"id":7,"cmd":"bp.add","args":{"addr":1227728}})", request, error))
		<< error;

	EXPECT_EQ(request.cmd, "bp.add");
	ASSERT_NE(request.args, nullptr);
	EXPECT_EQ((*request.args)["addr"].GetUint(), 1227728u);
}

TEST(DebugServerJson, RejectsMalformedJsonWithoutThrowing)
{
	DebugServerRequest request;
	std::string error;
	EXPECT_FALSE(DebugServerJson::ParseRequest("{not json", request, error));
	EXPECT_FALSE(error.empty());
}

TEST(DebugServerJson, RejectsARequestWithNoCommand)
{
	DebugServerRequest request;
	std::string error;
	EXPECT_FALSE(DebugServerJson::ParseRequest(R"({"id":1})", request, error));
}

TEST(DebugServerJson, RejectsANonObjectRoot)
{
	DebugServerRequest request;
	std::string error;
	EXPECT_FALSE(DebugServerJson::ParseRequest(R"([1,2,3])", request, error));
}

TEST(DebugServerJson, MissingArgsBecomesAnEmptyObjectNotANullPointer)
{
	DebugServerRequest request;
	std::string error;
	ASSERT_TRUE(DebugServerJson::ParseRequest(R"({"id":1,"cmd":"status"})", request, error));
	ASSERT_NE(request.args, nullptr);
	EXPECT_TRUE(request.args->IsObject());
	EXPECT_EQ(request.args->MemberCount(), 0u);
}

TEST(DebugServerJson, ErrorObjectCarriesCodeAndMessage)
{
	DebugServerRequest request;
	std::string error;
	ASSERT_TRUE(DebugServerJson::ParseRequest(R"({"id":7,"cmd":"status"})", request, error));

	const std::string line = DebugServerJson::MakeError(request.id, "not_paused", "needs a paused VM");
	EXPECT_NE(line.find(R"("ok":false)"), std::string::npos);
	EXPECT_NE(line.find(R"("code":"not_paused")"), std::string::npos);
	EXPECT_NE(line.find("needs a paused VM"), std::string::npos);
	EXPECT_NE(line.find(R"("id":7)"), std::string::npos);
	EXPECT_EQ(line.find('\n'), std::string::npos)
		<< "the framing newline is added by the socket writer, not the encoder";
}

TEST(DebugServerJson, ResultObjectEchoesTheId)
{
	DebugServerRequest request;
	std::string error;
	ASSERT_TRUE(DebugServerJson::ParseRequest(R"({"id":"abc","cmd":"status"})", request, error));

	rapidjson::Document result;
	result.SetObject();
	result.AddMember("addr", 1227728, result.GetAllocator());

	const std::string line = DebugServerJson::MakeResult(request.id, result, result.GetAllocator());
	EXPECT_NE(line.find(R"("ok":true)"), std::string::npos);
	EXPECT_NE(line.find(R"("id":"abc")"), std::string::npos);
	EXPECT_NE(line.find(R"("addr":1227728)"), std::string::npos);
}

TEST(DebugServerJson, ParsesEveryAddressLiteralForm)
{
	u32 value = 0;
	ASSERT_TRUE(DebugServerJson::ParseAddressLiteral("0x12BBD0", value));
	EXPECT_EQ(value, 0x12BBD0u);

	ASSERT_TRUE(DebugServerJson::ParseAddressLiteral("12BBD0", value));
	EXPECT_EQ(value, 0x12BBD0u) << "bare hex is the form used throughout the BT3 tooling";

	ASSERT_TRUE(DebugServerJson::ParseAddressLiteral("1227728", value));
	EXPECT_EQ(value, 1227728u) << "an all-digit literal is decimal";

	EXPECT_FALSE(DebugServerJson::ParseAddressLiteral("main+0x40", value)) << "expressions are not literals";
	EXPECT_FALSE(DebugServerJson::ParseAddressLiteral("", value));
	EXPECT_FALSE(DebugServerJson::ParseAddressLiteral("0x1234567890", value)) << "wider than 32 bits";
}

TEST(DebugServerJson, HexFormattingIsStableAndLowercase)
{
	EXPECT_EQ(DebugServerJson::HexU32(0x12BBD0), "0x0012bbd0");
	EXPECT_EQ(DebugServerJson::HexU32(0), "0x00000000");
	EXPECT_EQ(DebugServerJson::HexU32(0xFFFFFFFF), "0xffffffff");
}

TEST(DebugServerJson, HexRoundTripsBytes)
{
	const u8 bytes[] = {0x00, 0x0F, 0xA5, 0xFF};
	const std::string hex = DebugServerJson::BytesToHex(bytes, sizeof(bytes));
	EXPECT_EQ(hex, "000fa5ff");

	std::vector<u8> out;
	ASSERT_TRUE(DebugServerJson::HexToBytes(hex, out));
	ASSERT_EQ(out.size(), sizeof(bytes));
	EXPECT_EQ(std::memcmp(out.data(), bytes, sizeof(bytes)), 0);

	EXPECT_FALSE(DebugServerJson::HexToBytes("abc", out)) << "odd length";
	EXPECT_FALSE(DebugServerJson::HexToBytes("zz", out)) << "not hex";
}

TEST(DebugServerJson, Base64RoundTripsBytes)
{
	// Five bytes exercises both padding cases across the 3-byte group boundary.
	const u8 bytes[] = {0x00, 0x0F, 0xA5, 0xFF, 0x10};
	const std::string encoded = DebugServerJson::BytesToBase64(bytes, sizeof(bytes));

	std::vector<u8> out;
	ASSERT_TRUE(DebugServerJson::Base64ToBytes(encoded, out));
	ASSERT_EQ(out.size(), sizeof(bytes));
	EXPECT_EQ(std::memcmp(out.data(), bytes, sizeof(bytes)), 0);
}

TEST(DebugServerJson, Base64MatchesKnownVectors)
{
	const std::string_view text = "PCSXROO";
	EXPECT_EQ(DebugServerJson::BytesToBase64(reinterpret_cast<const u8*>(text.data()), text.size()),
		"UENTWFJPTw==");

	std::vector<u8> out;
	ASSERT_TRUE(DebugServerJson::Base64ToBytes("UENTWFJPTw==", out));
	EXPECT_EQ(std::string(out.begin(), out.end()), "PCSXROO");

	EXPECT_FALSE(DebugServerJson::Base64ToBytes("A", out)) << "length is not a multiple of four";
	EXPECT_FALSE(DebugServerJson::Base64ToBytes("!!!!", out)) << "outside the alphabet";
}
