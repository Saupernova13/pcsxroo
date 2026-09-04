// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <rapidjson/document.h>

#include <string>
#include <string_view>
#include <vector>

// One parsed protocol line.
//
// args points into doc (or at empty_args when the request carried none), so a request must
// outlive any use of args. Both live here together for exactly that reason.
struct DebugServerRequest
{
	std::string cmd;
	rapidjson::Document doc;
	rapidjson::Value id;                                    // null when the request had none
	rapidjson::Value empty_args{rapidjson::kObjectType};    // stand-in so args is never null
	const rapidjson::Value* args = nullptr;
};

namespace DebugServerJson
{
	// Parses one protocol line. Never throws; returns false with a reason in error.
	bool ParseRequest(std::string_view line, DebugServerRequest& out, std::string& error);

	// Both return a complete response object with no trailing newline. Framing belongs to
	// the socket writer, which is the only place that knows about the connection.
	std::string MakeError(const rapidjson::Value& id, const char* code, const std::string& message);
	std::string MakeResult(const rapidjson::Value& id, const rapidjson::Value& result,
		rapidjson::Document::AllocatorType& allocator);

	// "0x0012bbd0" - lowercase and zero padded, stable across the protocol.
	std::string HexU32(u32 value);

	// Accepts "0x12BBD0", bare hex "12BBD0", or decimal "1227728". Expressions are rejected
	// here; the caller falls back to the expression parser, which needs the CPU thread.
	bool ParseAddressLiteral(std::string_view text, u32& out);

	std::string BytesToHex(const u8* data, size_t size);
	bool HexToBytes(std::string_view text, std::vector<u8>& out);
	std::string BytesToBase64(const u8* data, size_t size);
	bool Base64ToBytes(std::string_view text, std::vector<u8>& out);
} // namespace DebugServerJson
