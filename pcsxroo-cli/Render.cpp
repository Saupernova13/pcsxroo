// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsxroo-cli/Render.h"

#include "common/Pcsx2Types.h"

#include <fmt/format.h>

#include <cctype>
#include <string_view>

namespace
{
	std::string Scalar(const rapidjson::Value& value)
	{
		if (value.IsString())
			return std::string(value.GetString(), value.GetStringLength());
		if (value.IsBool())
			return value.GetBool() ? "true" : "false";
		if (value.IsUint64())
			return std::to_string(value.GetUint64());
		if (value.IsInt64())
			return std::to_string(value.GetInt64());
		if (value.IsDouble())
			return fmt::format("{}", value.GetDouble());
		if (value.IsNull())
			return "null";

		return {};
	}

	std::string Text(const rapidjson::Value& parent, const char* name, const char* fallback = "")
	{
		const auto it = parent.FindMember(name);
		if (it == parent.MemberEnd())
			return fallback;

		return Scalar(it->value);
	}

	bool Has(const rapidjson::Value& parent, const char* name)
	{
		return parent.FindMember(name) != parent.MemberEnd();
	}

	// A key/value dump that stays readable for shapes with no dedicated renderer, so a new
	// server command is usable the moment it exists.
	void Generic(const rapidjson::Value& value, int indent = 0)
	{
		const std::string pad(static_cast<size_t>(indent) * 2, ' ');

		if (value.IsObject())
		{
			for (auto it = value.MemberBegin(); it != value.MemberEnd(); ++it)
			{
				const std::string_view name(it->name.GetString(), it->name.GetStringLength());

				// The _hex siblings duplicate their numeric partner; showing both doubles
				// the output for no gain.
				if (name.size() > 4 && name.substr(name.size() - 4) == "_hex")
					continue;

				if (it->value.IsObject() || it->value.IsArray())
				{
					fmt::print("{}{}:\n", pad, name);
					Generic(it->value, indent + 1);
					continue;
				}

				const auto hex = value.FindMember((std::string(name) + "_hex").c_str());
				if (hex != value.MemberEnd() && hex->value.IsString())
					fmt::print("{}{:<14} {}\n", pad, std::string(name) + ":", Scalar(hex->value));
				else
					fmt::print("{}{:<14} {}\n", pad, std::string(name) + ":", Scalar(it->value));
			}

			return;
		}

		if (value.IsArray())
		{
			if (value.Empty())
			{
				fmt::print("{}(none)\n", pad);
				return;
			}

			for (const rapidjson::Value& item : value.GetArray())
			{
				Generic(item, indent);
				if (item.IsObject() || item.IsArray())
					fmt::print("\n");
			}

			return;
		}

		fmt::print("{}{}\n", pad, Scalar(value));
	}

	void Stop(const rapidjson::Value& stop)
	{
		fmt::print("stopped  {} at {} (seq {}, {})\n", Text(stop, "reason"), Text(stop, "pc_hex"),
			Text(stop, "seq"), Text(stop, "cpu"));
	}

	void Status(const rapidjson::Value& result)
	{
		fmt::print("state    {}{}\n", Text(result, "vm_state"),
			Text(result, "paused") == "true" ? " (paused)" : "");

		if (Has(result, "game"))
		{
			const rapidjson::Value& game = result["game"];
			fmt::print("game     {} [{}] crc {}\n", Text(game, "title"), Text(game, "serial"),
				Text(game, "crc_hex"));
		}

		if (Has(result, "ee"))
			fmt::print("ee pc    {}\n", Text(result["ee"], "pc_hex"));
		if (Has(result, "iop"))
			fmt::print("iop pc   {}\n", Text(result["iop"], "pc_hex"));

		if (Has(result, "last_stop") && Text(result["last_stop"], "reason") != "none")
			Stop(result["last_stop"]);
	}

	void Disassembly(const rapidjson::Value& result)
	{
		if (!Has(result, "instructions"))
			return;

		std::string current_symbol;
		for (const rapidjson::Value& line : result["instructions"].GetArray())
		{
			const std::string symbol = Text(line, "symbol");
			if (!symbol.empty() && symbol != current_symbol)
			{
				current_symbol = symbol;
				fmt::print("\n{}:\n", symbol);
			}

			fmt::print("  {}  {}  {}\n", Text(line, "addr_hex"), Text(line, "opcode_hex").substr(2),
				Text(line, "text"));
		}
	}

	void Registers(const rapidjson::Value& result)
	{
		if (!Has(result, "registers"))
			return;

		fmt::print("pc  {}\n", Text(result, "pc_hex"));

		int column = 0;
		for (const rapidjson::Value& reg : result["registers"].GetArray())
		{
			fmt::print("{:<6} {:<18}", Text(reg, "name"), Text(reg, "string"));
			if (++column % 3 == 0)
				fmt::print("\n");
		}

		if (column % 3 != 0)
			fmt::print("\n");
	}

	void Breakpoints(const rapidjson::Value& result)
	{
		const rapidjson::Value& list = result["breakpoints"];
		if (list.Empty())
		{
			fmt::print("no breakpoints\n");
			return;
		}

		fmt::print("{:<12} {:<4} {:<8} {}\n", "ADDRESS", "CPU", "STATE", "CONDITION / DESCRIPTION");
		for (const rapidjson::Value& bp : list.GetArray())
		{
			std::string state = Text(bp, "enabled") == "true" ? "enabled" : "disabled";
			if (Text(bp, "temporary") == "true")
				state += "*";

			std::string detail = Text(bp, "condition");
			const std::string description = Text(bp, "description");
			if (!description.empty())
				detail += detail.empty() ? description : "  " + description;

			fmt::print("{:<12} {:<4} {:<8} {}\n", Text(bp, "addr_hex"), Text(bp, "cpu"), state, detail);
		}
	}

	void MemoryRead(const rapidjson::Value& result)
	{
		const std::string data = Text(result, "data");
		if (Text(result, "format") != "hex")
		{
			Generic(result);
			return;
		}

		u64 base = 0;
		const auto addr = result.FindMember("addr");
		if (addr != result.MemberEnd() && addr->value.IsUint64())
			base = addr->value.GetUint64();

		for (size_t offset = 0; offset < data.size(); offset += 32)
		{
			const std::string row = data.substr(offset, 32);
			std::string spaced;
			std::string ascii;

			for (size_t i = 0; i < row.size(); i += 2)
			{
				spaced += row.substr(i, 2);
				spaced += ' ';

				const int byte = std::stoi(row.substr(i, 2), nullptr, 16);
				ascii += (byte >= 0x20 && byte < 0x7F) ? static_cast<char>(byte) : '.';
			}

			fmt::print("0x{:08x}  {:<48} {}\n", base + (offset / 2), spaced, ascii);
		}
	}
} // namespace

void PcsxrooRender::Result(const std::string& cmd, const rapidjson::Value& result)
{
	if (cmd == "status")
		Status(result);
	else if (cmd == "dis")
		Disassembly(result);
	else if (cmd == "reg.dump")
		Registers(result);
	else if (cmd == "bp.list")
		Breakpoints(result);
	else if (cmd == "mem.read")
		MemoryRead(result);
	else if (cmd == "wait" || cmd == "step" || cmd == "pause")
		Stop(result);
	else
		Generic(result);
}

void PcsxrooRender::Event(const rapidjson::Value& event)
{
	Stop(event);
}
