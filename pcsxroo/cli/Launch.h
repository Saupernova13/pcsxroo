// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <string>
#include <vector>

namespace PcsxrooLaunch
{
	constexpr u32 MAX_READY_TIMEOUT_MS = 60 * 60 * 1000;

	struct Options
	{
		std::string game;          // iso or elf; empty for --bios, or for an idle emulator
		std::string emulator;      // explicit path, otherwise found beside this executable
		bool pause_on_entry = false;
		bool boot_bios = false;   // --bios: boot the PS2 BIOS with no disc
		u32 ready_timeout_ms = 60000;
		int port = 28110;
	};

	// Spawns the emulator detached and polls the debug server port until it answers.
	// Returns the process id in pid, or false with a reason in error - including when the
	// emulator exits before its server ever answers.
	bool Run(const Options& options, u64& pid, std::string& error);

	// Parses launch-specific arguments out of argv. Anything it does not recognise is an
	// error rather than a guess: a mistyped flag used to be taken as the game path.
	bool ParseOptions(std::vector<std::string>& argv, int port, Options& out, std::string& error);
} // namespace PcsxrooLaunch
