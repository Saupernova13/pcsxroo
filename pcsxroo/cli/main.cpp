// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsxroo/cli/Args.h"
#include "pcsxroo/cli/Client.h"
#include "pcsxroo/cli/Commands.h"
#include "pcsxroo/cli/Launch.h"
#include "pcsxroo/cli/Render.h"

#include <rapidjson/document.h>

#include <fmt/format.h>

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

	if (args.empty() || args[0] == "--help" || args[0] == "-h" || args[0] == "help")
		return Usage({});

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
		return RunEvents(global);

	PcsxrooCommands::Request request;
	if (!PcsxrooCommands::Build(args, global, request, error))
		return Usage(error);

	PcsxrooClient client;
	// wait and step can legitimately block far longer than a normal request, so the socket
	// timeout follows the command's own timeout with headroom for the round trip.
	const u32 socket_timeout =
		(request.cmd == "wait" || request.cmd == "step") ? (global.timeout_explicit ? global.timeout_ms : 60000) + 5000
														 : global.timeout_ms;

	if (!client.Connect(global.host, global.port, socket_timeout, error))
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
