// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// PCSXROO debugger server: newline-delimited JSON over loopback TCP.
//
// The server grants unrestricted guest memory read and write plus process control, and has
// no authentication. It binds INADDR_LOOPBACK only and is off by default. It must never be
// made reachable from a network.
//
// Deliberately separate from PINE rather than an extension of it: PINE's fixed-width binary
// protocol is shared with other emulators and is a poor fit for variable-length replies such
// as disassembly, stack traces and search results. Both can run at once on different ports.

#define PCSXROO_DEBUG_SERVER_DEFAULT_PORT 28110
#define PCSXROO_DEBUG_PROTOCOL_VERSION 1

namespace DebugServer
{
	bool IsInitialized();

	// The port currently bound, or -1 when the server is not running.
	int GetPort();

	bool Initialize(int port = PCSXROO_DEBUG_SERVER_DEFAULT_PORT);
	void Deinitialize();
} // namespace DebugServer
