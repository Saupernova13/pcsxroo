// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "DebugTools/DebuggerControl.h"

#include "DebugTools/Breakpoints.h"
#include "DebugTools/MipsStackWalk.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

namespace
{
	std::mutex s_mutex;
	std::condition_variable s_stop_cv;
	DebuggerControl::StopEvent s_last_stop;
	u64 s_next_seq = 1;

	struct CallbackEntry
	{
		size_t handle;
		DebuggerControl::StopCallback callback;
	};

	std::vector<CallbackEntry> s_callbacks;
	size_t s_next_callback_handle = 1;
} // namespace

const char* DebuggerControl::StopReasonName(StopReason reason)
{
	switch (reason)
	{
		case StopReason::Breakpoint:
			return "breakpoint";
		case StopReason::MemCheck:
			return "memcheck";
		case StopReason::Step:
			return "step";
		case StopReason::UserPause:
			return "user";
		case StopReason::Entry:
			return "entry";
		case StopReason::VMShutdown:
			return "vm_shutdown";
		case StopReason::None:
		default:
			return "none";
	}
}

void DebuggerControl::RecordStop(StopEvent event)
{
	std::vector<CallbackEntry> callbacks;
	{
		std::lock_guard lock(s_mutex);
		event.seq = s_next_seq++;
		s_last_stop = event;
		callbacks = s_callbacks;
	}

	s_stop_cv.notify_all();

	// Callbacks run outside the lock: a subscriber writing to a slow socket must not hold
	// up the CPU thread that recorded the stop.
	for (const CallbackEntry& entry : callbacks)
		entry.callback(event);
}

DebuggerControl::StopEvent DebuggerControl::GetLastStop()
{
	std::lock_guard lock(s_mutex);
	return s_last_stop;
}

bool DebuggerControl::WaitForStop(u64 since, u32 timeout_ms, StopEvent& out)
{
	std::unique_lock lock(s_mutex);
	const bool signalled = s_stop_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
		[since] { return s_last_stop.seq > since; });

	if (!signalled)
		return false;

	out = s_last_stop;
	return true;
}

size_t DebuggerControl::AddStopCallback(StopCallback callback)
{
	std::lock_guard lock(s_mutex);
	const size_t handle = s_next_callback_handle++;
	s_callbacks.push_back({handle, std::move(callback)});
	return handle;
}

void DebuggerControl::RemoveStopCallback(size_t handle)
{
	std::lock_guard lock(s_mutex);
	std::erase_if(s_callbacks, [handle](const CallbackEntry& entry) { return entry.handle == handle; });
}

u32 DebuggerControl::ComputeStepTarget(StepMode mode, u32 pc, const MIPSAnalyst::MipsOpcodeInfo& info)
{
	u32 target = pc + 0x4; // default: the next instruction

	if (info.isBranch)
	{
		if (!info.isConditional)
		{
			// Step-over treats a call as a single instruction: land after the delay slot.
			if (mode == StepMode::Over && info.isLinkedBranch)
				target = pc + (2 * 4);
			else
				target = info.branchTarget;
		}
		else if (info.conditionMet)
		{
			target = info.branchTarget;
		}
		else
		{
			target = pc + (2 * 4); // skip the branch delay slot
		}
	}

	// Syscalls are always taken, but stepping over one should not enter the handler.
	if (info.isSyscall && mode == StepMode::Into)
		target = info.branchTarget;

	return target;
}

bool DebuggerControl::Step(BreakPointCpu cpu_type, StepMode mode)
{
	DebugInterface& cpu = DebugInterface::get(cpu_type);
	if (!cpu.isAlive() || !cpu.isCpuPaused())
		return false;

	const u32 pc = cpu.getPC();

	u32 target = 0;
	if (mode == StepMode::Out)
	{
		std::vector<MipsStackWalk::StackFrame> frames;
		for (const auto& thread : cpu.GetThreadList())
		{
			if (thread->Status() != ThreadStatus::THS_RUN)
				continue;

			frames = MipsStackWalk::Walk(
				&cpu, pc, cpu.getRegister(0, 31), cpu.getRegister(0, 29), thread->EntryPoint());
			break;
		}

		if (frames.size() < 2)
			return false; // nothing to return to

		target = frames.at(1).pc;
	}
	else
	{
		target = ComputeStepTarget(mode, pc, MIPSAnalyst::GetOpcodeInfo(&cpu, pc));
	}

	// Step-into and step-out let the core skip a breakpoint sitting on the current pc.
	// Step-over deliberately does not, matching the behaviour the debugger window had.
	if (mode != StepMode::Over)
		CBreakPoints::SetSkipFirst(cpu_type, pc);

	CBreakPoints::AddBreakPoint(cpu_type, target, true, true, true);
	cpu.resumeCpu();
	return true;
}

bool DebuggerControl::RunTo(BreakPointCpu cpu_type, u32 addr)
{
	DebugInterface& cpu = DebugInterface::get(cpu_type);
	if (!cpu.isAlive() || !cpu.isCpuPaused())
		return false;

	CBreakPoints::SetSkipFirst(cpu_type, cpu.getPC());
	CBreakPoints::AddBreakPoint(cpu_type, addr, true, true, true);
	cpu.resumeCpu();
	return true;
}

void DebuggerControl::ResetForTesting()
{
	std::lock_guard lock(s_mutex);
	s_last_stop = StopEvent();
	s_next_seq = 1;
	s_callbacks.clear();
	s_next_callback_handle = 1;
}
