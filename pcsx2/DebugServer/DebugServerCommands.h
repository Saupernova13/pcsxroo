// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "DebugServer/DebugServerJson.h"
#include "DebugTools/DebuggerControl.h"

#include <functional>
#include <string>

// What a handler is allowed to do to the connection that issued the request.
//
// Only "subscribe" needs this, but passing it to every handler keeps the signature uniform
// and stops the command table needing to know which commands are connection-scoped.
class DebugServerConnection
{
public:
	virtual ~DebugServerConnection() = default;

	// After this, stop events are pushed to this connection as they happen.
	virtual void SetSubscribed(bool subscribed) = 0;
};

// Returns a complete response line with no trailing newline; framing is the writer's job.
using DebugServerHandler = std::function<std::string(const DebugServerRequest&, DebugServerConnection&)>;

namespace DebugServerCommands
{
	// Idempotent; safe to call on every server start.
	void RegisterAll();

	const DebugServerHandler* Find(const std::string& name);

	// The wire shape of a stop, shared by wait/step/pause results and the event stream, so
	// a client parses one representation rather than two.
	void WriteStopEvent(const DebuggerControl::StopEvent& stop, rapidjson::Value& out,
		rapidjson::Document::AllocatorType& allocator);

	// A complete pushed event line, carrying "event":"stop" and no id.
	std::string MakeStopEventLine(const DebuggerControl::StopEvent& stop);

	// Re-asserts CLI-injected pad state. Called once per frame from the CPU thread, after
	// InputManager::PollSources, because that call overwrites the pad from the real
	// controllers - a single write would be discarded before the game ever saw it.
	void ApplyHeldInputs();

	// Drops all injected pad state; called when a VM goes away so input cannot leak into
	// the next one.
	void ClearHeldInputs();
} // namespace DebugServerCommands
