// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "DebugServer/DebugServerCommands.h"

#include "DebugServer/DebugServer.h"
#include "DebugServer/DebugServerDispatch.h"
#include "DebugServer/MemorySearch.h"

#include "DebugTools/Breakpoints.h"

#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadTypes.h"
#include "DebugTools/MipsAssembler.h"
#include "DebugTools/MipsStackWalk.h"

#include "CDVD/CDVDcommon.h"
#include "GS/GS.h"

#include "common/Error.h"

#include "BuildVersion.h"
#include "Host.h"
#include "VMManager.h"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <cstdio>
#include <string_view>
#include <utility>
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

	// Named Fail rather than Error so it does not shadow the Error type from common/Error.h.
	std::string Fail(const DebugServerRequest& request, const char* code, const std::string& message)
	{
		return DebugServerJson::MakeError(request.id, code, message);
	}

	// Every command that changes VM state needs these two guards, and getting either wrong
	// is the difference between a clear error and a crash.
	bool RequireVM(const DebugServerRequest& request, std::string& out)
	{
		if (DebugServerDispatch::VMIsValid())
			return true;

		out = Fail(request, "no_vm", "no virtual machine is running");
		return false;
	}

	bool RequirePaused(const DebugServerRequest& request, std::string& out)
	{
		if (DebugServerDispatch::VMIsPaused())
			return true;

		out = Fail(request, "not_paused", "this command requires a paused VM");
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
			return Fail(request, "timeout", "the CPU thread did not respond");

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
			return Fail(request, "timeout", "the CPU thread did not respond");

		DebuggerControl::StopEvent stop;
		if (!DebuggerControl::WaitForStop(since, ArgU32(request, "timeout_ms", 2000), stop))
			return Fail(request, "timeout", "the VM did not report a stop after pausing");

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
			return Fail(request, "bad_args", "mode must be one of into, over, out");

		const BreakPointCpu cpu = ArgCpu(request);
		const u64 since = DebuggerControl::GetLastStop().seq;

		bool started = false;
		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout(
				[cpu, mode, &started]() { started = DebuggerControl::Step(cpu, mode); }))
		{
			return Fail(request, "timeout", "the CPU thread did not respond");
		}

		if (!started)
		{
			return Fail(request, "unsupported",
				"could not step; for mode \"out\" there may be no caller frame to return to");
		}

		DebuggerControl::StopEvent stop;
		if (!DebuggerControl::WaitForStop(since, ArgU32(request, "timeout_ms", 5000), stop))
			return Fail(request, "timeout", "the step did not complete");

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
			return Fail(request, "bad_address", error);

		bool started = false;
		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout(
				[cpu, addr, &started]() { started = DebuggerControl::RunTo(cpu, addr); }))
		{
			return Fail(request, "timeout", "the CPU thread did not respond");
		}

		if (!started)
			return Fail(request, "unsupported", "could not resume from the current state");

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
			return Fail(request, "timeout", "the CPU thread did not respond");

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
			return Fail(request, "timeout", "the CPU thread did not respond");

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
			return Fail(request, "timeout", "no stop occurred within the timeout");

		return StopResult(request, stop);
	}

	// --- breakpoints --------------------------------------------------------------------

	std::string ArgString(const DebugServerRequest& request, const char* name)
	{
		const rapidjson::Value* value = Member(request, name);
		if (!value || !value->IsString())
			return {};

		return std::string(value->GetString(), value->GetStringLength());
	}

	bool ArgBool(const DebugServerRequest& request, const char* name, bool fallback)
	{
		const rapidjson::Value* value = Member(request, name);
		return (value && value->IsBool()) ? value->GetBool() : fallback;
	}

	// Builds a condition on the CPU thread; the expression parser reads live CPU state.
	// A condition that fails to parse must abort the whole command rather than silently
	// leaving an unconditional breakpoint behind.
	bool MakeCondition(BreakPointCpu cpu, const std::string& text, BreakPointCond& out, std::string& error)
	{
		out.debug = &DebugInterface::get(cpu);
		out.expressionString = text;
		return out.debug->initExpression(text.c_str(), out.expression, error);
	}

	std::string CmdBpAdd(const DebugServerRequest& request, DebugServerConnection&)
	{
		std::string failure;
		if (!RequireVM(request, failure))
			return failure;

		const BreakPointCpu cpu = ArgCpu(request);

		u32 addr = 0;
		std::string error;
		if (!ResolveAddress(request, "addr", cpu, addr, error))
			return Fail(request, "bad_address", error);

		const bool enabled = ArgBool(request, "enabled", true);
		const bool temporary = ArgBool(request, "temporary", false);
		const std::string condition = ArgString(request, "condition");
		const std::string description = ArgString(request, "description");

		bool condition_ok = true;
		std::string condition_error;

		const bool dispatched = DebugServerDispatch::RunOnCPUThreadWithTimeout(
			[cpu, addr, enabled, temporary, condition, description, &condition_ok, &condition_error]() {
				BreakPointCond cond;
				if (!condition.empty() && !MakeCondition(cpu, condition, cond, condition_error))
				{
					condition_ok = false;
					return;
				}

				CBreakPoints::AddBreakPoint(cpu, addr, temporary, enabled, false);

				if (!condition.empty())
					CBreakPoints::ChangeBreakPointAddCond(cpu, addr, cond);

				if (!description.empty())
					CBreakPoints::ChangeBreakPointDescription(cpu, addr, description);
			});

		if (!dispatched)
			return Fail(request, "timeout", "the CPU thread did not respond");

		if (!condition_ok)
			return Fail(request, "bad_args", "condition did not parse: " + condition_error);

		rapidjson::Document result;
		result.SetObject();
		AddAddress(result, "addr", addr, result.GetAllocator());
		return DebugServerJson::MakeResult(request.id, result, result.GetAllocator());
	}

	std::string CmdBpRemove(const DebugServerRequest& request, DebugServerConnection&)
	{
		std::string failure;
		if (!RequireVM(request, failure))
			return failure;

		const BreakPointCpu cpu = ArgCpu(request);

		u32 addr = 0;
		std::string error;
		if (!ResolveAddress(request, "addr", cpu, addr, error))
			return Fail(request, "bad_address", error);

		bool removed = false;
		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout([cpu, addr, &removed]() {
				removed = CBreakPoints::IsAddressBreakPoint(cpu, addr);
				if (removed)
					CBreakPoints::RemoveBreakPoint(cpu, addr);
			}))
		{
			return Fail(request, "timeout", "the CPU thread did not respond");
		}

		// Removing an address with no breakpoint is not an error: a cleanup script should be
		// able to run twice without failing the second time.
		rapidjson::Document result;
		result.SetObject();
		AddAddress(result, "addr", addr, result.GetAllocator());
		result.AddMember("removed", removed, result.GetAllocator());
		return DebugServerJson::MakeResult(request.id, result, result.GetAllocator());
	}

	std::string CmdBpList(const DebugServerRequest& request, DebugServerConnection&)
	{
		std::string failure;
		if (!RequireVM(request, failure))
			return failure;

		const BreakPointCpu cpu = ArgCpu(request);
		const bool include_temp = ArgBool(request, "include_temp", false);

		std::vector<BreakPoint> breakpoints;
		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout([cpu, include_temp, &breakpoints]() {
				breakpoints = CBreakPoints::GetBreakpoints(cpu, include_temp);
			}))
		{
			return Fail(request, "timeout", "the CPU thread did not respond");
		}

		rapidjson::Document result;
		result.SetObject();
		auto& allocator = result.GetAllocator();

		rapidjson::Value list(rapidjson::kArrayType);
		for (const BreakPoint& bp : breakpoints)
		{
			rapidjson::Value entry(rapidjson::kObjectType);
			AddAddress(entry, "addr", bp.addr, allocator);
			entry.AddMember("cpu", rapidjson::Value(bp.cpu == BREAKPOINT_IOP ? "iop" : "ee", allocator), allocator);
			entry.AddMember("enabled", bp.enabled, allocator);
			entry.AddMember("temporary", bp.temporary, allocator);
			entry.AddMember("stepping", bp.stepping, allocator);
			entry.AddMember("condition", Str(bp.hasCond ? bp.cond.expressionString : std::string(), allocator),
				allocator);
			entry.AddMember("description", Str(bp.description, allocator), allocator);
			list.PushBack(entry, allocator);
		}

		result.AddMember("breakpoints", list, allocator);
		return DebugServerJson::MakeResult(request.id, result, allocator);
	}

	std::string ChangeBreakpointEnabled(const DebugServerRequest& request, bool enable)
	{
		std::string failure;
		if (!RequireVM(request, failure))
			return failure;

		const BreakPointCpu cpu = ArgCpu(request);

		u32 addr = 0;
		std::string error;
		if (!ResolveAddress(request, "addr", cpu, addr, error))
			return Fail(request, "bad_address", error);

		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout(
				[cpu, addr, enable]() { CBreakPoints::ChangeBreakPoint(cpu, addr, enable); }))
		{
			return Fail(request, "timeout", "the CPU thread did not respond");
		}

		rapidjson::Document result;
		result.SetObject();
		AddAddress(result, "addr", addr, result.GetAllocator());
		result.AddMember("enabled", enable, result.GetAllocator());
		return DebugServerJson::MakeResult(request.id, result, result.GetAllocator());
	}

	std::string CmdBpEnable(const DebugServerRequest& request, DebugServerConnection&)
	{
		return ChangeBreakpointEnabled(request, true);
	}

	std::string CmdBpDisable(const DebugServerRequest& request, DebugServerConnection&)
	{
		return ChangeBreakpointEnabled(request, false);
	}

	std::string CmdBpClear(const DebugServerRequest& request, DebugServerConnection&)
	{
		std::string failure;
		if (!RequireVM(request, failure))
			return failure;

		const BreakPointCpu cpu = ArgCpu(request);

		size_t removed = 0;
		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout([cpu, &removed]() {
				// Not ClearAllBreakPoints: that would also drop the other CPU's breakpoints,
				// which the caller did not ask for.
				const std::vector<BreakPoint> existing = CBreakPoints::GetBreakpoints(cpu, false);
				for (const BreakPoint& bp : existing)
					CBreakPoints::RemoveBreakPoint(cpu, bp.addr);

				removed = existing.size();
			}))
		{
			return Fail(request, "timeout", "the CPU thread did not respond");
		}

		rapidjson::Document result;
		result.SetObject();
		result.AddMember("removed", static_cast<u64>(removed), result.GetAllocator());
		return DebugServerJson::MakeResult(request.id, result, result.GetAllocator());
	}

	// --- memchecks ----------------------------------------------------------------------

	bool ParseMemCheckCondition(const DebugServerRequest& request, MemCheckCondition& out, std::string& error)
	{
		const rapidjson::Value* value = Member(request, "on");
		if (!value || !value->IsArray() || value->Empty())
		{
			error = "\"on\" must be a non-empty array of read, write or change";
			return false;
		}

		int bits = 0;
		for (const rapidjson::Value& item : value->GetArray())
		{
			if (!item.IsString())
			{
				error = "\"on\" entries must be strings";
				return false;
			}

			const std::string_view text(item.GetString());
			if (text == "read")
				bits |= MEMCHECK_READ;
			else if (text == "write")
				bits |= MEMCHECK_WRITE;
			else if (text == "change")
				bits |= MEMCHECK_WRITE_ONCHANGE;
			else
			{
				error = "unknown \"on\" value: " + std::string(text);
				return false;
			}
		}

		out = static_cast<MemCheckCondition>(bits);
		return true;
	}

	MemCheckResult ParseMemCheckResult(const DebugServerRequest& request)
	{
		const rapidjson::Value* value = Member(request, "result");
		if (!value || !value->IsArray() || value->Empty())
			return MEMCHECK_BREAK;

		int bits = 0;
		for (const rapidjson::Value& item : value->GetArray())
		{
			if (!item.IsString())
				continue;

			const std::string_view text(item.GetString());
			if (text == "break")
				bits |= MEMCHECK_BREAK;
			else if (text == "log")
				bits |= MEMCHECK_LOG;
		}

		return bits == 0 ? MEMCHECK_BREAK : static_cast<MemCheckResult>(bits);
	}

	std::string CmdMcAdd(const DebugServerRequest& request, DebugServerConnection&)
	{
		std::string failure;
		if (!RequireVM(request, failure))
			return failure;

		const BreakPointCpu cpu = ArgCpu(request);

		u32 start = 0;
		u32 end = 0;
		std::string error;
		if (!ResolveAddress(request, "start", cpu, start, error))
			return Fail(request, "bad_address", error);
		if (!ResolveAddress(request, "end", cpu, end, error))
			return Fail(request, "bad_address", error);

		if (end <= start)
			return Fail(request, "bad_args", "end must be greater than start");

		MemCheckCondition condition;
		if (!ParseMemCheckCondition(request, condition, error))
			return Fail(request, "bad_args", error);

		const MemCheckResult result_flags = ParseMemCheckResult(request);
		const std::string cond_text = ArgString(request, "condition");
		const std::string description = ArgString(request, "description");

		bool condition_ok = true;
		std::string condition_error;

		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout([cpu, start, end, condition, result_flags, cond_text,
															   description, &condition_ok, &condition_error]() {
				BreakPointCond cond;
				if (!cond_text.empty() && !MakeCondition(cpu, cond_text, cond, condition_error))
				{
					condition_ok = false;
					return;
				}

				CBreakPoints::AddMemCheck(cpu, start, end, condition, result_flags);

				if (!cond_text.empty())
					CBreakPoints::ChangeMemCheckAddCond(cpu, start, end, cond);

				if (!description.empty())
					CBreakPoints::ChangeMemCheckDescription(cpu, start, end, description);
			}))
		{
			return Fail(request, "timeout", "the CPU thread did not respond");
		}

		if (!condition_ok)
			return Fail(request, "bad_args", "condition did not parse: " + condition_error);

		rapidjson::Document result;
		result.SetObject();
		auto& allocator = result.GetAllocator();
		AddAddress(result, "start", start, allocator);
		AddAddress(result, "end", end, allocator);

		if ((condition & (MEMCHECK_WRITE | MEMCHECK_WRITE_ONCHANGE)) != 0)
		{
			// A silent no-hit is indistinguishable from a wrong address, and this trips
			// people up for hours. Breakpoints.h says memchecks are not used by the
			// interpreter or HLE, and writes that bypass cached EE stores never reach them.
			result.AddMember("note",
				rapidjson::Value("write memchecks are not observed by every write path; "
								 "see docs/pcsxroo/cli.md",
					allocator),
				allocator);
		}

		return DebugServerJson::MakeResult(request.id, result, allocator);
	}

	std::string CmdMcRemove(const DebugServerRequest& request, DebugServerConnection&)
	{
		std::string failure;
		if (!RequireVM(request, failure))
			return failure;

		const BreakPointCpu cpu = ArgCpu(request);

		u32 start = 0;
		u32 end = 0;
		std::string error;
		if (!ResolveAddress(request, "start", cpu, start, error))
			return Fail(request, "bad_address", error);
		if (!ResolveAddress(request, "end", cpu, end, error))
			return Fail(request, "bad_address", error);

		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout(
				[cpu, start, end]() { CBreakPoints::RemoveMemCheck(cpu, start, end); }))
		{
			return Fail(request, "timeout", "the CPU thread did not respond");
		}

		rapidjson::Document result;
		result.SetObject();
		AddAddress(result, "start", start, result.GetAllocator());
		AddAddress(result, "end", end, result.GetAllocator());
		return DebugServerJson::MakeResult(request.id, result, result.GetAllocator());
	}

	std::string CmdMcList(const DebugServerRequest& request, DebugServerConnection&)
	{
		std::string failure;
		if (!RequireVM(request, failure))
			return failure;

		const BreakPointCpu cpu = ArgCpu(request);

		std::vector<MemCheck> checks;
		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout(
				[cpu, &checks]() { checks = CBreakPoints::GetMemChecks(cpu); }))
		{
			return Fail(request, "timeout", "the CPU thread did not respond");
		}

		rapidjson::Document result;
		result.SetObject();
		auto& allocator = result.GetAllocator();

		rapidjson::Value list(rapidjson::kArrayType);
		for (const MemCheck& check : checks)
		{
			rapidjson::Value entry(rapidjson::kObjectType);
			AddAddress(entry, "start", check.start, allocator);
			AddAddress(entry, "end", check.end, allocator);
			entry.AddMember("cpu", rapidjson::Value(check.cpu == BREAKPOINT_IOP ? "iop" : "ee", allocator), allocator);

			rapidjson::Value on(rapidjson::kArrayType);
			if (check.memCond & MEMCHECK_READ)
				on.PushBack(rapidjson::Value("read", allocator), allocator);
			if (check.memCond & MEMCHECK_WRITE)
				on.PushBack(rapidjson::Value("write", allocator), allocator);
			if (check.memCond & MEMCHECK_WRITE_ONCHANGE)
				on.PushBack(rapidjson::Value("change", allocator), allocator);
			entry.AddMember("on", on, allocator);

			rapidjson::Value results(rapidjson::kArrayType);
			if (check.result & MEMCHECK_BREAK)
				results.PushBack(rapidjson::Value("break", allocator), allocator);
			if (check.result & MEMCHECK_LOG)
				results.PushBack(rapidjson::Value("log", allocator), allocator);
			entry.AddMember("result", results, allocator);

			entry.AddMember("condition",
				Str(check.hasCond ? check.cond.expressionString : std::string(), allocator), allocator);
			entry.AddMember("description", Str(check.description, allocator), allocator);
			entry.AddMember("num_hits", check.numHits, allocator);
			AddAddress(entry, "last_pc", check.lastPC, allocator);
			AddAddress(entry, "last_addr", check.lastAddr, allocator);
			entry.AddMember("last_size", check.lastSize, allocator);

			list.PushBack(entry, allocator);
		}

		result.AddMember("memchecks", list, allocator);
		return DebugServerJson::MakeResult(request.id, result, allocator);
	}

	std::string CmdMcClear(const DebugServerRequest& request, DebugServerConnection&)
	{
		std::string failure;
		if (!RequireVM(request, failure))
			return failure;

		const BreakPointCpu cpu = ArgCpu(request);

		size_t removed = 0;
		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout([cpu, &removed]() {
				const std::vector<MemCheck> existing = CBreakPoints::GetMemChecks(cpu);
				for (const MemCheck& check : existing)
					CBreakPoints::RemoveMemCheck(cpu, check.start, check.end);

				removed = existing.size();
			}))
		{
			return Fail(request, "timeout", "the CPU thread did not respond");
		}

		rapidjson::Document result;
		result.SetObject();
		result.AddMember("removed", static_cast<u64>(removed), result.GetAllocator());
		return DebugServerJson::MakeResult(request.id, result, result.GetAllocator());
	}

	// --- registers ----------------------------------------------------------------------

	std::string ToLower(std::string_view text)
	{
		std::string out(text);
		std::transform(out.begin(), out.end(), out.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return out;
	}

	std::string U128ToHex(const u128& value)
	{
		return fmt::format("{:016x}{:016x}", value.hi, value.lo);
	}

	// Pseudo-registers that are not in any category but are what a caller usually wants.
	constexpr int PSEUDO_PC = -1;
	constexpr int PSEUDO_HI = -2;
	constexpr int PSEUDO_LO = -3;

	// Accepts "a0", "$a0", "gpr:a0", "cp0:Status", and the pseudo-registers pc, hi and lo.
	// Matching is case insensitive. Must run on the CPU thread: the register tables come
	// from the live DebugInterface.
	bool ResolveRegister(DebugInterface& cpu, std::string_view name, int& category, int& index)
	{
		std::string wanted = ToLower(name);
		if (!wanted.empty() && wanted.front() == '$')
			wanted.erase(0, 1);

		if (wanted == "pc")
		{
			category = PSEUDO_PC;
			index = 0;
			return true;
		}
		if (wanted == "hi")
		{
			category = PSEUDO_HI;
			index = 0;
			return true;
		}
		if (wanted == "lo")
		{
			category = PSEUDO_LO;
			index = 0;
			return true;
		}

		std::string wanted_category;
		const size_t colon = wanted.find(':');
		if (colon != std::string::npos)
		{
			wanted_category = wanted.substr(0, colon);
			wanted = wanted.substr(colon + 1);
		}

		for (int cat = 0; cat < cpu.getRegisterCategoryCount(); cat++)
		{
			if (!wanted_category.empty() && ToLower(cpu.getRegisterCategoryName(cat)) != wanted_category)
				continue;

			for (int num = 0; num < cpu.getRegisterCount(cat); num++)
			{
				if (ToLower(cpu.getRegisterName(cat, num)) != wanted)
					continue;

				category = cat;
				index = num;
				return true;
			}
		}

		return false;
	}

	u128 ReadRegister(DebugInterface& cpu, int category, int index)
	{
		switch (category)
		{
			case PSEUDO_PC:
				return u128::From32(cpu.getPC());
			case PSEUDO_HI:
				return cpu.getHI();
			case PSEUDO_LO:
				return cpu.getLO();
			default:
				return cpu.getRegister(category, index);
		}
	}

	std::string CmdRegList(const DebugServerRequest& request, DebugServerConnection&)
	{
		std::string failure;
		if (!RequireVM(request, failure))
			return failure;

		const BreakPointCpu cpu_type = ArgCpu(request);

		// Names are gathered into plain strings on the CPU thread; the pointers the debug
		// interface hands back are only guaranteed valid there.
		std::vector<std::pair<std::string, std::vector<std::string>>> categories;
		std::vector<int> sizes;

		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout([cpu_type, &categories, &sizes]() {
				DebugInterface& cpu = DebugInterface::get(cpu_type);
				for (int cat = 0; cat < cpu.getRegisterCategoryCount(); cat++)
				{
					std::vector<std::string> names;
					for (int num = 0; num < cpu.getRegisterCount(cat); num++)
						names.emplace_back(cpu.getRegisterName(cat, num));

					categories.emplace_back(cpu.getRegisterCategoryName(cat), std::move(names));
					sizes.push_back(cpu.getRegisterSize(cat));
				}
			}))
		{
			return Fail(request, "timeout", "the CPU thread did not respond");
		}

		rapidjson::Document result;
		result.SetObject();
		auto& allocator = result.GetAllocator();

		rapidjson::Value list(rapidjson::kArrayType);
		for (size_t i = 0; i < categories.size(); i++)
		{
			rapidjson::Value entry(rapidjson::kObjectType);
			entry.AddMember("name", Str(categories[i].first, allocator), allocator);
			entry.AddMember("size", sizes[i], allocator);

			rapidjson::Value names(rapidjson::kArrayType);
			for (const std::string& name : categories[i].second)
				names.PushBack(Str(name, allocator), allocator);

			entry.AddMember("registers", names, allocator);
			list.PushBack(entry, allocator);
		}

		result.AddMember("categories", list, allocator);
		return DebugServerJson::MakeResult(request.id, result, allocator);
	}

	std::string CmdRegGet(const DebugServerRequest& request, DebugServerConnection&)
	{
		std::string failure;
		if (!RequireVM(request, failure) || !RequirePaused(request, failure))
			return failure;

		const BreakPointCpu cpu_type = ArgCpu(request);
		const std::string name = ArgString(request, "name");
		if (name.empty())
			return Fail(request, "bad_args", "name is required");

		bool found = false;
		u128 value{};
		std::string text;

		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout([cpu_type, name, &found, &value, &text]() {
				DebugInterface& cpu = DebugInterface::get(cpu_type);
				int category = 0;
				int index = 0;
				if (!ResolveRegister(cpu, name, category, index))
					return;

				found = true;
				value = ReadRegister(cpu, category, index);
				text = (category >= 0) ? cpu.getRegisterString(category, index) : U128ToHex(value);
			}))
		{
			return Fail(request, "timeout", "the CPU thread did not respond");
		}

		if (!found)
			return Fail(request, "bad_args", "no such register: " + name);

		rapidjson::Document result;
		result.SetObject();
		auto& allocator = result.GetAllocator();
		result.AddMember("name", Str(name, allocator), allocator);
		result.AddMember("value", Str(U128ToHex(value), allocator), allocator);
		result.AddMember("value_u64", value.lo, allocator);
		result.AddMember("string", Str(text, allocator), allocator);
		return DebugServerJson::MakeResult(request.id, result, allocator);
	}

	std::string CmdRegSet(const DebugServerRequest& request, DebugServerConnection&)
	{
		std::string failure;
		if (!RequireVM(request, failure) || !RequirePaused(request, failure))
			return failure;

		const BreakPointCpu cpu_type = ArgCpu(request);
		const std::string name = ArgString(request, "name");
		if (name.empty())
			return Fail(request, "bad_args", "name is required");

		const rapidjson::Value* raw = Member(request, "value");
		if (!raw)
			return Fail(request, "bad_args", "value is required");

		u128 wanted{};
		if (raw->IsUint64())
		{
			wanted = u128::From64(raw->GetUint64());
		}
		else if (raw->IsString())
		{
			std::string text(raw->GetString(), raw->GetStringLength());
			if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
				text.erase(0, 2);

			if (text.empty() || text.size() > 32 ||
				!std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isxdigit(c) != 0; }))
			{
				return Fail(request, "bad_args", "value must be up to 32 hex digits or a number");
			}

			text.insert(text.begin(), 32 - text.size(), '0');
			wanted.hi = std::stoull(text.substr(0, 16), nullptr, 16);
			wanted.lo = std::stoull(text.substr(16), nullptr, 16);
		}
		else
		{
			return Fail(request, "bad_args", "value must be a number or a hex string");
		}

		bool found = false;
		u128 before{};
		u128 after{};

		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout(
				[cpu_type, name, wanted, &found, &before, &after]() {
					DebugInterface& cpu = DebugInterface::get(cpu_type);
					int category = 0;
					int index = 0;
					if (!ResolveRegister(cpu, name, category, index))
						return;

					found = true;
					before = ReadRegister(cpu, category, index);

					if (category == PSEUDO_PC)
						cpu.setPc(static_cast<u32>(wanted.lo));
					else if (category >= 0)
						cpu.setRegister(category, index, wanted);

					after = ReadRegister(cpu, category, index);
				}))
		{
			return Fail(request, "timeout", "the CPU thread did not respond");
		}

		if (!found)
			return Fail(request, "bad_args", "no such register: " + name);

		rapidjson::Document result;
		result.SetObject();
		auto& allocator = result.GetAllocator();
		result.AddMember("name", Str(name, allocator), allocator);
		result.AddMember("before", Str(U128ToHex(before), allocator), allocator);
		result.AddMember("after", Str(U128ToHex(after), allocator), allocator);
		return DebugServerJson::MakeResult(request.id, result, allocator);
	}

	std::string CmdRegDump(const DebugServerRequest& request, DebugServerConnection&)
	{
		std::string failure;
		if (!RequireVM(request, failure) || !RequirePaused(request, failure))
			return failure;

		const BreakPointCpu cpu_type = ArgCpu(request);
		const std::string wanted_category = ToLower(ArgString(request, "category"));

		struct Entry
		{
			std::string category;
			std::string name;
			u128 value;
			std::string text;
		};

		std::vector<Entry> entries;
		u32 pc = 0;

		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout([cpu_type, wanted_category, &entries, &pc]() {
				DebugInterface& cpu = DebugInterface::get(cpu_type);
				pc = cpu.getPC();

				for (int cat = 0; cat < cpu.getRegisterCategoryCount(); cat++)
				{
					const std::string category_name = cpu.getRegisterCategoryName(cat);
					if (!wanted_category.empty() && ToLower(category_name) != wanted_category)
						continue;

					for (int num = 0; num < cpu.getRegisterCount(cat); num++)
					{
						entries.push_back({category_name, cpu.getRegisterName(cat, num),
							cpu.getRegister(cat, num), cpu.getRegisterString(cat, num)});
					}
				}
			}))
		{
			return Fail(request, "timeout", "the CPU thread did not respond");
		}

		if (entries.empty() && !wanted_category.empty())
			return Fail(request, "bad_args", "no such register category");

		rapidjson::Document result;
		result.SetObject();
		auto& allocator = result.GetAllocator();
		AddAddress(result, "pc", pc, allocator);

		rapidjson::Value list(rapidjson::kArrayType);
		for (const Entry& entry : entries)
		{
			rapidjson::Value item(rapidjson::kObjectType);
			item.AddMember("category", Str(entry.category, allocator), allocator);
			item.AddMember("name", Str(entry.name, allocator), allocator);
			item.AddMember("value", Str(U128ToHex(entry.value), allocator), allocator);
			item.AddMember("value_u64", entry.value.lo, allocator);
			item.AddMember("string", Str(entry.text, allocator), allocator);
			list.PushBack(item, allocator);
		}

		result.AddMember("registers", list, allocator);
		return DebugServerJson::MakeResult(request.id, result, allocator);
	}

	// --- memory -------------------------------------------------------------------------

	constexpr u32 MAX_MEMORY_TRANSFER = 16 * 1024 * 1024;

	bool WantsBase64(const DebugServerRequest& request)
	{
		return ArgString(request, "format") == "base64";
	}

	std::string EncodeBytes(const DebugServerRequest& request, const std::vector<u8>& bytes)
	{
		return WantsBase64(request) ? DebugServerJson::BytesToBase64(bytes.data(), bytes.size())
									: DebugServerJson::BytesToHex(bytes.data(), bytes.size());
	}

	std::string CmdMemRead(const DebugServerRequest& request, DebugServerConnection&)
	{
		std::string failure;
		if (!RequireVM(request, failure))
			return failure;

		const BreakPointCpu cpu_type = ArgCpu(request);

		u32 addr = 0;
		std::string error;
		if (!ResolveAddress(request, "addr", cpu_type, addr, error))
			return Fail(request, "bad_address", error);

		const u32 size = ArgU32(request, "size", 4);
		if (size == 0 || size > MAX_MEMORY_TRANSFER)
			return Fail(request, "bad_args", "size must be between 1 and 16 MiB");

		std::vector<u8> bytes(size);
		bool ok = false;

		// Reads are allowed while running, matching what PINE already permits and what the
		// existing tooling relies on for sampling live counters.
		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout([cpu_type, addr, size, &bytes, &ok]() {
				ok = DebugInterface::get(cpu_type).ReadBytes(addr, bytes.data(), size);
			}))
		{
			return Fail(request, "timeout", "the CPU thread did not respond");
		}

		if (!ok)
			return Fail(request, "bad_address", "could not read " + std::to_string(size) + " bytes there");

		rapidjson::Document result;
		result.SetObject();
		auto& allocator = result.GetAllocator();
		AddAddress(result, "addr", addr, allocator);
		result.AddMember("size", size, allocator);
		result.AddMember("format", Str(WantsBase64(request) ? "base64" : "hex", allocator), allocator);
		result.AddMember("data", Str(EncodeBytes(request, bytes), allocator), allocator);
		return DebugServerJson::MakeResult(request.id, result, allocator);
	}

	std::string CmdMemWrite(const DebugServerRequest& request, DebugServerConnection&)
	{
		std::string failure;
		if (!RequireVM(request, failure))
			return failure;

		const BreakPointCpu cpu_type = ArgCpu(request);

		u32 addr = 0;
		std::string error;
		if (!ResolveAddress(request, "addr", cpu_type, addr, error))
			return Fail(request, "bad_address", error);

		const std::string data = ArgString(request, "data");
		if (data.empty())
			return Fail(request, "bad_args", "data is required");

		std::vector<u8> bytes;
		const bool decoded = WantsBase64(request) ? DebugServerJson::Base64ToBytes(data, bytes)
												  : DebugServerJson::HexToBytes(data, bytes);
		if (!decoded || bytes.empty())
			return Fail(request, "bad_args", "data is not valid " + std::string(WantsBase64(request) ? "base64" : "hex"));

		if (bytes.size() > MAX_MEMORY_TRANSFER)
			return Fail(request, "bad_args", "data exceeds 16 MiB");

		std::vector<u8> before(bytes.size());
		std::vector<u8> after(bytes.size());
		bool ok = false;

		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout([cpu_type, addr, &bytes, &before, &after, &ok]() {
				MemoryInterface& memory = DebugInterface::get(cpu_type);
				const u32 size = static_cast<u32>(bytes.size());

				if (!memory.ReadBytes(addr, before.data(), size))
					return;
				if (!memory.WriteBytes(addr, bytes.data(), size))
					return;

				ok = memory.ReadBytes(addr, after.data(), size);
			}))
		{
			return Fail(request, "timeout", "the CPU thread did not respond");
		}

		if (!ok)
			return Fail(request, "bad_address", "could not write there");

		rapidjson::Document result;
		result.SetObject();
		auto& allocator = result.GetAllocator();
		AddAddress(result, "addr", addr, allocator);
		result.AddMember("size", static_cast<u32>(bytes.size()), allocator);
		result.AddMember("before", Str(EncodeBytes(request, before), allocator), allocator);
		result.AddMember("after", Str(EncodeBytes(request, after), allocator), allocator);
		// A value the game rewrites every frame is otherwise indistinguishable from a
		// successful write, which is a trap the existing tooling already warns about.
		result.AddMember("verified", after == bytes, allocator);
		return DebugServerJson::MakeResult(request.id, result, allocator);
	}

	std::string CmdMemFill(const DebugServerRequest& request, DebugServerConnection&)
	{
		std::string failure;
		if (!RequireVM(request, failure))
			return failure;

		const BreakPointCpu cpu_type = ArgCpu(request);

		u32 addr = 0;
		std::string error;
		if (!ResolveAddress(request, "addr", cpu_type, addr, error))
			return Fail(request, "bad_address", error);

		const u32 size = ArgU32(request, "size", 0);
		if (size == 0 || size > MAX_MEMORY_TRANSFER)
			return Fail(request, "bad_args", "size must be between 1 and 16 MiB");

		std::vector<u8> pattern;
		if (!DebugServerJson::HexToBytes(ArgString(request, "pattern"), pattern) || pattern.empty())
			return Fail(request, "bad_args", "pattern must be a non-empty hex byte string");

		std::vector<u8> bytes(size);
		for (u32 i = 0; i < size; i++)
			bytes[i] = pattern[i % pattern.size()];

		bool ok = false;
		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout([cpu_type, addr, size, &bytes, &ok]() {
				ok = DebugInterface::get(cpu_type).WriteBytes(addr, bytes.data(), size);
			}))
		{
			return Fail(request, "timeout", "the CPU thread did not respond");
		}

		if (!ok)
			return Fail(request, "bad_address", "could not write there");

		rapidjson::Document result;
		result.SetObject();
		auto& allocator = result.GetAllocator();
		AddAddress(result, "addr", addr, allocator);
		result.AddMember("bytes_written", size, allocator);
		return DebugServerJson::MakeResult(request.id, result, allocator);
	}

	std::string CmdMemDump(const DebugServerRequest& request, DebugServerConnection&)
	{
		std::string failure;
		if (!RequireVM(request, failure))
			return failure;

		const BreakPointCpu cpu_type = ArgCpu(request);

		u32 addr = 0;
		std::string error;
		if (!ResolveAddress(request, "addr", cpu_type, addr, error))
			return Fail(request, "bad_address", error);

		const u32 size = ArgU32(request, "size", 0);
		if (size == 0)
			return Fail(request, "bad_args", "size is required");

		const std::string path = ArgString(request, "path");
		if (path.empty())
			return Fail(request, "bad_args", "path is required");

		// Whole-RAM dumps are 32 MiB, far past what belongs in a JSON reply, so this writes
		// server side and returns the path instead.
		std::vector<u8> bytes(size);
		bool ok = false;
		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout([cpu_type, addr, size, &bytes, &ok]() {
				ok = DebugInterface::get(cpu_type).ReadBytes(addr, bytes.data(), size);
			}, 10000))
		{
			return Fail(request, "timeout", "the CPU thread did not respond");
		}

		if (!ok)
			return Fail(request, "bad_address", "could not read that range");

		std::FILE* file = std::fopen(path.c_str(), "wb");
		if (!file)
			return Fail(request, "io_error", "could not open " + path + " for writing");

		const size_t written = std::fwrite(bytes.data(), 1, bytes.size(), file);
		std::fclose(file);

		if (written != bytes.size())
			return Fail(request, "io_error", "short write to " + path);

		rapidjson::Document result;
		result.SetObject();
		auto& allocator = result.GetAllocator();
		AddAddress(result, "addr", addr, allocator);
		result.AddMember("size", size, allocator);
		result.AddMember("path", Str(path, allocator), allocator);
		return DebugServerJson::MakeResult(request.id, result, allocator);
	}

	// --- code ---------------------------------------------------------------------------

	std::string CmdDisassemble(const DebugServerRequest& request, DebugServerConnection&)
	{
		std::string failure;
		if (!RequireVM(request, failure))
			return failure;

		const BreakPointCpu cpu_type = ArgCpu(request);

		u32 addr = 0;
		std::string error;
		if (!ResolveAddress(request, "addr", cpu_type, addr, error))
			return Fail(request, "bad_address", error);

		const u32 count = std::clamp(ArgU32(request, "count", 16), 1u, 1024u);
		const bool simplify = ArgBool(request, "simplify", true);

		struct Line
		{
			u32 addr;
			u32 opcode;
			std::string text;
			std::string symbol;
		};

		std::vector<Line> lines;

		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout([cpu_type, addr, count, simplify, &lines]() {
				DebugInterface& cpu = DebugInterface::get(cpu_type);
				for (u32 i = 0; i < count; i++)
				{
					const u32 at = addr + (i * 4);
					Line line;
					line.addr = at;
					line.opcode = cpu.Read32(at);
					line.text = cpu.disasm(at, simplify);

					const FunctionInfo function = cpu.GetSymbolGuardian().FunctionOverlappingAddress(at);
					if (!function.name.empty())
						line.symbol = function.name;

					lines.push_back(std::move(line));
				}
			}, 5000))
		{
			return Fail(request, "timeout", "the CPU thread did not respond");
		}

		rapidjson::Document result;
		result.SetObject();
		auto& allocator = result.GetAllocator();

		rapidjson::Value list(rapidjson::kArrayType);
		for (const Line& line : lines)
		{
			rapidjson::Value entry(rapidjson::kObjectType);
			AddAddress(entry, "addr", line.addr, allocator);
			entry.AddMember("opcode", line.opcode, allocator);
			entry.AddMember("opcode_hex", Str(DebugServerJson::HexU32(line.opcode), allocator), allocator);
			entry.AddMember("text", Str(line.text, allocator), allocator);
			if (!line.symbol.empty())
				entry.AddMember("symbol", Str(line.symbol, allocator), allocator);

			list.PushBack(entry, allocator);
		}

		result.AddMember("instructions", list, allocator);
		return DebugServerJson::MakeResult(request.id, result, allocator);
	}

	std::string CmdAssemble(const DebugServerRequest& request, DebugServerConnection&)
	{
		std::string failure;
		if (!RequireVM(request, failure))
			return failure;

		const BreakPointCpu cpu_type = ArgCpu(request);

		u32 addr = 0;
		std::string error;
		if (!ResolveAddress(request, "addr", cpu_type, addr, error))
			return Fail(request, "bad_address", error);

		std::vector<std::string> sources;
		const rapidjson::Value* instructions = Member(request, "instructions");
		if (instructions && instructions->IsString())
		{
			sources.emplace_back(instructions->GetString(), instructions->GetStringLength());
		}
		else if (instructions && instructions->IsArray())
		{
			for (const rapidjson::Value& item : instructions->GetArray())
			{
				if (!item.IsString())
					return Fail(request, "bad_args", "instructions entries must be strings");

				sources.emplace_back(item.GetString(), item.GetStringLength());
			}
		}

		if (sources.empty())
			return Fail(request, "bad_args", "instructions is required");

		std::vector<u32> words(sources.size());
		std::vector<u8> before(sources.size() * 4);
		std::string assemble_error;
		bool assembled = false;
		bool written = false;

		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout(
				[cpu_type, addr, &sources, &words, &before, &assemble_error, &assembled, &written]() {
					DebugInterface& cpu = DebugInterface::get(cpu_type);

					// Everything is assembled before anything is written: a half-applied
					// patch is worse than none, and a failing line must leave memory alone.
					for (size_t i = 0; i < sources.size(); i++)
					{
						const u32 at = addr + static_cast<u32>(i * 4);
						if (!MipsAssembleOpcode(sources[i].c_str(), &cpu, at, words[i], assemble_error))
							return;
					}

					assembled = true;

					if (!cpu.ReadBytes(addr, before.data(), static_cast<u32>(before.size())))
						return;

					written = cpu.WriteBytes(addr, words.data(), static_cast<u32>(words.size() * 4));
				}))
		{
			return Fail(request, "timeout", "the CPU thread did not respond");
		}

		if (!assembled)
			return Fail(request, "bad_args", "could not assemble: " + assemble_error);

		if (!written)
			return Fail(request, "bad_address", "could not write the assembled words there");

		rapidjson::Document result;
		result.SetObject();
		auto& allocator = result.GetAllocator();
		AddAddress(result, "addr", addr, allocator);

		rapidjson::Value word_list(rapidjson::kArrayType);
		for (const u32 word : words)
			word_list.PushBack(Str(DebugServerJson::HexU32(word), allocator), allocator);

		result.AddMember("words", word_list, allocator);
		result.AddMember("before", Str(DebugServerJson::BytesToHex(before.data(), before.size()), allocator),
			allocator);
		return DebugServerJson::MakeResult(request.id, result, allocator);
	}

	// --- symbols ------------------------------------------------------------------------

	std::string CmdSymLookup(const DebugServerRequest& request, DebugServerConnection&)
	{
		std::string failure;
		if (!RequireVM(request, failure))
			return failure;

		const BreakPointCpu cpu_type = ArgCpu(request);

		u32 addr = 0;
		std::string error;
		if (!ResolveAddress(request, "addr", cpu_type, addr, error))
			return Fail(request, "bad_address", error);

		std::string name;
		u32 symbol_address = 0;
		u32 size = 0;

		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout([cpu_type, addr, &name, &symbol_address, &size]() {
				const SymbolInfo info =
					DebugInterface::get(cpu_type).GetSymbolGuardian().SymbolOverlappingAddress(addr);
				name = info.name;
				symbol_address = info.address.valid() ? info.address.value : 0;
				size = info.size;
			}))
		{
			return Fail(request, "timeout", "the CPU thread did not respond");
		}

		rapidjson::Document result;
		result.SetObject();
		auto& allocator = result.GetAllocator();
		AddAddress(result, "addr", addr, allocator);
		result.AddMember("found", !name.empty(), allocator);

		if (!name.empty())
		{
			result.AddMember("name", Str(name, allocator), allocator);
			AddAddress(result, "symbol_addr", symbol_address, allocator);
			result.AddMember("size", size, allocator);
			result.AddMember("offset", addr - symbol_address, allocator);
		}

		return DebugServerJson::MakeResult(request.id, result, allocator);
	}

	std::string CmdSymFind(const DebugServerRequest& request, DebugServerConnection&)
	{
		std::string failure;
		if (!RequireVM(request, failure))
			return failure;

		const BreakPointCpu cpu_type = ArgCpu(request);
		const std::string name = ArgString(request, "name");
		if (name.empty())
			return Fail(request, "bad_args", "name is required");

		u32 address = 0;
		u32 size = 0;
		bool found = false;

		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout([cpu_type, name, &address, &size, &found]() {
				const SymbolInfo info = DebugInterface::get(cpu_type).GetSymbolGuardian().SymbolWithName(name);
				found = !info.name.empty() && info.address.valid();
				address = info.address.valid() ? info.address.value : 0;
				size = info.size;
			}))
		{
			return Fail(request, "timeout", "the CPU thread did not respond");
		}

		if (!found)
			return Fail(request, "bad_args", "no symbol named " + name);

		rapidjson::Document result;
		result.SetObject();
		auto& allocator = result.GetAllocator();
		result.AddMember("name", Str(name, allocator), allocator);
		AddAddress(result, "addr", address, allocator);
		result.AddMember("size", size, allocator);
		return DebugServerJson::MakeResult(request.id, result, allocator);
	}

	// --- context ------------------------------------------------------------------------

	const char* ThreadStatusName(ThreadStatus status)
	{
		switch (status)
		{
			case ThreadStatus::THS_RUN:
				return "run";
			case ThreadStatus::THS_READY:
				return "ready";
			case ThreadStatus::THS_WAIT:
				return "wait";
			case ThreadStatus::THS_SUSPEND:
				return "suspend";
			case ThreadStatus::THS_WAIT_SUSPEND:
				return "wait_suspend";
			case ThreadStatus::THS_DORMANT:
				return "dormant";
			case ThreadStatus::THS_BAD:
			default:
				return "bad";
		}
	}

	std::string CmdStack(const DebugServerRequest& request, DebugServerConnection&)
	{
		std::string failure;
		if (!RequireVM(request, failure) || !RequirePaused(request, failure))
			return failure;

		const BreakPointCpu cpu_type = ArgCpu(request);

		std::vector<MipsStackWalk::StackFrame> frames;
		std::vector<std::string> functions;

		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout([cpu_type, &frames, &functions]() {
				DebugInterface& cpu = DebugInterface::get(cpu_type);

				for (const auto& thread : cpu.GetThreadList())
				{
					if (thread->Status() != ThreadStatus::THS_RUN)
						continue;

					frames = MipsStackWalk::Walk(&cpu, cpu.getPC(), cpu.getRegister(0, 31),
						cpu.getRegister(0, 29), thread->EntryPoint());
					break;
				}

				for (const MipsStackWalk::StackFrame& frame : frames)
					functions.push_back(cpu.GetSymbolGuardian().FunctionOverlappingAddress(frame.pc).name);
			}, 5000))
		{
			return Fail(request, "timeout", "the CPU thread did not respond");
		}

		rapidjson::Document result;
		result.SetObject();
		auto& allocator = result.GetAllocator();

		rapidjson::Value list(rapidjson::kArrayType);
		for (size_t i = 0; i < frames.size(); i++)
		{
			rapidjson::Value entry(rapidjson::kObjectType);
			AddAddress(entry, "pc", frames[i].pc, allocator);
			AddAddress(entry, "entry", frames[i].entry, allocator);
			AddAddress(entry, "sp", frames[i].sp, allocator);
			entry.AddMember("stack_size", frames[i].stackSize, allocator);
			entry.AddMember("function", Str(functions[i], allocator), allocator);
			list.PushBack(entry, allocator);
		}

		result.AddMember("frames", list, allocator);
		return DebugServerJson::MakeResult(request.id, result, allocator);
	}

	std::string CmdThreads(const DebugServerRequest& request, DebugServerConnection&)
	{
		std::string failure;
		if (!RequireVM(request, failure))
			return failure;

		const BreakPointCpu cpu_type = ArgCpu(request);

		struct ThreadRow
		{
			u32 tid;
			u32 pc;
			u32 entry;
			u32 priority;
			ThreadStatus status;
			u32 wait_id;
		};

		std::vector<ThreadRow> rows;

		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout([cpu_type, &rows]() {
				for (const auto& thread : DebugInterface::get(cpu_type).GetThreadList())
				{
					rows.push_back({thread->TID(), thread->PC(), thread->EntryPoint(), thread->Priority(),
						thread->Status(), thread->WaitId()});
				}
			}, 5000))
		{
			return Fail(request, "timeout", "the CPU thread did not respond");
		}

		rapidjson::Document result;
		result.SetObject();
		auto& allocator = result.GetAllocator();

		rapidjson::Value list(rapidjson::kArrayType);
		for (const ThreadRow& row : rows)
		{
			rapidjson::Value entry(rapidjson::kObjectType);
			entry.AddMember("tid", row.tid, allocator);
			AddAddress(entry, "pc", row.pc, allocator);
			AddAddress(entry, "entry", row.entry, allocator);
			entry.AddMember("priority", row.priority, allocator);
			entry.AddMember("status", rapidjson::Value(ThreadStatusName(row.status), allocator), allocator);
			entry.AddMember("wait_id", row.wait_id, allocator);
			list.PushBack(entry, allocator);
		}

		result.AddMember("threads", list, allocator);
		return DebugServerJson::MakeResult(request.id, result, allocator);
	}

	std::string CmdModules(const DebugServerRequest& request, DebugServerConnection&)
	{
		std::string failure;
		if (!RequireVM(request, failure))
			return failure;

		const BreakPointCpu cpu_type = ArgCpu(request);

		std::vector<IopMod> modules;
		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout(
				[cpu_type, &modules]() { modules = DebugInterface::get(cpu_type).GetModuleList(); }, 5000))
		{
			return Fail(request, "timeout", "the CPU thread did not respond");
		}

		rapidjson::Document result;
		result.SetObject();
		auto& allocator = result.GetAllocator();

		rapidjson::Value list(rapidjson::kArrayType);
		for (const IopMod& module : modules)
		{
			rapidjson::Value entry(rapidjson::kObjectType);
			entry.AddMember("name", Str(module.name, allocator), allocator);
			entry.AddMember("version", module.version, allocator);
			AddAddress(entry, "entry", module.entry, allocator);
			AddAddress(entry, "text_addr", module.text_addr, allocator);
			entry.AddMember("text_size", module.text_size, allocator);
			entry.AddMember("data_size", module.data_size, allocator);
			list.PushBack(entry, allocator);
		}

		result.AddMember("modules", list, allocator);
		return DebugServerJson::MakeResult(request.id, result, allocator);
	}

	std::string CmdEval(const DebugServerRequest& request, DebugServerConnection&)
	{
		std::string failure;
		if (!RequireVM(request, failure))
			return failure;

		const BreakPointCpu cpu_type = ArgCpu(request);
		const std::string expression = ArgString(request, "expression");
		if (expression.empty())
			return Fail(request, "bad_args", "expression is required");

		u64 value = 0;
		bool ok = false;
		std::string parse_error;

		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout([cpu_type, expression, &value, &ok, &parse_error]() {
				ok = DebugInterface::get(cpu_type).evaluateExpression(expression.c_str(), value, parse_error);
			}))
		{
			return Fail(request, "timeout", "the CPU thread did not respond");
		}

		if (!ok)
			return Fail(request, "bad_args", "could not evaluate: " + parse_error);

		rapidjson::Document result;
		result.SetObject();
		auto& allocator = result.GetAllocator();
		result.AddMember("expression", Str(expression, allocator), allocator);
		result.AddMember("value", value, allocator);
		result.AddMember("value_hex", Str(fmt::format("0x{:x}", value), allocator), allocator);
		return DebugServerJson::MakeResult(request.id, result, allocator);
	}

	// --- session state and observation --------------------------------------------------

	// Exactly one of slot or path, so an ambiguous request fails loudly instead of
	// silently picking one.
	bool ArgSlotOrPath(const DebugServerRequest& request, s32& slot, std::string& path, std::string& error)
	{
		const rapidjson::Value* slot_value = Member(request, "slot");
		path = ArgString(request, "path");

		const bool has_slot = slot_value && slot_value->IsInt();
		if (has_slot == !path.empty())
		{
			error = "pass exactly one of slot or path";
			return false;
		}

		if (has_slot)
		{
			slot = slot_value->GetInt();
			if (slot < 0 || slot > 9)
			{
				error = "slot must be between 0 and 9";
				return false;
			}
		}

		return true;
	}

	std::string CmdSaveState(const DebugServerRequest& request, DebugServerConnection&)
	{
		std::string failure;
		if (!RequireVM(request, failure))
			return failure;

		s32 slot = 0;
		std::string path;
		std::string error;
		if (!ArgSlotOrPath(request, slot, path, error))
			return Fail(request, "bad_args", error);

		const bool wait_flush = ArgBool(request, "wait_flush", false);

		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout([slot, path, wait_flush]() {
				if (path.empty())
					VMManager::SaveStateToSlot(slot, true, nullptr);
				else
					VMManager::SaveState(path.c_str(), true, false, nullptr);

				// Saving compresses on a worker thread, so without this the file is not on
				// disk when the reply arrives. Opt-in, because it can take a moment.
				if (wait_flush)
					VMManager::WaitForSaveStateFlush();
			}, wait_flush ? 30000 : 5000))
		{
			return Fail(request, "timeout", "the CPU thread did not respond");
		}

		rapidjson::Document result;
		result.SetObject();
		auto& allocator = result.GetAllocator();
		result.AddMember("queued", true, allocator);
		result.AddMember("flushed", wait_flush, allocator);
		if (path.empty())
			result.AddMember("slot", slot, allocator);
		else
			result.AddMember("path", Str(path, allocator), allocator);

		return DebugServerJson::MakeResult(request.id, result, allocator);
	}

	std::string CmdLoadState(const DebugServerRequest& request, DebugServerConnection&)
	{
		std::string failure;
		if (!RequireVM(request, failure))
			return failure;

		s32 slot = 0;
		std::string path;
		std::string error;
		if (!ArgSlotOrPath(request, slot, path, error))
			return Fail(request, "bad_args", error);

		bool loaded = false;
		std::string load_error;

		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout([slot, path, &loaded, &load_error]() {
				Error err;
				loaded = path.empty() ? VMManager::LoadStateFromSlot(slot, false, &err)
									  : VMManager::LoadState(path.c_str(), &err);
				if (!loaded)
					load_error = err.GetDescription();
			}, 30000))
		{
			return Fail(request, "timeout", "the CPU thread did not respond");
		}

		if (!loaded)
			return Fail(request, "io_error", load_error.empty() ? "could not load the state" : load_error);

		rapidjson::Document result;
		result.SetObject();
		auto& allocator = result.GetAllocator();
		result.AddMember("loaded", true, allocator);
		if (path.empty())
			result.AddMember("slot", slot, allocator);
		else
			result.AddMember("path", Str(path, allocator), allocator);

		return DebugServerJson::MakeResult(request.id, result, allocator);
	}

	std::string CmdScreenshot(const DebugServerRequest& request, DebugServerConnection&)
	{
		std::string failure;
		if (!RequireVM(request, failure))
			return failure;

		const std::string path = ArgString(request, "path");
		if (path.empty())
			return Fail(request, "bad_args", "path is required");

		// This is what lets an agent observe the game without the window: a visual change is
		// often the only evidence that a patch did what was intended.
		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout([path]() { GSQueueSnapshot(path, 0); }))
			return Fail(request, "timeout", "the CPU thread did not respond");

		rapidjson::Document result;
		result.SetObject();
		auto& allocator = result.GetAllocator();
		// The snapshot lands asynchronously on the GS thread, so the file may not exist yet.
		result.AddMember("queued", true, allocator);
		result.AddMember("path", Str(path, allocator), allocator);
		return DebugServerJson::MakeResult(request.id, result, allocator);
	}

	std::string CmdPatchReload(const DebugServerRequest& request, DebugServerConnection&)
	{
		std::string failure;
		if (!RequireVM(request, failure))
			return failure;

		// A freshly deployed pnach is only read at boot otherwise.
		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout(
				[]() { VMManager::ReloadPatches(true, true, true, false); }, 10000))
		{
			return Fail(request, "timeout", "the CPU thread did not respond");
		}

		rapidjson::Document result;
		result.SetObject();
		result.AddMember("reloaded", true, result.GetAllocator());
		return DebugServerJson::MakeResult(request.id, result, result.GetAllocator());
	}

	// --- memory search ------------------------------------------------------------------

	// Search sessions live here so a chained filter has something to narrow. Capped in both
	// count and size: an unbounded first pass over 32 MiB would otherwise be a memory leak
	// with extra steps.
	constexpr size_t MAX_SEARCH_SESSIONS = 8;
	constexpr size_t MAX_SESSION_HITS = 1000000;

	std::mutex s_search_mutex;
	std::map<u32, std::vector<MemorySearch::Hit>> s_search_sessions;
	u32 s_next_search_session = 1;

	// Encodes a needle from a JSON value into guest bytes for the given type.
	bool EncodeNeedle(const rapidjson::Value& value, MemorySearch::ValueType type, std::vector<u8>& out)
	{
		const size_t size = MemorySearch::ValueSize(type);
		if (size == 0)
			return false;

		out.assign(size, 0);

		if (type == MemorySearch::ValueType::F32)
		{
			if (!value.IsNumber())
				return false;

			const float number = static_cast<float>(value.GetDouble());
			std::memcpy(out.data(), &number, sizeof(number));
			return true;
		}

		if (type == MemorySearch::ValueType::F64)
		{
			if (!value.IsNumber())
				return false;

			const double number = value.GetDouble();
			std::memcpy(out.data(), &number, sizeof(number));
			return true;
		}

		u64 raw = 0;
		if (value.IsUint64())
			raw = value.GetUint64();
		else if (value.IsInt64())
			raw = static_cast<u64>(value.GetInt64());
		else if (value.IsString())
		{
			u32 parsed = 0;
			if (!DebugServerJson::ParseAddressLiteral(
					std::string_view(value.GetString(), value.GetStringLength()), parsed))
				return false;

			raw = parsed;
		}
		else
		{
			return false;
		}

		std::memcpy(out.data(), &raw, size);
		return true;
	}

	std::string CmdMemSearch(const DebugServerRequest& request, DebugServerConnection&)
	{
		std::string failure;
		if (!RequireVM(request, failure))
			return failure;

		const BreakPointCpu cpu_type = ArgCpu(request);

		MemorySearch::Query query;
		if (!MemorySearch::ParseValueType(ArgString(request, "type").empty() ? "u32" : ArgString(request, "type"),
				query.type))
		{
			return Fail(request, "bad_args", "unknown value type");
		}

		const std::string comparison = ArgString(request, "comparison");
		if (!MemorySearch::ParseComparison(comparison.empty() ? "eq" : comparison, query.comparison))
			return Fail(request, "bad_args", "unknown comparison");

		query.max_results = std::clamp<size_t>(ArgU32(request, "max_results", 1000), 1, 100000);

		if (const rapidjson::Value* value = Member(request, "value"))
		{
			if (!EncodeNeedle(*value, query.type, query.value))
				return Fail(request, "bad_args", "value does not match the value type");
		}

		const u32 session_id = ArgU32(request, "session", 0);
		std::vector<MemorySearch::Hit> previous;

		if (session_id != 0)
		{
			std::lock_guard lock(s_search_mutex);
			const auto it = s_search_sessions.find(session_id);
			if (it == s_search_sessions.end())
				return Fail(request, "bad_args", "no such search session");

			previous = it->second;
		}
		else
		{
			std::string error;
			if (!ResolveAddress(request, "start", cpu_type, query.start, error))
				return Fail(request, "bad_address", error);
			if (!ResolveAddress(request, "end", cpu_type, query.end, error))
				return Fail(request, "bad_address", error);
		}

		std::vector<MemorySearch::Hit> hits;
		std::string search_error;
		bool ok = false;

		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout(
				[cpu_type, &query, &previous, session_id, &hits, &search_error, &ok]() {
					MemoryInterface& memory = DebugInterface::get(cpu_type);
					ok = (session_id != 0)
							 ? MemorySearch::RunFilterPass(memory, query, previous, hits, search_error)
							 : MemorySearch::RunFirstPass(memory, query, hits, search_error);
				},
				30000))
		{
			return Fail(request, "timeout", "the search did not finish in time");
		}

		if (!ok)
			return Fail(request, "bad_args", search_error);

		u32 result_session = session_id;
		{
			std::lock_guard lock(s_search_mutex);
			if (result_session == 0)
			{
				if (s_search_sessions.size() >= MAX_SEARCH_SESSIONS)
					s_search_sessions.erase(s_search_sessions.begin());

				result_session = s_next_search_session++;
			}

			std::vector<MemorySearch::Hit> stored = hits;
			if (stored.size() > MAX_SESSION_HITS)
				stored.resize(MAX_SESSION_HITS);

			s_search_sessions[result_session] = std::move(stored);
		}

		rapidjson::Document result;
		result.SetObject();
		auto& allocator = result.GetAllocator();
		result.AddMember("session", result_session, allocator);
		result.AddMember("count", static_cast<u64>(hits.size()), allocator);
		result.AddMember("truncated", hits.size() >= query.max_results, allocator);

		rapidjson::Value list(rapidjson::kArrayType);
		for (const MemorySearch::Hit& hit : hits)
		{
			rapidjson::Value entry(rapidjson::kObjectType);
			AddAddress(entry, "addr", hit.addr, allocator);
			entry.AddMember("value", hit.raw, allocator);
			entry.AddMember("value_number", hit.as_double, allocator);
			list.PushBack(entry, allocator);
		}

		result.AddMember("results", list, allocator);
		return DebugServerJson::MakeResult(request.id, result, allocator);
	}

	// --- booting ------------------------------------------------------------------------

	std::string CmdBoot(const DebugServerRequest& request, DebugServerConnection&)
	{
		if (DebugServerDispatch::VMIsValid())
			return Fail(request, "bad_args", "a VM is already running; shut it down first");

		VMBootParameters boot;
		boot.filename = ArgString(request, "path");
		boot.elf_override = ArgString(request, "elf");

		const bool bios_only = ArgBool(request, "bios", false);
		if (bios_only)
			boot.source_type = CDVD_SourceType::NoDisc;
		else if (boot.filename.empty() && boot.elf_override.empty())
			return Fail(request, "bad_args", "pass a path, an elf, or bios:true");

		if (const rapidjson::Value* fast = Member(request, "fast_boot"); fast && fast->IsBool())
			boot.fast_boot = fast->GetBool();

		// Pause on entry is set before booting rather than after: by the time a reply came
		// back the ELF would already be running, and the entry point long gone.
		const bool pause_on_entry = ArgBool(request, "pause_on_entry", false);

		std::string boot_error;
		bool started = false;

		// Booting takes seconds, so it is given a long budget; the alternative is a spurious
		// timeout on a boot that is progressing perfectly well.
		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout(
				[boot, pause_on_entry, &started, &boot_error]() {
					DebugInterface::setPauseOnEntry(pause_on_entry);

					Error error;
					const VMBootResult result = VMManager::Initialize(boot, &error);
					started = (result == VMBootResult::StartupSuccess);
					if (result == VMBootResult::PromptDisableHardcoreMode)
					{
						// Not something the server can answer: turning hardcore mode off is
						// the user's decision, and it would disable the debug server anyway.
						boot_error = "RetroAchievements hardcore mode is active; disable it to debug";
					}
					else if (!started)
					{
						boot_error = error.GetDescription();
					}

					if (started)
						VMManager::SetState(VMState::Running);
				},
				60000))
		{
			return Fail(request, "timeout", "the boot did not finish in time");
		}

		if (!started)
			return Fail(request, "io_error", boot_error.empty() ? "the VM failed to start" : boot_error);

		rapidjson::Document result;
		result.SetObject();
		auto& allocator = result.GetAllocator();
		result.AddMember("booted", true, allocator);
		result.AddMember("vm_state", rapidjson::Value(VMStateName(), allocator), allocator);
		return DebugServerJson::MakeResult(request.id, result, allocator);
	}

	// --- pad input ----------------------------------------------------------------------
	//
	// Injection goes through Pad::SetControllerState, the same entry point the input sources
	// use, so a bind behaves exactly as it would from a real controller: pressure, deadzone
	// and inversion settings all still apply.
	//
	// A held state is kept here and re-asserted every frame. The pad is repolled from the
	// real input sources each frame, so a single write would be overwritten immediately and
	// the button would appear not to work at all.

	struct HeldInput
	{
		u32 pad = 0;
		u32 bind = 0;
		float value = 1.0f;
		// 0 means hold until cleared; otherwise decremented once per frame.
		u32 frames_remaining = 0;
	};

	// A fixed slot per (pad, bind), held in atomics rather than behind a mutex.
	//
	// ApplyHeldInputs runs on the CPU thread every frame, including in the window where the
	// debugger is pausing and resuming the VM. A lock there put input injection and
	// execution control on the same mutex and wedged the emulator on resume once a session
	// had used both. Atomics mean the frame hook only ever loads and stores, so there is
	// nothing for it to block on.
	constexpr u32 MAX_INPUT_BINDS = 32;

	struct InputSlot
	{
		std::atomic<float> value{0.0f};
		std::atomic<u32> frames_remaining{0};
		std::atomic_bool active{false};
	};

	InputSlot s_input_slots[Pad::NUM_CONTROLLER_PORTS][MAX_INPUT_BINDS];

	void ClearPadSlots(u32 pad)
	{
		for (InputSlot& slot : s_input_slots[pad])
		{
			if (!slot.active.load(std::memory_order_acquire))
				continue;

			// Left active for one more frame at zero rather than switched off outright.
			// Simply going inactive would stop this slot being written, and the pad would
			// keep whatever value was last put there - so a released button stayed down.
			slot.value.store(0.0f, std::memory_order_relaxed);
			slot.frames_remaining.store(1, std::memory_order_release);
		}
	}

	bool ResolveBind(u32 pad, std::string_view name, u32& out_bind, std::string& error)
	{
		const Pad::ControllerInfo* info = Pad::GetControllerInfo(EmuConfig.Pad.Ports[pad].Type);
		if (!info)
		{
			error = "no controller is configured in that port";
			return false;
		}

		const std::optional<u32> index = info->GetBindIndex(name);
		if (!index.has_value())
		{
			error = "unknown button \"" + std::string(name) +
					"\" for controller type " + std::string(info->name);
			return false;
		}

		out_bind = index.value();
		return true;
	}

	// Parses {"buttons": ["cross", "start"], "analog": {"left": {"x": .., "y": ..}}} into
	// bind/value pairs. Button names are matched case insensitively against the controller's
	// own binding table, so "cross", "Cross" and "CROSS" all work.
	bool CollectInputs(const DebugServerRequest& request, u32 pad, std::vector<HeldInput>& out,
		std::string& error)
	{
		const Pad::ControllerInfo* info = Pad::GetControllerInfo(EmuConfig.Pad.Ports[pad].Type);
		if (!info)
		{
			error = "no controller is configured in that port";
			return false;
		}

		if (const rapidjson::Value* buttons = Member(request, "buttons"))
		{
			if (!buttons->IsArray())
			{
				error = "buttons must be an array of names";
				return false;
			}

			for (const rapidjson::Value& button : buttons->GetArray())
			{
				if (!button.IsString())
				{
					error = "button names must be strings";
					return false;
				}

				const std::string wanted = ToLower(std::string(button.GetString(), button.GetStringLength()));

				bool found = false;
				for (const InputBindingInfo& binding : info->bindings)
				{
					if (ToLower(binding.name) != wanted)
						continue;

					out.push_back({pad, static_cast<u32>(&binding - info->bindings.data()), 1.0f, 0});
					found = true;
					break;
				}

				if (!found)
				{
					error = "unknown button \"" + wanted + "\" for controller type " + info->name;
					return false;
				}
			}
		}

		// Analog sticks are expressed as -1..1 per axis and split across the two half-axis
		// binds the pad actually exposes, which is how a real stick reports.
		static const struct
		{
			const char* stick;
			const char* negative;
			const char* positive;
			bool is_x;
		} axes[] = {
			{"left", "LLeft", "LRight", true},
			{"left", "LUp", "LDown", false},
			{"right", "RLeft", "RRight", true},
			{"right", "RUp", "RDown", false},
		};

		const rapidjson::Value* analog = Member(request, "analog");
		if (analog && analog->IsObject())
		{
			for (const auto& axis : axes)
			{
				const auto stick = analog->FindMember(axis.stick);
				if (stick == analog->MemberEnd() || !stick->value.IsObject())
					continue;

				const auto component = stick->value.FindMember(axis.is_x ? "x" : "y");
				if (component == stick->value.MemberEnd() || !component->value.IsNumber())
					continue;

				const float value = std::clamp(static_cast<float>(component->value.GetDouble()), -1.0f, 1.0f);
				if (value == 0.0f)
					continue;

				u32 bind = 0;
				const char* name = (value < 0.0f) ? axis.negative : axis.positive;
				if (!ResolveBind(pad, name, bind, error))
					return false;

				out.push_back({pad, bind, std::fabs(value), 0});
			}
		}

		return true;
	}

	std::string ApplyInput(const DebugServerRequest& request, bool momentary)
	{
		std::string failure;
		if (!RequireVM(request, failure))
			return failure;

		const u32 pad = ArgU32(request, "pad", 0);
		if (pad >= Pad::NUM_CONTROLLER_PORTS)
			return Fail(request, "bad_args", "pad must be a valid controller port");

		const u32 frames = momentary ? std::clamp(ArgU32(request, "duration_frames", 2), 1u, 600u) : 0;

		std::vector<HeldInput> inputs;
		std::string error;
		bool ok = false;

		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout([&request, pad, &inputs, &error, &ok]() {
				ok = CollectInputs(request, pad, inputs, error);
			}))
		{
			return Fail(request, "timeout", "the CPU thread did not respond");
		}

		if (!ok)
			return Fail(request, "bad_args", error);

		// Replacing rather than adding: setting a state twice should not stack, and a set
		// with no buttons is how a caller releases everything on that pad.
		ClearPadSlots(pad);

		for (const HeldInput& input : inputs)
		{
			if (input.bind >= MAX_INPUT_BINDS)
				continue;

			// Overwrites the release-at-zero that ClearPadSlots just queued for this slot,
			// so re-pressing a button does not spend a frame released first.

			InputSlot& slot = s_input_slots[pad][input.bind];
			slot.value.store(input.value, std::memory_order_relaxed);
			slot.frames_remaining.store(frames, std::memory_order_relaxed);
			slot.active.store(true, std::memory_order_release);
		}

		rapidjson::Document result;
		result.SetObject();
		auto& allocator = result.GetAllocator();
		result.AddMember("pad", pad, allocator);
		result.AddMember("binds", static_cast<u64>(inputs.size()), allocator);
		if (momentary)
			result.AddMember("duration_frames", frames, allocator);
		else
			result.AddMember("held", !inputs.empty(), allocator);

		return DebugServerJson::MakeResult(request.id, result, allocator);
	}

	std::string CmdInputPress(const DebugServerRequest& request, DebugServerConnection&)
	{
		return ApplyInput(request, true);
	}

	std::string CmdInputSet(const DebugServerRequest& request, DebugServerConnection&)
	{
		return ApplyInput(request, false);
	}

	std::string CmdInputRelease(const DebugServerRequest& request, DebugServerConnection&)
	{
		std::string failure;
		if (!RequireVM(request, failure))
			return failure;

		for (u32 pad = 0; pad < Pad::NUM_CONTROLLER_PORTS; pad++)
			ClearPadSlots(pad);

		rapidjson::Document result;
		result.SetObject();
		result.AddMember("released", true, result.GetAllocator());
		return DebugServerJson::MakeResult(request.id, result, result.GetAllocator());
	}

	std::string CmdInputList(const DebugServerRequest& request, DebugServerConnection&)
	{
		std::string failure;
		if (!RequireVM(request, failure))
			return failure;

		const u32 pad = ArgU32(request, "pad", 0);
		if (pad >= Pad::NUM_CONTROLLER_PORTS)
			return Fail(request, "bad_args", "pad must be a valid controller port");

		std::vector<std::string> names;
		std::string type_name;

		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout([pad, &names, &type_name]() {
				const Pad::ControllerInfo* info = Pad::GetControllerInfo(EmuConfig.Pad.Ports[pad].Type);
				if (!info)
					return;

				type_name = info->name;
				for (const InputBindingInfo& binding : info->bindings)
				{
					if (binding.bind_type == InputBindingInfo::Type::Button ||
						binding.bind_type == InputBindingInfo::Type::HalfAxis)
					{
						names.emplace_back(binding.name);
					}
				}
			}))
		{
			return Fail(request, "timeout", "the CPU thread did not respond");
		}

		rapidjson::Document result;
		result.SetObject();
		auto& allocator = result.GetAllocator();
		result.AddMember("pad", pad, allocator);
		result.AddMember("controller", Str(type_name, allocator), allocator);

		rapidjson::Value list(rapidjson::kArrayType);
		for (const std::string& name : names)
			list.PushBack(Str(name, allocator), allocator);

		result.AddMember("buttons", list, allocator);
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

