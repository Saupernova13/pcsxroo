// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "DebugTools/DebugInterface.h"
#include "DebugTools/MIPSAnalyst.h"

#include "common/Pcsx2Types.h"

#include <functional>

// Debugger session state shared by the Qt debugger window and the debug server.
//
// Stepping and the bookkeeping that has to happen after a breakpoint fires used to live in
// DebuggerWindow, which meant neither happened with the window closed. Both live here now,
// so a headless session behaves identically to a windowed one.
namespace DebuggerControl
{
	enum class StopReason
	{
		None,
		Breakpoint,
		MemCheck,
		Step,
		UserPause,
		Entry,
		VMShutdown
	};

	// Stable strings used on the wire. Do not rename without bumping the protocol version.
	const char* StopReasonName(StopReason reason);

	struct StopEvent
	{
		u64 seq = 0; // monotonic; the first recorded stop is 1
		StopReason reason = StopReason::None;
		BreakPointCpu cpu = BREAKPOINT_EE;
		u32 pc = 0;
		u32 bp_addr = 0;
		u32 mem_addr = 0;
		u32 mem_size = 0;
		bool mem_write = false;
	};

	using StopCallback = std::function<void(const StopEvent&)>;

	// Assigns the next sequence number, stores the event, and notifies waiters and callbacks.
	void RecordStop(StopEvent event);

	StopEvent GetLastStop();

	// Blocks until a stop with seq > since exists, or timeout_ms elapses. Returns false on
	// timeout. A stop that has already happened returns immediately, which is what stops an
	// agent from blocking forever on a breakpoint that fired before it started waiting.
	bool WaitForStop(u64 since, u32 timeout_ms, StopEvent& out);

	size_t AddStopCallback(StopCallback callback);
	void RemoveStopCallback(size_t handle);

	enum class StepMode
	{
		Into,
		Over,
		Out
	};

	// Pure: where execution lands after one step from pc. Extracted so it can be tested
	// without a VM, since this is the logic most likely to break.
	u32 ComputeStepTarget(StepMode mode, u32 pc, const MIPSAnalyst::MipsOpcodeInfo& info);

	// CPU thread only, VM must be paused. Sets a temporary stepping breakpoint at the
	// computed target and resumes. Returns false if the CPU is not in a steppable state,
	// or for Out when there is no caller frame to return to.
	bool Step(BreakPointCpu cpu, StepMode mode);

	// CPU thread only, VM must be paused. Temporary breakpoint at addr, then resume.
	bool RunTo(BreakPointCpu cpu, u32 addr);

	// Drops all state. Tests only.
	void ResetForTesting();
} // namespace DebuggerControl
