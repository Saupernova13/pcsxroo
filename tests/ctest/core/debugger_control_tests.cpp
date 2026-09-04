// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "DebugTools/DebuggerControl.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using namespace std::chrono_literals;

namespace
{
	DebuggerControl::StopEvent MakeStop(u32 pc, u32 bp_addr)
	{
		DebuggerControl::StopEvent event;
		event.reason = DebuggerControl::StopReason::Breakpoint;
		event.cpu = BREAKPOINT_EE;
		event.pc = pc;
		event.bp_addr = bp_addr;
		return event;
	}
} // namespace

TEST(DebuggerControl, SequenceStartsAtOneAndIncrements)
{
	DebuggerControl::ResetForTesting();

	EXPECT_EQ(DebuggerControl::GetLastStop().seq, 0u);

	DebuggerControl::RecordStop(MakeStop(0x100000, 0x100000));
	EXPECT_EQ(DebuggerControl::GetLastStop().seq, 1u);

	DebuggerControl::RecordStop(MakeStop(0x100004, 0x100004));
	EXPECT_EQ(DebuggerControl::GetLastStop().seq, 2u);
	EXPECT_EQ(DebuggerControl::GetLastStop().pc, 0x100004u);
}

// The failure that matters most for unattended use: a stop that happened between
// "run" and "wait" must still be reported, not waited for a second time.
TEST(DebuggerControl, WaitReturnsImmediatelyForAStopAlreadyRecorded)
{
	DebuggerControl::ResetForTesting();
	DebuggerControl::RecordStop(MakeStop(0x12BBD0, 0x12BBD0));

	DebuggerControl::StopEvent out;
	const auto start = std::chrono::steady_clock::now();
	ASSERT_TRUE(DebuggerControl::WaitForStop(0, 5000, out));
	EXPECT_LT(std::chrono::steady_clock::now() - start, 1000ms);

	EXPECT_EQ(out.seq, 1u);
	EXPECT_EQ(out.pc, 0x12BBD0u);
	EXPECT_EQ(out.reason, DebuggerControl::StopReason::Breakpoint);
}

TEST(DebuggerControl, WaitIgnoresStopsAtOrBeforeSince)
{
	DebuggerControl::ResetForTesting();
	DebuggerControl::RecordStop(MakeStop(0x100000, 0x100000));

	DebuggerControl::StopEvent out;
	EXPECT_FALSE(DebuggerControl::WaitForStop(1, 50, out));
}

TEST(DebuggerControl, WaitWakesOnALaterStop)
{
	DebuggerControl::ResetForTesting();

	std::thread producer([] {
		std::this_thread::sleep_for(50ms);
		DebuggerControl::RecordStop(MakeStop(0x200000, 0x200000));
	});

	DebuggerControl::StopEvent out;
	ASSERT_TRUE(DebuggerControl::WaitForStop(0, 5000, out));
	EXPECT_EQ(out.pc, 0x200000u);
	producer.join();
}

TEST(DebuggerControl, WaitTimesOutWhenNothingStops)
{
	DebuggerControl::ResetForTesting();

	DebuggerControl::StopEvent out;
	EXPECT_FALSE(DebuggerControl::WaitForStop(0, 50, out));
}

TEST(DebuggerControl, CallbacksFireOnEveryStopUntilRemoved)
{
	DebuggerControl::ResetForTesting();

	int calls = 0;
	const size_t handle = DebuggerControl::AddStopCallback([&calls](const DebuggerControl::StopEvent&) { calls++; });

	DebuggerControl::RecordStop(MakeStop(0x100000, 0x100000));
	EXPECT_EQ(calls, 1);

	DebuggerControl::RemoveStopCallback(handle);
	DebuggerControl::RecordStop(MakeStop(0x100004, 0x100004));
	EXPECT_EQ(calls, 1);
}

TEST(DebuggerControl, StopReasonNamesAreStableProtocolStrings)
{
	EXPECT_STREQ(DebuggerControl::StopReasonName(DebuggerControl::StopReason::Breakpoint), "breakpoint");
	EXPECT_STREQ(DebuggerControl::StopReasonName(DebuggerControl::StopReason::MemCheck), "memcheck");
	EXPECT_STREQ(DebuggerControl::StopReasonName(DebuggerControl::StopReason::Step), "step");
	EXPECT_STREQ(DebuggerControl::StopReasonName(DebuggerControl::StopReason::UserPause), "user");
	EXPECT_STREQ(DebuggerControl::StopReasonName(DebuggerControl::StopReason::Entry), "entry");
	EXPECT_STREQ(DebuggerControl::StopReasonName(DebuggerControl::StopReason::VMShutdown), "vm_shutdown");
}
