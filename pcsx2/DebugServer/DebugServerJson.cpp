// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "DebugServer/DebugServerJson.h"

#include "common/StringUtil.h"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <fmt/format.h>

#include <algorithm>
#include <charconv>

namespace
{
	constexpr std::string_view BASE64_ALPHABET =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	int Base64Value(char c)
	{
		const size_t index = BASE64_ALPHABET.find(c);
		return index == std::string_view::npos ? -1 : static_cast<int>(index);
	}

	std::string Serialise(const rapidjson::Document& doc)
	{
		rapidjson::StringBuffer buffer;
		rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
		doc.Accept(writer);
		return std::string(buffer.GetString(), buffer.GetSize());
	}

	// Every response carries the request's id, so a client that pipelines requests can match
	// replies to them. A request with no id gets a null one back rather than none at all.
	void AddId(rapidjson::Document& doc, const rapidjson::Value& id)
	{
		rapidjson::Value copy;
		copy.CopyFrom(id, doc.GetAllocator());
		doc.AddMember("id", copy, doc.GetAllocator());
	}
} // namespace

bool DebugServerJson::ParseRequest(std::string_view line, DebugServerRequest& out, std::string& error)
{
	out.doc.Parse(line.data(), line.size());

	if (out.doc.HasParseError())
	{
		error = fmt::format("invalid JSON at offset {}", out.doc.GetErrorOffset());
		return false;
	}

	if (!out.doc.IsObject())
	{
		error = "request must be a JSON object";
		return false;
	}

	const auto cmd = out.doc.FindMember("cmd");
	if (cmd == out.doc.MemberEnd() || !cmd->value.IsString())
	{
		error = "request is missing a string \"cmd\"";
		return false;
	}

	out.cmd.assign(cmd->value.GetString(), cmd->value.GetStringLength());

	const auto id = out.doc.FindMember("id");
	if (id != out.doc.MemberEnd())
		out.id.CopyFrom(id->value, out.doc.GetAllocator());
	else
		out.id.SetNull();

	const auto args = out.doc.FindMember("args");
	out.args = (args != out.doc.MemberEnd() && args->value.IsObject()) ? &args->value : &out.empty_args;

	return true;
}

std::string DebugServerJson::MakeError(const rapidjson::Value& id, const char* code, const std::string& message)
{
	rapidjson::Document doc;
	doc.SetObject();
	auto& allocator = doc.GetAllocator();

	AddId(doc, id);
	doc.AddMember("ok", false, allocator);

	rapidjson::Value error(rapidjson::kObjectType);
	error.AddMember("code", rapidjson::Value(code, allocator), allocator);
	error.AddMember("message", rapidjson::Value(message.c_str(), static_cast<rapidjson::SizeType>(message.size()),
									 allocator),
		allocator);
	doc.AddMember("error", error, allocator);

	return Serialise(doc);
}

std::string DebugServerJson::MakeResult(const rapidjson::Value& id, const rapidjson::Value& result,
	rapidjson::Document::AllocatorType& allocator)
{
	rapidjson::Document doc;
	doc.SetObject();

	AddId(doc, id);
	doc.AddMember("ok", true, doc.GetAllocator());

	// Copied rather than moved: result belongs to the caller's allocator, which is usually a
	// different one, and a moved value would dangle when that document goes out of scope.
	rapidjson::Value copy;
	copy.CopyFrom(result, doc.GetAllocator());
	doc.AddMember("result", copy, doc.GetAllocator());

	(void)allocator;
	return Serialise(doc);
}

std::string DebugServerJson::HexU32(u32 value)
{
	return fmt::format("0x{:08x}", value);
}

bool DebugServerJson::ParseAddressLiteral(std::string_view text, u32& out)
{
	if (text.empty())
		return false;

	int base = 10;
	if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
	{
		text.remove_prefix(2);
		base = 16;
	}
	else if (!std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; }))
	{
		// Not all digits, so it is either bare hex or something we cannot resolve here.
		if (!std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isxdigit(c) != 0; }))
			return false;

		base = 16;
	}

	if (text.empty())
		return false;

	u64 value = 0;
	const auto result = std::from_chars(text.data(), text.data() + text.size(), value, base);
	if (result.ec != std::errc() || result.ptr != text.data() + text.size())
		return false;

	if (value > 0xFFFFFFFFull)
		return false;

	out = static_cast<u32>(value);
	return true;
}

std::string DebugServerJson::BytesToHex(const u8* data, size_t size)
{
	return StringUtil::EncodeHex(data, static_cast<int>(size));
}

bool DebugServerJson::HexToBytes(std::string_view text, std::vector<u8>& out)
{
	if ((text.size() % 2) != 0)
		return false;

	std::optional<std::vector<u8>> decoded = StringUtil::DecodeHex(text);
	if (!decoded.has_value())
		return false;

	out = std::move(decoded.value());
	return true;
}

std::string DebugServerJson::BytesToBase64(const u8* data, size_t size)
{
	std::string out;
	out.reserve(((size + 2) / 3) * 4);

	size_t i = 0;
	for (; i + 2 < size; i += 3)
	{
		const u32 group = (static_cast<u32>(data[i]) << 16) | (static_cast<u32>(data[i + 1]) << 8) | data[i + 2];
		out += BASE64_ALPHABET[(group >> 18) & 0x3F];
		out += BASE64_ALPHABET[(group >> 12) & 0x3F];
		out += BASE64_ALPHABET[(group >> 6) & 0x3F];
		out += BASE64_ALPHABET[group & 0x3F];
	}

	if (i < size)
	{
		const bool has_second = (i + 1) < size;
		const u32 group = (static_cast<u32>(data[i]) << 16) | (has_second ? (static_cast<u32>(data[i + 1]) << 8) : 0);
		out += BASE64_ALPHABET[(group >> 18) & 0x3F];
		out += BASE64_ALPHABET[(group >> 12) & 0x3F];
		out += has_second ? BASE64_ALPHABET[(group >> 6) & 0x3F] : '=';
		out += '=';
	}

	return out;
}

bool DebugServerJson::Base64ToBytes(std::string_view text, std::vector<u8>& out)
{
	if (text.empty() || (text.size() % 4) != 0)
		return false;

	size_t padding = 0;
	while (padding < 2 && !text.empty() && text.back() == '=')
	{
		text.remove_suffix(1);
		padding++;
	}

	out.clear();
	out.reserve((text.size() / 4) * 3);

	u32 group = 0;
	int bits = 0;
	for (const char c : text)
	{
		const int value = Base64Value(c);
		if (value < 0)
			return false;

		group = (group << 6) | static_cast<u32>(value);
		bits += 6;

		if (bits >= 8)
		{
			bits -= 8;
			out.push_back(static_cast<u8>((group >> bits) & 0xFF));
		}
	}

	return true;
}