void DebugServerCommands::ApplyHeldInputs()
{
	// Only while actually executing. Pad::SetControllerState dereferences the controller for
	// the port without checking it exists, so writing to it while the VM is paused or being
	// torn down - which is exactly the window stepping puts us in - is a null dereference.
	if (VMManager::GetState() != VMState::Running)
		return;

	// Applied after InputManager::PollSources, which has just overwritten the pad from the
	// real controllers; writing before that point would be silently discarded. Lock free by
	// design - see the note on s_input_slots.
	for (u32 pad = 0; pad < Pad::NUM_CONTROLLER_PORTS; pad++)
	{
		if (!Pad::HasConnectedPad(static_cast<u8>(pad)))
			continue;

		for (u32 bind = 0; bind < MAX_INPUT_BINDS; bind++)
		{
			InputSlot& slot = s_input_slots[pad][bind];
			if (!slot.active.load(std::memory_order_acquire))
				continue;

			Pad::SetControllerState(pad, bind, slot.value.load(std::memory_order_relaxed));

			// 0 means hold until released; anything else is a tap that expires.
			const u32 remaining = slot.frames_remaining.load(std::memory_order_relaxed);
			if (remaining == 0)
				continue;

			if (remaining == 1)
			{
				// Released explicitly on the last frame, so the button does not stick down
				// simply because nothing overwrote it.
				Pad::SetControllerState(pad, bind, 0.0f);
				slot.active.store(false, std::memory_order_release);
			}

			slot.frames_remaining.store(remaining - 1, std::memory_order_relaxed);
		}
	}
}

