// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsxroo/server/DebugServerDispatch.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using namespace std::chrono_literals;

TEST(DebugServerDispatch, RunsTheTaskAndReportsSuccess)
{
	int value = 0;
	EXPECT_TRUE(DebugServerDispatch::RunWithTimeout(
		[](std::function<void()> task) { task(); }, [&value] { value = 7; }, 1000));
	EXPECT_EQ(value, 7);
}

// Every command handler captures its results by reference. A task that the CPU thread picked
// up only after the handler had timed out and returned used to write into that dead frame.
TEST(DebugServerDispatch, ATaskNotStartedInTimeIsCancelledAndNeverRuns)
{
	std::function<void()> queued;
	bool ran = false;

	EXPECT_FALSE(DebugServerDispatch::RunWithTimeout(
		[&queued](std::function<void()> task) { queued = std::move(task); }, [&ran] { ran = true; }, 20));

	queued(); // the CPU thread gets to it late
	EXPECT_FALSE(ran);
}

TEST(DebugServerDispatch, ATaskAlreadyRunningAtTheTimeoutIsWaitedFor)
{
	std::thread cpu_thread;
	int result = 0;

	const bool ok = DebugServerDispatch::RunWithTimeout(
		[&cpu_thread](std::function<void()> task) { cpu_thread = std::thread(std::move(task)); },
		[&result] {
			std::this_thread::sleep_for(500ms);
			result = 42;
		},
		100);

	EXPECT_TRUE(ok);
	EXPECT_EQ(result, 42);
	cpu_thread.join();
}
