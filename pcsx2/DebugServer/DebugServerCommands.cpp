// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "DebugServer/DebugServerCommands.h"

#include "DebugServer/DebugServer.h"
#include "DebugServer/DebugServerDispatch.h"

#include "BuildVersion.h"
#include "VMManager.h"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <unordered_map>

namespace
{
	std::unordered_map<std::string, DebugServerHandler> s_handlers;

	using Allocator = rapidjson::Document::AllocatorType;

	rapidjson::Value Str(const std::string& text, Allocator& allocator)
	{
		return rapidjson::Value(text.c_str(), static_cast<rapidjson::SizeType>(text.size()), allocator);
	}

	// Addresses go out as a number plus a "_hex" sibling: the number is what a program wants,
	// the hex string is what a person reads in a log.
	void AddAddress(rapidjson::Value& object, const char* name, u32 value, Allocator& allocator)
	{
		// rapidjson's primitive AddMember overload takes the name by lvalue reference, so a
		// temporary Value name only matches the Value/Value overload; hence the explicit
		// rapidjson::Value around the number.
		object.AddMember(rapidjson::Value(name, allocator), rapidjson::Value(value), allocator);
		object.AddMember(Str(std::string(name) + "_hex", allocator),
			Str(DebugServerJson::HexU32(value), allocator), allocator);
	}

	const char* VMStateName()
	{
		switch (VMManager::GetState())
		{
			case VMState::Running:
				return "running";
			case VMState::Paused:
				return "paused";
			case VMState::Stopping:
				return "stopping";
			case VMState::Resetting:
				return "resetting";
			case VMState::Initializing:
				return "initializing";
			case VMState::Shutdown:
			default:
				return "shutdown";
		}
	}

	std::string CmdVersion(const DebugServerRequest& request, DebugServerConnection&)
	{
		rapidjson::Document result;
		result.SetObject();
		auto& allocator = result.GetAllocator();

		result.AddMember("emulator", rapidjson::Value("PCSXROO", allocator), allocator);
		result.AddMember("pcsx2_base", rapidjson::Value(BuildVersion::GitRev, allocator), allocator);
		result.AddMember("pcsx2_hash", rapidjson::Value(BuildVersion::GitHash, allocator), allocator);
		result.AddMember("protocol_version", PCSXROO_DEBUG_PROTOCOL_VERSION, allocator);
		result.AddMember("port", DebugServer::GetPort(), allocator);

		return DebugServerJson::MakeResult(request.id, result, allocator);
	}

	std::string CmdStatus(const DebugServerRequest& request, DebugServerConnection&)
	{
		rapidjson::Document result;
		result.SetObject();
		auto& allocator = result.GetAllocator();

		result.AddMember("vm_state", rapidjson::Value(VMStateName(), allocator), allocator);
		result.AddMember("paused", DebugServerDispatch::VMIsPaused(), allocator);

		rapidjson::Value last_stop(rapidjson::kObjectType);
		DebugServerCommands::WriteStopEvent(DebuggerControl::GetLastStop(), last_stop, allocator);
		result.AddMember("last_stop", last_stop, allocator);

		if (DebugServerDispatch::VMIsValid())
		{
			// Reading the program counters and the game identity both touch VM state, so
			// they are gathered in a single hop onto the CPU thread rather than several.
			u32 ee_pc = 0;
			u32 iop_pc = 0;
			std::string serial;
			std::string title;
			std::string version;
			u32 crc = 0;

			const bool ok = DebugServerDispatch::RunOnCPUThreadWithTimeout(
				[&ee_pc, &iop_pc, &serial, &title, &version, &crc]() {
					ee_pc = r5900Debug.getPC();
					iop_pc = r3000Debug.getPC();
					serial = VMManager::GetDiscSerial();
					title = VMManager::GetTitle(false);
					version = VMManager::GetDiscVersion();
					crc = VMManager::GetCurrentCRC();
				});

			if (!ok)
			{
				return DebugServerJson::MakeError(request.id, "timeout",
					"the CPU thread did not respond; the emulator may be wedged");
			}

			rapidjson::Value ee(rapidjson::kObjectType);
			AddAddress(ee, "pc", ee_pc, allocator);
			result.AddMember("ee", ee, allocator);

			rapidjson::Value iop(rapidjson::kObjectType);
			AddAddress(iop, "pc", iop_pc, allocator);
			result.AddMember("iop", iop, allocator);

			rapidjson::Value game(rapidjson::kObjectType);
			game.AddMember("serial", Str(serial, allocator), allocator);
			game.AddMember("title", Str(title, allocator), allocator);
			game.AddMember("version", Str(version, allocator), allocator);
			game.AddMember("crc", crc, allocator);
			game.AddMember("crc_hex", Str(DebugServerJson::HexU32(crc), allocator), allocator);
			result.AddMember("game", game, allocator);
		}

		return DebugServerJson::MakeResult(request.id, result, allocator);
	}

	std::string CmdSubscribe(const DebugServerRequest& request, DebugServerConnection& connection)
	{
		connection.SetSubscribed(true);

		rapidjson::Document result;
		result.SetObject();
		auto& allocator = result.GetAllocator();

		rapidjson::Value subscribed(rapidjson::kArrayType);
		subscribed.PushBack(rapidjson::Value("stop", allocator), allocator);
		result.AddMember("subscribed", subscribed, allocator);

		return DebugServerJson::MakeResult(request.id, result, allocator);
	}
} // namespace

void DebugServerCommands::WriteStopEvent(const DebuggerControl::StopEvent& stop, rapidjson::Value& out,
	Allocator& allocator)
{
	out.AddMember("seq", stop.seq, allocator);
	out.AddMember("reason", rapidjson::Value(DebuggerControl::StopReasonName(stop.reason), allocator), allocator);
	out.AddMember("cpu", rapidjson::Value(stop.cpu == BREAKPOINT_IOP ? "iop" : "ee", allocator), allocator);
	AddAddress(out, "pc", stop.pc, allocator);
	AddAddress(out, "bp_addr", stop.bp_addr, allocator);
	AddAddress(out, "mem_addr", stop.mem_addr, allocator);
	out.AddMember("mem_size", stop.mem_size, allocator);
	out.AddMember("mem_write", stop.mem_write, allocator);
}

std::string DebugServerCommands::MakeStopEventLine(const DebuggerControl::StopEvent& stop)
{
	rapidjson::Document doc;
	doc.SetObject();
	auto& allocator = doc.GetAllocator();

	doc.AddMember("event", rapidjson::Value("stop", allocator), allocator);
	WriteStopEvent(stop, doc, allocator);

	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	doc.Accept(writer);
	return std::string(buffer.GetString(), buffer.GetSize());
}

void DebugServerCommands::RegisterAll()
{
	if (!s_handlers.empty())
		return;

	s_handlers["version"] = CmdVersion;
	s_handlers["status"] = CmdStatus;
	s_handlers["subscribe"] = CmdSubscribe;
}

const DebugServerHandler* DebugServerCommands::Find(const std::string& name)
{
	const auto it = s_handlers.find(name);
	return it == s_handlers.end() ? nullptr : &it->second;
}
