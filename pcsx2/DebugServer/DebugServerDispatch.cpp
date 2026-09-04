// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "DebugServer/DebugServerDispatch.h"

#include "Host.h"
#include "VMManager.h"

#include <chrono>
#include <future>
#include <memory>

bool DebugServerDispatch::RunOnCPUThreadWithTimeout(std::function<void()> fn, u32 timeout_ms)
{
	// Shared rather than captured by reference: if the wait times out, this function
	// returns and its stack goes away, but the queued lambda may still run afterwards.
	auto promise = std::make_shared<std::promise<void>>();
	std::future<void> future = promise->get_future();

	Host::RunOnCPUThread([fn = std::move(fn), promise]() {
		fn();
		promise->set_value();
	});

	return future.wait_for(std::chrono::milliseconds(timeout_ms)) == std::future_status::ready;
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
