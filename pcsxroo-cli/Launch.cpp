// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsxroo-cli/Launch.h"

#include "pcsxroo-cli/Args.h"
#include "pcsxroo-cli/Client.h"

#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"

#include <fmt/format.h>

#include <chrono>
#include <cstdlib>
#include <thread>

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#endif

namespace
{
	// The emulator is expected beside this executable, which is how an installed PCSXROO is
	// laid out. Both names are tried so the CLI keeps working before the rebrand lands.
	std::string FindEmulator(std::string& tried)
	{
		const std::string dir(Path::GetDirectory(FileSystem::GetProgramPath()));

		for (const char* name : {"pcsxroo-qt.exe", "pcsx2-qt.exe", "pcsxroo-qt", "pcsx2-qt"})
		{
			const std::string candidate = Path::Combine(dir, name);
			if (FileSystem::FileExists(candidate.c_str()))
				return candidate;

			if (!tried.empty())
				tried += ", ";

			tried += candidate;
		}

		return {};
	}
} // namespace

bool PcsxrooLaunch::ParseOptions(std::vector<std::string>& argv, int port, Options& out, std::string& error)
{
	out.port = port;
	out.pause_on_entry = PcsxrooArgs::TakeFlag(argv, "--pause-on-entry");
	out.boot_bios = PcsxrooArgs::TakeFlag(argv, "--bios");

	std::string value;
	if (PcsxrooArgs::TakeOption(argv, "--emulator", value, error))
		out.emulator = value;
	else if (!error.empty())
		return false;

	if (PcsxrooArgs::TakeOption(argv, "--ready-timeout", value, error))
	{
		u64 timeout = 0;
		if (!PcsxrooArgs::ParseNumber(value, timeout))
		{
			error = "invalid --ready-timeout";
			return false;
		}

		out.ready_timeout_ms = static_cast<u32>(timeout);
	}
	else if (!error.empty())
	{
		return false;
	}

	if (!argv.empty())
		out.game = argv[0];

	return true;
}

bool PcsxrooLaunch::Run(const Options& options, u64& pid, std::string& error)
{
	std::string emulator = options.emulator;
	if (emulator.empty())
	{
		std::string tried;
		emulator = FindEmulator(tried);
		if (emulator.empty())
		{
			error = "could not find the emulator; looked for " + tried + ". Pass --emulator PATH.";
			return false;
		}
	}

	if (!FileSystem::FileExists(emulator.c_str()))
	{
		error = "no such emulator: " + emulator;
		return false;
	}

	std::string arguments = fmt::format("\"{}\" -debugserver {}", emulator, options.port);
	if (options.pause_on_entry)
		arguments += " -pauseonentry";

	// With no game, the emulator comes up idle with the server listening and no VM. That is
	// deliberately not the same as booting the BIOS: starting the emulator and starting a VM
	// fail for different reasons, and an agent that cannot tell them apart is stuck.
	if (options.boot_bios)
		arguments += " -bios";
	else if (!options.game.empty())
		arguments += fmt::format(" -batch \"{}\"", options.game);

#ifdef _WIN32
	STARTUPINFOW startup = {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process = {};

	std::wstring wide = StringUtil::UTF8StringToWideString(arguments);
	// DETACHED_PROCESS so closing the shell that ran pcsxroo does not take the emulator
	// with it - an unattended session outlives the terminal that started it.
	if (!CreateProcessW(nullptr, wide.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, nullptr,
			&startup, &process))
	{
		error = fmt::format("could not start {} (error {})", emulator, GetLastError());
		return false;
	}

	pid = process.dwProcessId;
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
#else
	arguments += " &";
	if (std::system(arguments.c_str()) != 0)
	{
		error = "could not start " + emulator;
		return false;
	}

	pid = 0;
#endif

	// Polling is the right tool here: there is no other signal that the server is up.
	const auto deadline =
		std::chrono::steady_clock::now() + std::chrono::milliseconds(options.ready_timeout_ms);

	while (std::chrono::steady_clock::now() < deadline)
	{
		PcsxrooClient client;
		std::string connect_error;
		if (client.Connect("127.0.0.1", options.port, 1000, connect_error))
		{
			std::string response;
			std::string request_error;
			if (client.Request(R"({"id":1,"cmd":"version"})", response, request_error))
				return true;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	error = fmt::format("the emulator did not answer on port {} within {} ms", options.port,
		options.ready_timeout_ms);
	return false;
}
