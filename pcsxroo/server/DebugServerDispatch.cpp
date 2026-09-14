// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsxroo/server/DebugServerDispatch.h"

#include "Host.h"
#include "VMManager.h"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace
{
	// Shared between the caller and the queued task so that it outlives either. The task
	// never touches the caller's stack directly; it only ever signals through this.
	struct DispatchState
	{
		std::mutex mutex;
		std::condition_variable cv;
		bool finished = false;
	};
} // namespace

bool DebugServerDispatch::RunOnCPUThreadWithTimeout(std::function<void()> fn, u32 timeout_ms)
{
	auto state = std::make_shared<DispatchState>();

	Host::RunOnCPUThread([fn = std::move(fn), state]() {
		fn();

		{
			std::lock_guard lock(state->mutex);
			state->finished = true;
		}

		state->cv.notify_all();
	});

	// The lock is taken only to wait on the condition variable, never across fn itself.
	// An earlier version held it for the whole of fn so that a timed-out caller could know
	// fn had stopped - but that made the caller block on the mutex before it could even
	// start its bounded wait, so a hung fn hung the client forever instead of timing out.
	std::unique_lock lock(state->mutex);
	return state->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
		[&state] { return state->finished; });
}

bool DebugServerDispatch::VMIsValid()
{
	const VMState state = VMManager::GetState();
	return state == VMState::Running || state == VMState::Paused;
}

bool DebugServerDispatch::VMIsPaused()
{
	return VMManager::GetState() == VMState::Paused;
}
