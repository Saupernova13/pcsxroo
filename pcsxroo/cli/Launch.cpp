// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsxroo/cli/Launch.h"

#include "pcsxroo/cli/Args.h"
#include "pcsxroo/cli/Client.h"

#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"

#include <fmt/format.h>

#include <chrono>
#include <thread>

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace
{
	// The emulator is expected beside this executable, which is how an installed PCSXROO is
	// laid out. pcsx2-qt is accepted too: it is what a build without the fork's CMake rename
	// (an MSBuild one, say) produces, and the server inside it is the same.
	std::string FindEmulator(std::string& tried)
	{
		const std::string dir(Path::GetDirectory(FileSystem::GetProgramPath()));

#ifdef _WIN32
		static constexpr const char* names[] = {"pcsxroo-qt.exe", "pcsx2-qt.exe"};
#else
		static constexpr const char* names[] = {"pcsxroo-qt", "pcsx2-qt"};
#endif

		for (const char* name : names)
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

	std::vector<std::string> EmulatorArguments(const std::string& emulator, const PcsxrooLaunch::Options& options)
	{
		std::vector<std::string> args{emulator, "-debugserver", std::to_string(options.port)};
		if (options.pause_on_entry)
			args.push_back("-pauseonentry");

		// With no game, the emulator comes up idle with the server listening and no VM. That is
		// deliberately not the same as booting the BIOS: starting the emulator and starting a
		// VM fail for different reasons, and an agent that cannot tell them apart is stuck.
		if (options.boot_bios)
		{
			args.push_back("-bios");
		}
		else if (!options.game.empty())
		{
			args.push_back("-batch");
			args.push_back(options.game);
		}

		return args;
	}

#ifdef _WIN32
	// Quotes one argument so that CommandLineToArgvW, which is how the emulator splits its
	// command line, hands it back unchanged: backslashes are literal except before a quote.
	std::string QuoteArgument(const std::string& arg)
	{
		std::string out = "\"";
		size_t backslashes = 0;
		for (const char c : arg)
		{
			if (c == '\\')
			{
				backslashes++;
				continue;
			}

			out.append(c == '"' ? backslashes * 2 + 1 : backslashes, '\\');
			backslashes = 0;
			out += c;
		}

		out.append(backslashes * 2, '\\');
		out += '"';
		return out;
	}
#endif
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
		if (!PcsxrooArgs::ParseNumber(value, timeout) || timeout == 0 || timeout > MAX_READY_TIMEOUT_MS)
		{
			error = fmt::format("--ready-timeout must be between 1 and {} ms", MAX_READY_TIMEOUT_MS);
			return false;
		}

		out.ready_timeout_ms = static_cast<u32>(timeout);
	}
	else if (!error.empty())
	{
		return false;
	}

	for (const std::string& argument : argv)
	{
		if (argument.size() > 1 && argument[0] == '-' && argument[1] == '-')
		{
			error = "unknown launch option: " + argument;
			return false;
		}
	}

	if (argv.size() > 1)
	{
		error = "launch takes at most one game path; unexpected " + argv[1];
		return false;
	}

	if (!argv.empty())
		out.game = argv[0];

	if (out.boot_bios && !out.game.empty())
	{
		error = "pass either a game path or --bios, not both";
		return false;
	}

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

	std::vector<std::string> args = EmulatorArguments(emulator, options);

#ifdef _WIN32
	std::string command_line;
	for (const std::string& arg : args)
	{
		if (!command_line.empty())
			command_line += ' ';

		command_line += QuoteArgument(arg);
	}

	STARTUPINFOW startup = {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process = {};

	std::wstring wide = StringUtil::UTF8StringToWideString(command_line);
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

	// Kept open only to notice the emulator exiting before its server answers.
	const auto exited = [&process](std::string& why) {
		if (WaitForSingleObject(process.hProcess, 0) != WAIT_OBJECT_0)
			return false;

		DWORD code = 0;
		GetExitCodeProcess(process.hProcess, &code);
		why = fmt::format("the emulator exited (code {}) before its debug server answered", code);
		return true;
	};
	const auto release = [&process]() { CloseHandle(process.hProcess); };
#else
	std::vector<char*> c_args;
	for (std::string& arg : args)
		c_args.push_back(arg.data());
	c_args.push_back(nullptr);

	const pid_t child = fork();
	if (child < 0)
	{
		error = fmt::format("could not start {}: {}", emulator, std::strerror(errno));
		return false;
	}

	if (child == 0)
	{
		// A session of its own, so closing the terminal that ran pcsxroo does not take the
		// emulator with it; and stdio pointed away from this process's pipes, so a caller
		// reading launch's output to the end is not held open for the emulator's lifetime.
		setsid();
		const int devnull = open("/dev/null", O_RDWR);
		if (devnull >= 0)
		{
			dup2(devnull, STDIN_FILENO);
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
			if (devnull > STDERR_FILENO)
				close(devnull);
		}

		execv(c_args[0], c_args.data());
		_exit(127);
	}

	pid = static_cast<u64>(child);

	const auto exited = [child](std::string& why) {
		int status = 0;
		if (waitpid(child, &status, WNOHANG) != child)
			return false;

		if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
			why = "the emulator could not be executed";
		else if (WIFEXITED(status))
			why = fmt::format("the emulator exited (code {}) before its debug server answered", WEXITSTATUS(status));
		else
			why = "the emulator was killed before its debug server answered";

		return true;
	};
	const auto release = []() {};
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
			{
				release();
				return true;
			}
		}

		// Checked every tick, so a crash at startup is reported in a moment rather than after
		// the whole ready timeout.
		if (exited(error))
		{
			release();
			return false;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	release();
	error = fmt::format("the emulator did not answer on port {} within {} ms", options.port,
		options.ready_timeout_ms);
	return false;
}
