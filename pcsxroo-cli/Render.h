// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <rapidjson/document.h>

#include <string>

namespace PcsxrooRender
{
	// Renders a successful result for a person to read. cmd selects a shape-specific
	// renderer; anything unrecognised falls back to a generic key/value dump, so a new
	// server command is readable before it has a renderer of its own.
	void Result(const std::string& cmd, const rapidjson::Value& result);

	// One streamed event line.
	void Event(const rapidjson::Value& event);
} // namespace PcsxrooRender
