// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsxroo/cli/Render.h"

#include <gtest/gtest.h>

#include <string>

namespace
{
	// Renders a reply parsed from text and returns what was printed.
	std::string Render(const std::string& cmd, const char* json)
	{
		rapidjson::Document doc;
		doc.Parse(json);
		EXPECT_FALSE(doc.HasParseError()) << json;

		testing::internal::CaptureStdout();
		PcsxrooRender::Result(cmd, doc);
		return testing::internal::GetCapturedStdout();
	}
} // namespace

// Each of these used to crash or throw: an absent or mistyped member was dereferenced, an
// empty opcode had its prefix cut off, and odd or non-hex data went through std::stoi.
TEST(PcsxrooRender, SurvivesRepliesOfAnUnexpectedShape)
{
	Render("bp.list", "{}");
	Render("bp.list", R"({"breakpoints":null})");
	Render("dis", R"({"instructions":[{"addr_hex":"0x00100000"}]})");
	Render("dis", R"({"instructions":{}})");
	Render("reg.dump", R"({"registers":7})");
	Render("status", R"({"vm_state":"running","game":null,"ee":"x","last_stop":[]})");
	Render("status", "null");
	Render("mem.read", R"({"format":"hex","addr":1048576,"data":"abc"})");
	Render("mem.read", R"({"format":"hex","addr":1048576,"data":"zz11"})");
	Render("wait", "[]");
}

TEST(PcsxrooRender, HexDumpShowsBytesAndAscii)
{
	const std::string out = Render("mem.read", R"({"format":"hex","addr":1048576,"data":"41424300"})");
	EXPECT_NE(out.find("0x00100000"), std::string::npos) << out;
	EXPECT_NE(out.find("41 42 43 00"), std::string::npos) << out;
	EXPECT_NE(out.find("ABC."), std::string::npos) << out;
}

TEST(PcsxrooRender, NonHexMemoryFallsBackToTheGenericDump)
{
	const std::string out = Render("mem.read", R"({"format":"hex","addr":1048576,"data":"zz11"})");
	EXPECT_NE(out.find("zz11"), std::string::npos) << out;
}

TEST(PcsxrooRender, DisassemblyDropsOnlyARealHexPrefix)
{
	const std::string out = Render("dis",
		R"({"instructions":[{"addr_hex":"0x00100000","opcode_hex":"0x27bdfff0","text":"addiu sp,sp,-0x10"}]})");
	EXPECT_NE(out.find("27bdfff0"), std::string::npos) << out;
	EXPECT_EQ(out.find("0x27bdfff0"), std::string::npos) << out;
}
