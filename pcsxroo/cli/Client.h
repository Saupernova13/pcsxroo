// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <functional>
#include <string>

// Exit codes are part of the interface: a script has to be able to tell "no breakpoint hit
// yet" from "the emulator is gone", and both from "I typed the command wrong".
enum PcsxrooExit
{
	PCSXROO_OK = 0,
	PCSXROO_SERVER_ERROR = 1,
	PCSXROO_USAGE = 2,
	PCSXROO_NO_CONNECTION = 3,
	PCSXROO_TIMEOUT = 4
};

class PcsxrooClient
{
public:
	~PcsxrooClient();

	bool Connect(const std::string& host, int port, u32 timeout_ms, std::string& error);
	void Close();

	// Sends one request line and reads one response line.
	bool Request(const std::string& line, std::string& response, std::string& error);

	// Sends one request line, then reads lines until the connection closes or the callback
	// returns false. Used by "events".
	bool Stream(const std::string& line, const std::function<bool(const std::string&)>& on_line,
		std::string& error);

private:
	bool ReadLine(std::string& out, std::string& error);

	// SOCKET on Windows is an unsigned handle; kept as an intptr so the header needs no
	// platform includes.
	std::intptr_t m_sock = -1;
	std::string m_buffer;
	u32 m_timeout_ms = 5000;
};
