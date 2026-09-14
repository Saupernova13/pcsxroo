// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <functional>

namespace DebugServerDispatch
{
	// Runs fn on the CPU thread and waits up to timeout_ms for it to start.
	//
	// Host::RunOnCPUThread(fn, true) is deliberately not used: it is a
	// Qt::BlockingQueuedConnection, which cannot time out, so a wedged or exiting emu thread
	// would hang the calling client forever.
	//
	// Returns false when the CPU thread has not picked fn up within timeout_ms. fn is then
	// cancelled and never runs, so it may safely capture the caller's locals by reference - a
	// late task can no longer write into a stack frame that has already returned. A fn that has
	// started by then is waited for, and the call returns true: it is running, so the CPU
	// thread is alive, and its results are about to be valid.
	//
	// While the VM is paused the emu thread sits in its event loop, so this returns almost
	// immediately. While it runs, the core pumps messages once per frame, so the worst case
	// is roughly one frame.
	bool RunOnCPUThreadWithTimeout(std::function<void()> fn, u32 timeout_ms = 2000);

	// The same, with the queue passed in. RunOnCPUThreadWithTimeout posts to the CPU thread;
	// tests post to a thread of their own so they can make the task start late or run long.
	bool RunWithTimeout(const std::function<void(std::function<void()>)>& post, std::function<void()> fn,
		u32 timeout_ms);

	// True when a VM exists and is either running or paused.
	bool VMIsValid();
	bool VMIsPaused();
} // namespace DebugServerDispatch
