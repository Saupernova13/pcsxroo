// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsxroo/cli/Args.h"
#include "pcsxroo/cli/Client.h"
#include "pcsxroo/cli/Commands.h"
#include "pcsxroo/cli/Launch.h"
#include "pcsxroo/cli/Render.h"

#include <rapidjson/document.h>

#include <fmt/format.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
	int Usage(const std::string& message)
	{
		if (!message.empty())
			fmt::print(stderr, "pcsxroo: {}\n\n", message);

		PcsxrooCommands::PrintUsage();
		return PCSXROO_USAGE;
	}

	// The server bounds every command itself - a boot is allowed a minute, a loadstate thirty
	// seconds - so the socket timeout is only a backstop against a wedged emulator, and must
	// never cut off a reply the server is still legitimately producing. It used to be 5 s for
	// everything but wait and step, which cut off reset, frame-advance, boot and loadstate.
	u32 SocketTimeout(const PcsxrooCommands::Request& request, const PcsxrooArgs::Global& global)
	{
		// The CPU-thread dispatch ahead of a command's own wait, plus the round trip.
		constexpr u32 OVERHEAD_MS = 2000 + 5000;
		// Longer than the longest fixed budget on the server, which is boot's 60 s.
		constexpr u32 BACKSTOP_MS = 90000;

		if (request.command_timeout_ms != 0)
			return request.command_timeout_ms + OVERHEAD_MS;

		return global.timeout_explicit ? global.timeout_ms : BACKSTOP_MS;
	}

	// A request that got no reply in time is exit 4; one whose connection went away is exit 3,
	// because "still busy" and "the emulator is gone" call for different next steps.
	int FailureExit(const PcsxrooClient& client)
	{
		return client.LastFailure() == PcsxrooClient::Failure::Timeout ? PCSXROO_TIMEOUT : PCSXROO_NO_CONNECTION;
	}

	// Splits a reply into ok/error, printing whichever applies. Returns the process exit
	// code, so a timeout stays distinguishable from any other server error.
	int ReportResponse(const std::string& cmd, const std::string& response, bool raw)
	{
		if (raw)
		{
			fmt::print("{}\n", response);
		}

		rapidjson::Document doc;
		doc.Parse(response.c_str(), response.size());
		if (doc.HasParseError() || !doc.IsObject())
		{
			if (!raw)
				fmt::print(stderr, "pcsxroo: could not parse the reply: {}\n", response);

			return PCSXROO_SERVER_ERROR;
		}

		const auto ok = doc.FindMember("ok");
		if (ok != doc.MemberEnd() && ok->value.IsBool() && ok->value.GetBool())
		{
			const auto result = doc.FindMember("result");
			if (!raw && result != doc.MemberEnd())
				PcsxrooRender::Result(cmd, result->value);

			return PCSXROO_OK;
		}

		std::string code = "error";
		std::string message = "the emulator reported a failure";

		const auto error = doc.FindMember("error");
		if (error != doc.MemberEnd() && error->value.IsObject())
		{
			const auto code_value = error->value.FindMember("code");
			if (code_value != error->value.MemberEnd() && code_value->value.IsString())
				code = code_value->value.GetString();

			const auto message_value = error->value.FindMember("message");
			if (message_value != error->value.MemberEnd() && message_value->value.IsString())
				message = message_value->value.GetString();
		}

		if (!raw)
			fmt::print(stderr, "pcsxroo: {}: {}\n", code, message);

		return code == "timeout" ? PCSXROO_TIMEOUT : PCSXROO_SERVER_ERROR;
	}

	int RunLaunch(std::vector<std::string> argv, const PcsxrooArgs::Global& global)
	{
		PcsxrooLaunch::Options options;
		std::string error;
		if (!PcsxrooLaunch::ParseOptions(argv, global.port, options, error))
			return Usage(error);

		u64 pid = 0;
		if (!PcsxrooLaunch::Run(options, pid, error))
		{
			fmt::print(stderr, "pcsxroo: {}\n", error);
			return PCSXROO_NO_CONNECTION;
		}

		if (global.json)
			fmt::print("{{\"pid\":{},\"port\":{},\"ready\":true}}\n", pid, options.port);
		else
			fmt::print("emulator ready on port {} (pid {})\n", options.port, pid);

		return PCSXROO_OK;
	}

	int RunEvents(const PcsxrooArgs::Global& global)
	{
		PcsxrooClient client;
		std::string error;
		if (!client.Connect(global.host, global.port, global.timeout_ms, error))
		{
			fmt::print(stderr, "pcsxroo: {}\n", error);
			return PCSXROO_NO_CONNECTION;
		}

		bool first = true;
		const bool raw = global.json;

		const bool finished = client.Stream(
			R"({"id":1,"cmd":"subscribe","args":{"events":["stop"]}})",
			[&first, raw](const std::string& line) {
				if (first)
				{
					// The subscribe acknowledgement, not an event.
					first = false;
					return true;
				}

				if (raw)
				{
					fmt::print("{}\n", line);
				}
				else
				{
					rapidjson::Document doc;
					doc.Parse(line.c_str(), line.size());
					if (!doc.HasParseError() && doc.IsObject())
						PcsxrooRender::Event(doc);
				}

				// Flushed per line so a supervising agent reading the pipe sees events as
				// they happen rather than in block-sized bursts.
				std::fflush(stdout);
				return true;
			},
			error);

		if (!finished)
		{
			fmt::print(stderr, "pcsxroo: {}\n", error);
			return FailureExit(client);
		}

		return PCSXROO_OK;
	}
} // namespace

int main(int argc, char* argv[])
{
	std::vector<std::string> args(argv + 1, argv + argc);

	if (args.empty())
		return Usage({});

	// Asked-for help is a success, and is honoured wherever it appears: "pcsxroo bp add --help"
	// used to try to set a breakpoint at "--help".
	if (args[0] == "help" || std::find_if(args.begin(), args.end(), [](const std::string& arg) {
			return arg == "--help" || arg == "-h";
		}) != args.end())
	{
		PcsxrooCommands::PrintUsage();
		return PCSXROO_OK;
	}

	PcsxrooArgs::Global global;
	std::string error;
	if (!PcsxrooArgs::ParseGlobal(args, global, error))
		return Usage(error);

	if (args.empty())
		return Usage("no command given");

	if (args[0] == "launch")
	{
		args.erase(args.begin());
		return RunLaunch(std::move(args), global);
	}

	if (args[0] == "events")
	{
		if (args.size() > 1)
			return Usage("events takes no arguments; unexpected " + args[1]);

		return RunEvents(global);
	}

	PcsxrooCommands::Request request;
	if (!PcsxrooCommands::Build(args, global, request, error))
		return Usage(error);

	PcsxrooClient client;
	if (!client.Connect(global.host, global.port, SocketTimeout(request, global), error))
	{
		fmt::print(stderr, "pcsxroo: {}\n", error);
		return PCSXROO_NO_CONNECTION;
	}

	std::string response;
	if (!client.Request(request.json, response, error))
	{
		fmt::print(stderr, "pcsxroo: {}\n", error);
		return FailureExit(client);
	}

	return ReportResponse(request.cmd, response, global.json);
}
