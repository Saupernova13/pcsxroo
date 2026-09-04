// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "pcsxroo-cli/Args.h"

#include <string>
#include <vector>

namespace PcsxrooCommands
{
	struct Request
	{
		std::string cmd;   // protocol command name
		std::string json;  // the complete request line
	};

	// Translates CLI syntax into a protocol request. argv has already had the global flags
	// removed. Returns false with a usage message in error.
	bool Build(std::vector<std::string> argv, const PcsxrooArgs::Global& global, Request& out,
		std::string& error);

	void PrintUsage();
} // namespace PcsxrooCommands
