// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <functional>

namespace DebugServerDispatch
{
	// Runs fn on the CPU thread and waits up to timeout_ms for it to finish.
	//
	// Host::RunOnCPUThread(fn, true) is deliberately not used: it is a
	// Qt::BlockingQueuedConnection, which cannot time out, so a wedged or exiting emu thread
	// would hang the calling client forever.
	//
	// Returns false on timeout, in which case fn may still run afterwards, so fn must not
	// outlive anything it captures by reference. In practice a timeout means the CPU thread
	// is not servicing its queue at all, which is already a wedged emulator.
	//
	// The wait is genuinely bounded. An earlier attempt held a mutex across fn so a
	// timed-out caller could know fn had stopped, but that forced the caller to acquire
	// that same mutex before it could start waiting - so a hung fn hung the caller forever
	// rather than timing out, which is strictly worse.
	//
	// While the VM is paused the emu thread sits in its event loop, so this returns almost
	// immediately. While it runs, the core pumps messages once per frame, so the worst case
	// is roughly one frame.
	bool RunOnCPUThreadWithTimeout(std::function<void()> fn, u32 timeout_ms = 2000);

	// True when a VM exists and is either running or paused.
	bool VMIsValid();
	bool VMIsPaused();
} // namespace DebugServerDispatch
