// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <string>
#include <string_view>
#include <vector>

namespace PcsxrooArgs
{
	struct Global
	{
		std::string host = "127.0.0.1";
		int port = 28110;
		std::string cpu = "ee";
		bool json = false;
		u32 timeout_ms = 5000;
		// True once --timeout was given, so per-command defaults know not to override it.
		bool timeout_explicit = false;
	};

	// Removes recognised global flags from argv in place and leaves everything else
	// untouched, so a command's own arguments can appear before or after them.
	// PCSXROO_PORT overrides the default port; an explicit --port still wins.
	bool ParseGlobal(std::vector<std::string>& argv, Global& out, std::string& error);

	// Accepts "0x" hex and decimal only. Expressions are passed through to the server,
	// which has the parser and the CPU state needed to resolve them.
	bool ParseNumber(std::string_view text, u64& out);

	// Reads a named "--flag value" pair out of argv, removing both. Returns false when the
	// flag is absent; sets error when it is present without a value.
	bool TakeOption(std::vector<std::string>& argv, std::string_view name, std::string& value,
		std::string& error);

	// Reads a valueless "--flag", removing it.
	bool TakeFlag(std::vector<std::string>& argv, std::string_view name);
} // namespace PcsxrooArgs
