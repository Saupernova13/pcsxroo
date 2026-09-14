// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsxroo/cli/Args.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>

namespace
{
	bool ParseInt(const std::string& text, int& out)
	{
		const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
		return result.ec == std::errc() && result.ptr == text.data() + text.size();
	}
} // namespace

bool PcsxrooArgs::TakeOption(std::vector<std::string>& argv, std::string_view name, std::string& value,
	std::string& error)
{
	for (size_t i = 0; i < argv.size(); i++)
	{
		if (argv[i] != name)
			continue;

		if (i + 1 >= argv.size())
		{
			error = std::string(name) + " needs a value";
			return false;
		}

		value = argv[i + 1];
		argv.erase(argv.begin() + i, argv.begin() + i + 2);
		return true;
	}

	return false;
}

bool PcsxrooArgs::TakeFlag(std::vector<std::string>& argv, std::string_view name)
{
	const auto it = std::find(argv.begin(), argv.end(), name);
	if (it == argv.end())
		return false;

	argv.erase(it);
	return true;
}

bool PcsxrooArgs::ParseGlobal(std::vector<std::string>& argv, Global& out, std::string& error)
{
	if (const char* env_port = std::getenv("PCSXROO_PORT"))
	{
		const std::string text(env_port);
		if (!ParseInt(text, out.port) || out.port <= 0 || out.port > 65535)
		{
			error = "PCSXROO_PORT is not a valid port: " + text;
			return false;
		}
	}

	std::string value;

	if (TakeOption(argv, "--host", value, error))
		out.host = value;
	else if (!error.empty())
		return false;

	if (TakeOption(argv, "--port", value, error))
	{
		if (!ParseInt(value, out.port) || out.port <= 0 || out.port > 65535)
		{
			error = "invalid port: " + value;
			return false;
		}
	}
	else if (!error.empty())
	{
		return false;
	}

	if (TakeOption(argv, "--cpu", value, error))
	{
		if (value != "ee" && value != "iop")
		{
			error = "cpu must be ee or iop";
			return false;
		}

		out.cpu = value;
	}
	else if (!error.empty())
	{
		return false;
	}

	if (TakeOption(argv, "--timeout", value, error))
	{
		int timeout = 0;
		// 0 used to be accepted, and meant "do not wait at all" to the server but "wait
		// forever" to the socket.
		if (!ParseInt(value, timeout) || timeout <= 0)
		{
			error = "invalid timeout (must be at least 1 ms): " + value;
			return false;
		}

		out.timeout_ms = static_cast<u32>(timeout);
		out.timeout_explicit = true;
	}
	else if (!error.empty())
	{
		return false;
	}

	out.json = TakeFlag(argv, "--json");
	return true;
}

bool PcsxrooArgs::ParseNumber(std::string_view text, u64& out)
{
	if (text.empty())
		return false;

	int base = 10;
	if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
	{
		text.remove_prefix(2);
		base = 16;
	}

	const auto result = std::from_chars(text.data(), text.data() + text.size(), out, base);
	return result.ec == std::errc() && result.ptr == text.data() + text.size();
}
