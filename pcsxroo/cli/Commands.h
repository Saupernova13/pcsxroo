// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "pcsxroo/cli/Args.h"

#include <string>
#include <vector>

namespace PcsxrooCommands
{
	struct Request
	{
		std::string cmd;   // protocol command name
		std::string json;  // the complete request line

		// For commands that block on the server for a time of their own (wait, step, pause,
		// frame-advance): that time, which the client's socket timeout has to outlast. 0 for
		// every other command.
		u32 command_timeout_ms = 0;
	};

	// Translates CLI syntax into a protocol request. argv has already had the global flags
	// removed. Returns false with a usage message in error; an option or argument the command
	// does not take is an error, never silently ignored.
	bool Build(std::vector<std::string> argv, const PcsxrooArgs::Global& global, Request& out,
		std::string& error);

	void PrintUsage();
} // namespace PcsxrooCommands
