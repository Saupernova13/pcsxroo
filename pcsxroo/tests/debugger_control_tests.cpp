// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsxroo/server/DebuggerControl.h"

#include "DebugTools/MIPSAnalyst.h"

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

	MIPSAnalyst::MipsOpcodeInfo MakeInfo()
	{
		MIPSAnalyst::MipsOpcodeInfo info{};
		info.cpu = nullptr;
		return info;
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

// The debug server cancels waits while it shuts down, so a client blocked in one cannot keep
// its reader thread, and the join on it, busy until the wait times out.
TEST(DebuggerControl, CancelledWaitsReturnAtOnce)
{
	DebuggerControl::ResetForTesting();

	std::thread canceller([] {
		std::this_thread::sleep_for(50ms);
		DebuggerControl::SetWaitsCancelled(true);
	});

	DebuggerControl::StopEvent out;
	const auto start = std::chrono::steady_clock::now();
	EXPECT_FALSE(DebuggerControl::WaitForStop(0, 5000, out));
	EXPECT_LT(std::chrono::steady_clock::now() - start, 2000ms);
	canceller.join();

	EXPECT_FALSE(DebuggerControl::WaitForStop(0, 5000, out)) << "a wait started while cancelled returns at once";

	DebuggerControl::SetWaitsCancelled(false);
	DebuggerControl::RecordStop(MakeStop(0x100000, 0x100000));
	EXPECT_TRUE(DebuggerControl::WaitForStop(0, 1000, out)) << "and once uncancelled, waits work again";
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

TEST(DebuggerControlStep, PlainInstructionAdvancesOneWord)
{
	MIPSAnalyst::MipsOpcodeInfo info = MakeInfo();
	EXPECT_EQ(DebuggerControl::ComputeStepTarget(DebuggerControl::StepMode::Into, 0x100000, info), 0x100004u);
	EXPECT_EQ(DebuggerControl::ComputeStepTarget(DebuggerControl::StepMode::Over, 0x100000, info), 0x100004u);
}

TEST(DebuggerControlStep, UnconditionalBranchGoesToTheTarget)
{
	MIPSAnalyst::MipsOpcodeInfo info = MakeInfo();
	info.isBranch = true;
	info.isConditional = false;
	info.branchTarget = 0x200000;

	EXPECT_EQ(DebuggerControl::ComputeStepTarget(DebuggerControl::StepMode::Into, 0x100000, info), 0x200000u);
	EXPECT_EQ(DebuggerControl::ComputeStepTarget(DebuggerControl::StepMode::Over, 0x100000, info), 0x200000u);
}

// Step-over must not follow a call: it lands after the delay slot instead.
TEST(DebuggerControlStep, StepOverSkipsALinkedBranchAndItsDelaySlot)
{
	MIPSAnalyst::MipsOpcodeInfo info = MakeInfo();
	info.isBranch = true;
	info.isConditional = false;
	info.isLinkedBranch = true;
	info.branchTarget = 0x200000;

	EXPECT_EQ(DebuggerControl::ComputeStepTarget(DebuggerControl::StepMode::Over, 0x100000, info), 0x100008u);
	// Step-into still follows the call.
	EXPECT_EQ(DebuggerControl::ComputeStepTarget(DebuggerControl::StepMode::Into, 0x100000, info), 0x200000u);
}

TEST(DebuggerControlStep, ConditionalBranchFollowsWhetherTheConditionIsMet)
{
	MIPSAnalyst::MipsOpcodeInfo taken = MakeInfo();
	taken.isBranch = true;
	taken.isConditional = true;
	taken.conditionMet = true;
	taken.branchTarget = 0x200000;
	EXPECT_EQ(DebuggerControl::ComputeStepTarget(DebuggerControl::StepMode::Into, 0x100000, taken), 0x200000u);

	MIPSAnalyst::MipsOpcodeInfo not_taken = taken;
	not_taken.conditionMet = false;
	// Skips the branch and its delay slot.
	EXPECT_EQ(DebuggerControl::ComputeStepTarget(DebuggerControl::StepMode::Into, 0x100000, not_taken), 0x100008u);
}

TEST(DebuggerControlStep, SyscallIsAlwaysTakenOnStepInto)
{
	MIPSAnalyst::MipsOpcodeInfo info = MakeInfo();
	info.isSyscall = true;
	info.branchTarget = 0x80000180;

	EXPECT_EQ(DebuggerControl::ComputeStepTarget(DebuggerControl::StepMode::Into, 0x100000, info), 0x80000180u);
	// Step-over does not enter the exception handler.
	EXPECT_EQ(DebuggerControl::ComputeStepTarget(DebuggerControl::StepMode::Over, 0x100000, info), 0x100004u);
}