void DebugServerCommands::ClearHeldInputs()
{
	for (u32 pad = 0; pad < Pad::NUM_CONTROLLER_PORTS; pad++)
		ClearPadSlots(pad);
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

	s_handlers["bp.add"] = CmdBpAdd;
	s_handlers["bp.remove"] = CmdBpRemove;
	s_handlers["bp.list"] = CmdBpList;
	s_handlers["bp.enable"] = CmdBpEnable;
	s_handlers["bp.disable"] = CmdBpDisable;
	s_handlers["bp.clear"] = CmdBpClear;

	s_handlers["mc.add"] = CmdMcAdd;
	s_handlers["mc.remove"] = CmdMcRemove;
	s_handlers["mc.list"] = CmdMcList;
	s_handlers["mc.clear"] = CmdMcClear;

	s_handlers["reg.list"] = CmdRegList;
	s_handlers["reg.get"] = CmdRegGet;
	s_handlers["reg.set"] = CmdRegSet;
	s_handlers["reg.dump"] = CmdRegDump;

	s_handlers["mem.read"] = CmdMemRead;
	s_handlers["mem.write"] = CmdMemWrite;
	s_handlers["mem.fill"] = CmdMemFill;
	s_handlers["mem.dump"] = CmdMemDump;
	s_handlers["mem.search"] = CmdMemSearch;

	s_handlers["dis"] = CmdDisassemble;
	s_handlers["asm"] = CmdAssemble;

	s_handlers["sym.lookup"] = CmdSymLookup;
	s_handlers["sym.find"] = CmdSymFind;

	s_handlers["stack"] = CmdStack;
	s_handlers["threads"] = CmdThreads;
	s_handlers["modules"] = CmdModules;
	s_handlers["eval"] = CmdEval;

	s_handlers["savestate"] = CmdSaveState;
	s_handlers["loadstate"] = CmdLoadState;
	s_handlers["screenshot"] = CmdScreenshot;
	s_handlers["patch.reload"] = CmdPatchReload;

	s_handlers["boot"] = CmdBoot;

	s_handlers["input.press"] = CmdInputPress;
	s_handlers["input.set"] = CmdInputSet;
	s_handlers["input.release"] = CmdInputRelease;
	s_handlers["input.list"] = CmdInputList;
}

const DebugServerHandler* DebugServerCommands::Find(const std::string& name)
{
	const auto it = s_handlers.find(name);
	return it == s_handlers.end() ? nullptr : &it->second;
}
