// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "DebugTools/DebuggerControl.h"

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

void DebuggerControl::ResetForTesting()
{
	std::lock_guard lock(s_mutex);
	s_last_stop = StopEvent();
	s_next_seq = 1;
	s_callbacks.clear();
	s_next_callback_handle = 1;
}
