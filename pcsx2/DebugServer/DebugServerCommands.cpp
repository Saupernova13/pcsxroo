// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "DebugServer/DebugServerCommands.h"

#include "DebugServer/DebugServer.h"
#include "DebugServer/DebugServerDispatch.h"

#include "DebugTools/Breakpoints.h"

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
			return Error(request, "bad_address", error);

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
			return Error(request, "timeout", "the CPU thread did not respond");

		if (!condition_ok)
			return Error(request, "bad_args", "condition did not parse: " + condition_error);

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
			return Error(request, "bad_address", error);

		bool removed = false;
		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout([cpu, addr, &removed]() {
				removed = CBreakPoints::IsAddressBreakPoint(cpu, addr);
				if (removed)
					CBreakPoints::RemoveBreakPoint(cpu, addr);
			}))
		{
			return Error(request, "timeout", "the CPU thread did not respond");
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
			return Error(request, "timeout", "the CPU thread did not respond");
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
			return Error(request, "bad_address", error);

		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout(
				[cpu, addr, enable]() { CBreakPoints::ChangeBreakPoint(cpu, addr, enable); }))
		{
			return Error(request, "timeout", "the CPU thread did not respond");
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
			return Error(request, "timeout", "the CPU thread did not respond");
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
			return Error(request, "bad_address", error);
		if (!ResolveAddress(request, "end", cpu, end, error))
			return Error(request, "bad_address", error);

		if (end <= start)
			return Error(request, "bad_args", "end must be greater than start");

		MemCheckCondition condition;
		if (!ParseMemCheckCondition(request, condition, error))
			return Error(request, "bad_args", error);

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
			return Error(request, "timeout", "the CPU thread did not respond");
		}

		if (!condition_ok)
			return Error(request, "bad_args", "condition did not parse: " + condition_error);

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
			return Error(request, "bad_address", error);
		if (!ResolveAddress(request, "end", cpu, end, error))
			return Error(request, "bad_address", error);

		if (!DebugServerDispatch::RunOnCPUThreadWithTimeout(
				[cpu, start, end]() { CBreakPoints::RemoveMemCheck(cpu, start, end); }))
		{
			return Error(request, "timeout", "the CPU thread did not respond");
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
			return Error(request, "timeout", "the CPU thread did not respond");
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
			return Error(request, "timeout", "the CPU thread did not respond");
		}

		rapidjson::Document result;
		result.SetObject();
		result.AddMember("removed", static_cast<u64>(removed), result.GetAllocator());
		return DebugServerJson::MakeResult(request.id, result, result.GetAllocator());
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
}

const DebugServerHandler* DebugServerCommands::Find(const std::string& name)
{
	const auto it = s_handlers.find(name);
	return it == s_handlers.end() ? nullptr : &it->second;
}
