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
	// never touches the caller's stack before it has claimed the right to run.
	struct DispatchState
	{
		enum class Phase
		{
			Queued,
			Running,
			Finished,
			Cancelled
		};

		std::mutex mutex;
		std::condition_variable cv;
		Phase phase = Phase::Queued;
	};
} // namespace

bool DebugServerDispatch::RunWithTimeout(const std::function<void(std::function<void()>)>& post,
	std::function<void()> fn, u32 timeout_ms)
{
	using Phase = DispatchState::Phase;
	auto state = std::make_shared<DispatchState>();

	post([fn = std::move(fn), state]() {
		{
			std::lock_guard lock(state->mutex);
			if (state->phase == Phase::Cancelled)
				return; // the caller gave up and has returned; its captures are gone

			state->phase = Phase::Running;
		}

		fn();

		{
			std::lock_guard lock(state->mutex);
			state->phase = Phase::Finished;
		}

		state->cv.notify_all();
	});

	// The lock is never held across fn: the task takes it only to claim or report, so the
	// bounded wait below really is bounded.
	std::unique_lock lock(state->mutex);
	if (state->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
			[&state] { return state->phase == Phase::Finished; }))
	{
		return true;
	}

	if (state->phase == Phase::Queued)
	{
		state->phase = Phase::Cancelled;
		return false;
	}

	// fn is running and may be writing into the caller's locals, so returning now would leave
	// it doing that to a dead frame. It has started, which means the CPU thread is servicing
	// its queue; what remains is one call finishing.
	state->cv.wait(lock, [&state] { return state->phase == Phase::Finished; });
	return true;
}

bool DebugServerDispatch::RunOnCPUThreadWithTimeout(std::function<void()> fn, u32 timeout_ms)
{
	return RunWithTimeout([](std::function<void()> task) { Host::RunOnCPUThread(std::move(task)); }, std::move(fn),
		timeout_ms);
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
