// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "DebugServer/DebugServerCommands.h"

#include "DebugServer/DebugServer.h"
#include "DebugServer/DebugServerDispatch.h"

#include "BuildVersion.h"
#include "Host.h"
#include "VMManager.h"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <string_view>
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

	// --- argument helpers ---------------------------------------------------------------

	const rapidjson::Value* Member(const DebugServerRequest& request, const char* name)
	{
		const auto it = request.args->FindMember(name);
		return it == request.args->MemberEnd() ? nullptr : &it->value;
	}

	BreakPointCpu ArgCpu(const DebugServerRequest& request)
	{
		const rapidjson::Value* value = Member(request, "cpu");
		if (value && value->IsString() && std::string_view(value->GetString()) == "iop")
			return BREAKPOINT_IOP;

		return BREAKPOINT_EE;
	}

	u32 ArgU32(const DebugServerRequest& request, const char* name, u32 fallback)
	{
		const rapidjson::Value* value = Member(request, name);
		if (!value)
			return fallback;
		if (value->IsUint())
			return value->GetUint();
		if (value->IsInt() && value->GetInt() >= 0)
			return static_cast<u32>(value->GetInt());

		return fallback;
	}

	u64 ArgU64(const DebugServerRequest& request, const char* name, u64 fallback)
	{
		const rapidjson::Value* value = Member(request, name);
		if (!value)
			return fallback;
		if (value->IsUint64())
			return value->GetUint64();
		if (value->IsInt64() && value->GetInt64() >= 0)
			return static_cast<u64>(value->GetInt64());

		return fallback;
	}

	// Accepts a JSON number, a literal string ("0x12BBD0", "12BBD0", "1227728"), or an
	// expression ("main+0x40", "[0x1B1F038]"). Expressions need the CPU thread, so they cost
	// a hop; literals do not.
	bool ResolveAddress(const DebugServerRequest& request, const char* name, BreakPointCpu cpu,
		u32& out, std::string& error)
	{
		const rapidjson::Value* value = Member(request, name);
		if (!value)
		{
			error = std::string("missing required argument \"") + name + "\"";
			return false;
		}

		if (value->IsUint())
		{
			out = value->GetUint();
			return true;
		}

		if (!value->IsString())
		{
			error = std::string("argument \"") + name + "\" must be a number or a string";
			return false;
		}

		const std::string text(value->GetString(), value->GetStringLength());
		if (DebugServerJson::ParseAddressLiteral(text, out))
			return true;

		u32 resolved = 0;
		bool ok = false;
		std::string parse_error;
		const bool dispatched = DebugServerDispatch::RunOnCPUThreadWithTimeout(
			[cpu, text, &resolved, &ok, &parse_error]() {
				u64 value64 = 0;
				if (DebugInterface::get(cpu).evaluateExpression(text.c_str(), value64, parse_error))
				{
					resolved = static_cast<u32>(value64);
					ok = true;
				}
			});

		if (!dispatched)
		{
			error = "the CPU thread did not respond while evaluating \"" + text + "\"";
			return false;
		}

		if (!ok)
		{
			error = "could not resolve \"" + text + "\": " + parse_error;
			return false;
		}

		out = resolved;
		return true;
	}

	std::string Error(const DebugServerRequest& request, const char* code, const std::string& message)
	{
		return DebugServerJson::MakeError(request.id, code, message);
	}

	// Every command that changes VM state needs these two guards, and getting either wrong
	// is the difference between a clear error and a crash.
	bool RequireVM(const DebugServerRequest& request, std::string& out)
	{
		if (DebugServerDispatch::VMIsValid())
			return true;

		out = Error(request, "no_vm", "no virtual machine is running");
		return false;
	}

	bool RequirePaused(const DebugServerRequest& request, std::string& out)
	{
		if (DebugServerDispatch::VMIsPaused())
			return true;

		out = Error(request, "not_paused", "this command requires a paused VM");
		return false;
	}

	std::string StopResult(const DebugServerRequest& request, const DebuggerControl::StopEvent& stop)
	{
		rapidjson::Document result;
		result.SetObject();
		DebugServerCommands::WriteStopEvent(stop, result, result.GetAllocator());
		return DebugServerJson::MakeResult(request.id, result, result.GetAllocator());
	}

	std::string SimpleResult(const DebugServerRequest& request)
	{
		rapidjson::Document result;
		result.SetObject();
		result.AddMember("vm_state", rapidjson::Value(VMStateName(), result.GetAllocator()), result.GetAllocator());
		return DebugServerJson::MakeResult(request.id, result, result.GetAllocator());
	}

	// --- execution ----------------------------------------------------------------------

	std::string CmdRun(const DebugServerRequest& request, DebugServerConnection&)
	{
		std::string failure;
		if (!RequireVM(request, failure))
			return failure;

		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout([]() { VMManager::SetPaused(false); }))
			return Error(request, "timeout", "the CPU thread did not respond");

		return SimpleResult(request);
	}

	std::string CmdPause(const DebugServerRequest& request, DebugServerConnection&)
	{
		std::string failure;
		if (!RequireVM(request, failure))
			return failure;

		// Read the sequence before acting: the stop can be recorded before the wait starts,
		// and without this the wait would miss it and time out.
		const u64 since = DebuggerControl::GetLastStop().seq;

		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout([]() { VMManager::SetPaused(true); }))
			return Error(request, "timeout", "the CPU thread did not respond");

		DebuggerControl::StopEvent stop;
		if (!DebuggerControl::WaitForStop(since, ArgU32(request, "timeout_ms", 2000), stop))
			return Error(request, "timeout", "the VM did not report a stop after pausing");

		return StopResult(request, stop);
	}

	std::string CmdStep(const DebugServerRequest& request, DebugServerConnection&)
	{
		std::string failure;
		if (!RequireVM(request, failure) || !RequirePaused(request, failure))
			return failure;

		const rapidjson::Value* mode_value = Member(request, "mode");
		const std::string_view mode_text = (mode_value && mode_value->IsString())
											   ? std::string_view(mode_value->GetString())
											   : std::string_view("into");

		DebuggerControl::StepMode mode;
		if (mode_text == "into")
			mode = DebuggerControl::StepMode::Into;
		else if (mode_text == "over")
			mode = DebuggerControl::StepMode::Over;
		else if (mode_text == "out")
			mode = DebuggerControl::StepMode::Out;
		else
			return Error(request, "bad_args", "mode must be one of into, over, out");

		const BreakPointCpu cpu = ArgCpu(request);
		const u64 since = DebuggerControl::GetLastStop().seq;

		bool started = false;
		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout(
				[cpu, mode, &started]() { started = DebuggerControl::Step(cpu, mode); }))
		{
			return Error(request, "timeout", "the CPU thread did not respond");
		}

		if (!started)
		{
			return Error(request, "unsupported",
				"could not step; for mode \"out\" there may be no caller frame to return to");
		}

		DebuggerControl::StopEvent stop;
		if (!DebuggerControl::WaitForStop(since, ArgU32(request, "timeout_ms", 5000), stop))
			return Error(request, "timeout", "the step did not complete");

		return StopResult(request, stop);
	}

	std::string CmdRunTo(const DebugServerRequest& request, DebugServerConnection&)
	{
		std::string failure;
		if (!RequireVM(request, failure) || !RequirePaused(request, failure))
			return failure;

		const BreakPointCpu cpu = ArgCpu(request);

		u32 addr = 0;
		std::string error;
		if (!ResolveAddress(request, "addr", cpu, addr, error))
			return Error(request, "bad_address", error);

		bool started = false;
		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout(
				[cpu, addr, &started]() { started = DebuggerControl::RunTo(cpu, addr); }))
		{
			return Error(request, "timeout", "the CPU thread did not respond");
		}

		if (!started)
			return Error(request, "unsupported", "could not resume from the current state");

		// Deliberately does not wait: the target may never be reached, so the caller decides
		// how long to give it by calling wait with its own timeout.
		rapidjson::Document result;
		result.SetObject();
		AddAddress(result, "addr", addr, result.GetAllocator());
		return DebugServerJson::MakeResult(request.id, result, result.GetAllocator());
	}

	std::string CmdFrameAdvance(const DebugServerRequest& request, DebugServerConnection&)
	{
		std::string failure;
		if (!RequireVM(request, failure))
			return failure;

		const u32 count = std::clamp(ArgU32(request, "count", 1), 1u, 600u);
		const u64 since = DebuggerControl::GetLastStop().seq;

		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout([count]() { VMManager::FrameAdvance(count); }))
			return Error(request, "timeout", "the CPU thread did not respond");

		DebuggerControl::StopEvent stop;
		DebuggerControl::WaitForStop(since, ArgU32(request, "timeout_ms", 10000), stop);

		rapidjson::Document result;
		result.SetObject();
		auto& allocator = result.GetAllocator();
		result.AddMember("frames", count, allocator);

		rapidjson::Value stop_value(rapidjson::kObjectType);
		DebugServerCommands::WriteStopEvent(stop, stop_value, allocator);
		result.AddMember("stop", stop_value, allocator);

		return DebugServerJson::MakeResult(request.id, result, allocator);
	}

	std::string CmdReset(const DebugServerRequest& request, DebugServerConnection&)
	{
		std::string failure;
		if (!RequireVM(request, failure))
			return failure;

		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout([]() { VMManager::Reset(); }, 10000))
			return Error(request, "timeout", "the CPU thread did not respond");

		return SimpleResult(request);
	}

	std::string CmdShutdown(const DebugServerRequest& request, DebugServerConnection&)
	{
		std::string failure;
		if (!RequireVM(request, failure))
			return failure;

		// Queued without waiting for completion: shutting the VM down tears down the very
		// thread we would be waiting on.
		Host::RunOnCPUThread([]() { VMManager::SetState(VMState::Stopping); });

		rapidjson::Document result;
		result.SetObject();
		result.AddMember("stopping", true, result.GetAllocator());
		return DebugServerJson::MakeResult(request.id, result, result.GetAllocator());
	}

	std::string CmdWait(const DebugServerRequest& request, DebugServerConnection&)
	{
		const u64 since = ArgU64(request, "since", 0);
		const u32 timeout_ms = std::min(ArgU32(request, "timeout_ms", 60000), 86400000u);

		DebuggerControl::StopEvent stop;
		if (!DebuggerControl::WaitForStop(since, timeout_ms, stop))
			return Error(request, "timeout", "no stop occurred within the timeout");

		return StopResult(request, stop);
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

	s_handlers["run"] = CmdRun;
	s_handlers["pause"] = CmdPause;
	s_handlers["step"] = CmdStep;
	s_handlers["run-to"] = CmdRunTo;
	s_handlers["frame-advance"] = CmdFrameAdvance;
	s_handlers["reset"] = CmdReset;
	s_handlers["shutdown"] = CmdShutdown;
	s_handlers["wait"] = CmdWait;
}

const DebugServerHandler* DebugServerCommands::Find(const std::string& name)
{
	const auto it = s_handlers.find(name);
	return it == s_handlers.end() ? nullptr : &it->second;
}
