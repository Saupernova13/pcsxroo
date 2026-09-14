// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsxroo/cli/Commands.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <fmt/format.h>

#include <cmath>
#include <cstdlib>
#include <limits>

namespace
{
	using Allocator = rapidjson::Document::AllocatorType;

	// Server-side defaults for the commands that wait, mirrored so the client knows how long
	// its socket must stay open when --timeout is not given.
	constexpr u32 DEFAULT_WAIT_MS = 60000;
	constexpr u32 DEFAULT_STEP_MS = 5000;
	constexpr u32 DEFAULT_PAUSE_MS = 2000;
	constexpr u32 DEFAULT_FRAME_ADVANCE_MS = 10000;

	constexpr size_t ANY_NUMBER = std::numeric_limits<size_t>::max();

	class Builder
	{
	public:
		explicit Builder(std::string cmd)
			: m_cmd(std::move(cmd))
		{
			m_doc.SetObject();
			m_args.SetObject();
		}

		void Str(const char* name, const std::string& value)
		{
			m_args.AddMember(rapidjson::Value(name, Alloc()),
				rapidjson::Value(value.c_str(), static_cast<rapidjson::SizeType>(value.size()), Alloc()), Alloc());
		}

		void Num(const char* name, u64 value)
		{
			m_args.AddMember(rapidjson::Value(name, Alloc()), rapidjson::Value(value), Alloc());
		}

		void Int(const char* name, s64 value)
		{
			m_args.AddMember(rapidjson::Value(name, Alloc()), rapidjson::Value(value), Alloc());
		}

		void Real(const char* name, double value)
		{
			m_args.AddMember(rapidjson::Value(name, Alloc()), rapidjson::Value(value), Alloc());
		}

		void Bool(const char* name, bool value)
		{
			m_args.AddMember(rapidjson::Value(name, Alloc()), rapidjson::Value(value), Alloc());
		}

		void StrArray(const char* name, const std::vector<std::string>& values)
		{
			rapidjson::Value array(rapidjson::kArrayType);
			for (const std::string& value : values)
			{
				array.PushBack(
					rapidjson::Value(value.c_str(), static_cast<rapidjson::SizeType>(value.size()), Alloc()),
					Alloc());
			}

			m_args.AddMember(rapidjson::Value(name, Alloc()), array, Alloc());
		}

		// An address argument is passed through as a string so the server resolves literals
		// and expressions with one set of rules, rather than the CLI guessing.
		void Address(const char* name, const std::string& text) { Str(name, text); }

		// analog: { "<stick>": { "x": .., "y": .. } }, merged so both sticks can be set.
		void Stick(const char* stick, float x, float y)
		{
			if (!m_args.HasMember("analog"))
				m_args.AddMember("analog", rapidjson::Value(rapidjson::kObjectType), Alloc());

			rapidjson::Value axes(rapidjson::kObjectType);
			axes.AddMember("x", rapidjson::Value(x), Alloc());
			axes.AddMember("y", rapidjson::Value(y), Alloc());
			m_args["analog"].AddMember(rapidjson::Value(stick, Alloc()), axes, Alloc());
		}

		std::string Finish()
		{
			m_doc.AddMember("id", 1, Alloc());
			m_doc.AddMember("cmd",
				rapidjson::Value(m_cmd.c_str(), static_cast<rapidjson::SizeType>(m_cmd.size()), Alloc()), Alloc());
			m_doc.AddMember("args", m_args, Alloc());

			rapidjson::StringBuffer buffer;
			rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
			m_doc.Accept(writer);
			return std::string(buffer.GetString(), buffer.GetSize());
		}

		const std::string& Cmd() const { return m_cmd; }

	private:
		Allocator& Alloc() { return m_doc.GetAllocator(); }

		std::string m_cmd;
		rapidjson::Document m_doc;
		rapidjson::Value m_args;
	};

	bool Need(const std::vector<std::string>& argv, size_t index, const char* what, std::string& error)
	{
		if (index < argv.size())
			return true;

		error = std::string("missing ") + what;
		return false;
	}

	bool IsOption(const std::string& argument)
	{
		return argument.size() > 2 && argument[0] == '-' && argument[1] == '-';
	}

	// Whatever a command did not consume is an error. A mistyped option used to fall through
	// as a positional argument, so "bp add --condd x 0x100" set a breakpoint at "--condd".
	bool NoLeftovers(const std::vector<std::string>& argv, size_t max_positionals, std::string& error)
	{
		for (const std::string& argument : argv)
		{
			if (IsOption(argument))
			{
				error = "unexpected option: " + argument;
				return false;
			}
		}

		if (argv.size() > max_positionals)
		{
			error = "unexpected argument: " + argv[max_positionals];
			return false;
		}

		return true;
	}

	// Reads "--name N". Returns false only on an error; present says whether it was given.
	bool TakeNumber(std::vector<std::string>& argv, const char* name, u64& out, bool& present, std::string& error)
	{
		std::string value;
		present = PcsxrooArgs::TakeOption(argv, name, value, error);
		if (!present)
			return error.empty();

		if (!PcsxrooArgs::ParseNumber(value, out))
		{
			error = std::string("invalid ") + name + ": " + value;
			return false;
		}

		return true;
	}

	bool TakeString(std::vector<std::string>& argv, const char* name, std::string& out, bool& present,
		std::string& error)
	{
		present = PcsxrooArgs::TakeOption(argv, name, out, error);
		return present || error.empty();
	}

	// One stick axis, which the server takes in -1..1.
	bool ParseAxis(const std::string& text, float& out)
	{
		if (text.empty())
			return false;

		char* end = nullptr;
		out = std::strtof(text.c_str(), &end);
		return end == text.c_str() + text.size() && out >= -1.0f && out <= 1.0f;
	}

	// A memory search value, in any form the server accepts: an unsigned or negative integer,
	// or a real number for the float types. Only unsigned integers used to get through, so
	// "--type f32 --eq 1.5" and "--type i32 --eq -1" could not be expressed at all.
	bool AddSearchValue(Builder& builder, const std::string& text, std::string& error)
	{
		u64 magnitude = 0;
		if (PcsxrooArgs::ParseNumber(text, magnitude))
		{
			builder.Num("value", magnitude);
			return true;
		}

		if (text.size() > 1 && text[0] == '-' && PcsxrooArgs::ParseNumber(std::string_view(text).substr(1), magnitude) &&
			magnitude <= (u64{1} << 63))
		{
			builder.Int("value", static_cast<s64>(~magnitude + 1));
			return true;
		}

		char* end = nullptr;
		const double real = std::strtod(text.c_str(), &end);
		if (!text.empty() && end == text.c_str() + text.size() && std::isfinite(real))
		{
			builder.Real("value", real);
			return true;
		}

		error = "invalid search value: " + text;
		return false;
	}
} // namespace

void PcsxrooCommands::PrintUsage()
{
	fmt::print(
		"pcsxroo - drive the PCSXROO debugger from the command line\n"
		"\n"
		"usage: pcsxroo [global flags] <group> <verb> [arguments]\n"
		"\n"
		"global flags:\n"
		"  --port N          debug server port (default 28110, or $PCSXROO_PORT)\n"
		"  --host H          default 127.0.0.1\n"
		"  --cpu ee|iop      which CPU to address (default ee)\n"
		"  --json            print the raw JSON reply instead of a rendered one\n"
		"  --timeout MS      wait, step, pause and frame-advance: the command's own time limit\n"
		"                    (defaults 60000, 5000, 2000, 10000). Any other command: give up\n"
		"                    after MS with no reply. Without it the server's own limits apply.\n"
		"  --help, -h        this text\n"
		"\n"
		"starting up:\n"
		"  launch [game] [--bios] [--pause-on-entry] [--emulator PATH]\n"
		"                                   [--ready-timeout MS]\n"
		"                                   start the emulator; with no game it comes up\n"
		"                                   idle with the server listening and no VM\n"
		"  boot <game>                      boot a game in an emulator already running\n"
		"  boot --bios                      boot the PS2 BIOS with no disc\n"
		"  boot --elf PATH                  boot an ELF directly\n"
		"      all accept [--pause-on-entry] [--fast-boot]\n"
		"\n"
		"session:\n"
		"  version                          emulator, PCSX2 base revision, protocol\n"
		"  status                           VM state, PCs, game identity, last stop\n"
		"  wait [--since N]                 block until a stop newer than N\n"
		"  events                           stream stop events until interrupted\n"
		"\n"
		"execution:\n"
		"  run | resume                     unpause (resume is an alias for run)\n"
		"  pause | reset | shutdown\n"
		"  step into|over|out\n"
		"  run-to <addr>                    temporary breakpoint, then resume\n"
		"  frame-advance [count]            advance N frames and pause again\n"
		"\n"
		"input (drives the pad, so an agent can play the game):\n"
		"  input list [--pad N]             button names this controller accepts\n"
		"  input press <button...> [--pad N] [--frames N]\n"
		"                                   tap; default 2 frames\n"
		"  input set <button...> [--left-stick X,Y] [--right-stick X,Y] [--pad N]\n"
		"                                   hold until changed; stick axes run -1..1\n"
		"  input set                        with no buttons, releases that pad\n"
		"  input release                    release everything on every pad\n"
		"\n"
		"breakpoints:\n"
		"  bp add <addr> [--cond EXPR] [--desc TEXT] [--temporary] [--disabled]\n"
		"  bp remove <addr> | bp enable <addr> | bp disable <addr>\n"
		"  bp list [--include-temp] | bp clear\n"
		"\n"
		"memchecks (watchpoints):\n"
		"  mc add <start> <end> --on read,write,change [--log] [--cond EXPR] [--desc TEXT]\n"
		"  mc remove <start> <end> | mc list | mc clear\n"
		"\n"
		"registers (need a paused VM):\n"
		"  reg list                         every category and the names in it\n"
		"  reg get <name> | reg set <name> <value>\n"
		"  reg dump [--category NAME]       NAME is GPR, CP0, FPR, FCR, VU0f, VU0i or GS\n"
		"\n"
		"memory:\n"
		"  mem read <addr> [size] [--base64]\n"
		"  mem write <addr> <hex> [--base64]\n"
		"  mem fill <addr> <size> <pattern-hex>\n"
		"  mem dump <addr> <size> <path>    written server side, for large ranges\n"
		"  mem search --range START:END [--type u32] <comparison> [--max N]\n"
		"  mem search --session N <comparison>       narrow a previous result set\n"
		"      types:       u8 u16 u32 u64 i8 i16 i32 i64 f32 f64\n"
		"      comparisons: --eq V --ne V --gt V --gte V --lt V --lte V\n"
		"                   --increased-by V --decreased-by V --changed-by V\n"
		"                   --increased --decreased --changed --not-changed --unknown\n"
		"      V is 0x hex, decimal, a negative number, or a real number for f32 and f64\n"
		"\n"
		"code and symbols:\n"
		"  dis <addr> [count] [--raw]       disassemble; --raw skips simplification\n"
		"  asm <addr> \"<instruction>\"...    assemble and write in place\n"
		"  sym lookup <addr> | sym find <name>\n"
		"\n"
		"context:\n"
		"  stack                            call stack (needs a paused VM)\n"
		"  threads | modules\n"
		"  eval \"<expression>\"             registers have no $ here: \"a0 == 2\"\n"
		"\n"
		"state and observation:\n"
		"  savestate --slot N | --path P [--wait-flush]\n"
		"  loadstate --slot N | --path P\n"
		"  screenshot <path>                needs the VM running, not paused\n"
		"  patch reload                     re-read pnach files\n"
		"\n"
		"addresses accept 0x12BBD0, bare hex 12BBD0, decimal, or an expression such as\n"
		"main+0x40 or [0x1B1F038]. Quote an expression that contains spaces.\n"
		"\n"
		"exit codes: 0 ok, 1 server error, 2 usage, 3 cannot connect or connection lost,\n"
		"4 timed out. 3 and 4 differ on purpose: \"no breakpoint yet\" is not \"the emulator is gone\".\n"
		"\n"
		"full reference: pcsxroo/docs/cli.md   agent guide: pcsxroo/docs/agent-guide.md\n");
}

bool PcsxrooCommands::Build(std::vector<std::string> argv, const PcsxrooArgs::Global& global, Request& out,
	std::string& error)
{
	// The option helpers read a non-empty error as "a flag was given without its value", so
	// a message left over from an earlier call must not survive into this one.
	error.clear();

	if (argv.empty())
	{
		error = "no command given";
		return false;
	}

	const std::string group = argv[0];
	argv.erase(argv.begin());

	// Every successful path ends here, so no command can accept an argument it ignores.
	auto finish = [&](Builder& builder, size_t max_positionals) {
		if (!NoLeftovers(argv, max_positionals, error))
			return false;

		out.cmd = builder.Cmd();
		out.json = builder.Finish();
		return true;
	};

	auto simple = [&](const char* cmd) {
		Builder builder(cmd);
		builder.Str("cpu", global.cpu);
		return finish(builder, 0);
	};

	// The commands that block on the server: --timeout becomes their own limit.
	auto own_timeout = [&](Builder& builder, u32 default_ms) {
		const u32 ms = global.timeout_explicit ? global.timeout_ms : default_ms;
		builder.Num("timeout_ms", ms);
		out.command_timeout_ms = ms;
	};

	bool present = false;
	std::string value;

	// --- session ---
	if (group == "version" || group == "status")
		return simple(group.c_str());

	if (group == "wait")
	{
		Builder builder("wait");
		u64 since = 0;
		if (!TakeNumber(argv, "--since", since, present, error))
			return false;
		if (present)
			builder.Num("since", since);

		own_timeout(builder, DEFAULT_WAIT_MS);
		return finish(builder, 0);
	}

	// --- execution ---
	// "resume" reads better than "run" when unpausing, and agents reach for both.
	if (group == "resume")
		return simple("run");

	if (group == "run" || group == "reset" || group == "shutdown")
		return simple(group.c_str());

	if (group == "pause")
	{
		Builder builder("pause");
		builder.Str("cpu", global.cpu);
		own_timeout(builder, DEFAULT_PAUSE_MS);
		return finish(builder, 0);
	}

	if (group == "boot")
	{
		Builder builder("boot");

		const bool bios = PcsxrooArgs::TakeFlag(argv, "--bios");
		if (bios)
			builder.Bool("bios", true);

		if (PcsxrooArgs::TakeFlag(argv, "--pause-on-entry"))
			builder.Bool("pause_on_entry", true);
		if (PcsxrooArgs::TakeFlag(argv, "--fast-boot"))
			builder.Bool("fast_boot", true);

		std::string elf;
		bool has_elf = false;
		if (!TakeString(argv, "--elf", elf, has_elf, error))
			return false;
		if (has_elf)
			builder.Str("elf", elf);

		const bool has_path = !argv.empty() && !IsOption(argv[0]);
		const int sources = (bios ? 1 : 0) + (has_elf ? 1 : 0) + (has_path ? 1 : 0);
		if (sources == 0)
		{
			error = "pass a game path, --elf PATH, or --bios";
			return false;
		}

		if (sources > 1)
		{
			error = "pass only one of a game path, --elf PATH and --bios";
			return false;
		}

		if (has_path)
			builder.Str("path", argv[0]);

		return finish(builder, 1);
	}

	if (group == "input")
	{
		if (!Need(argv, 0, "an input verb", error))
			return false;

		const std::string verb = argv[0];
		argv.erase(argv.begin());

		if (verb != "list" && verb != "release" && verb != "press" && verb != "set")
		{
			error = "unknown input verb: " + verb;
			return false;
		}

		Builder builder("input." + verb);

		u64 pad = 0;
		if (!TakeNumber(argv, "--pad", pad, present, error))
			return false;
		if (present)
			builder.Num("pad", pad);

		if (verb == "list" || verb == "release")
			return finish(builder, 0);

		if (verb == "press")
		{
			u64 frames = 0;
			if (!TakeNumber(argv, "--frames", frames, present, error))
				return false;
			if (present)
				builder.Num("duration_frames", frames);
		}

		// Analog sticks as "--left-stick X,Y" with each component in -1..1.
		for (const char* stick : {"--left-stick", "--right-stick"})
		{
			if (!TakeString(argv, stick, value, present, error))
				return false;
			if (!present)
				continue;

			const size_t comma = value.find(',');
			float x = 0.0f;
			float y = 0.0f;
			if (comma == std::string::npos || !ParseAxis(value.substr(0, comma), x) ||
				!ParseAxis(value.substr(comma + 1), y))
			{
				error = std::string(stick) + " must be X,Y with each axis between -1 and 1";
				return false;
			}

			builder.Stick(std::string(stick) == "--left-stick" ? "left" : "right", x, y);
		}

		// Everything left is a button name; none is how "set" releases a pad.
		if (!NoLeftovers(argv, ANY_NUMBER, error))
			return false;

		if (verb == "press" && argv.empty())
		{
			error = "press needs at least one button; see input list";
			return false;
		}

		builder.StrArray("buttons", argv);
		argv.clear();
		return finish(builder, 0);
	}

	if (group == "step")
	{
		if (!Need(argv, 0, "step mode (into, over or out)", error))
			return false;

		if (argv[0] != "into" && argv[0] != "over" && argv[0] != "out")
		{
			error = "step mode must be into, over or out";
			return false;
		}

		Builder builder("step");
		builder.Str("cpu", global.cpu);
		builder.Str("mode", argv[0]);
		own_timeout(builder, DEFAULT_STEP_MS);
		return finish(builder, 1);
	}

	if (group == "run-to")
	{
		if (!Need(argv, 0, "an address", error))
			return false;

		Builder builder("run-to");
		builder.Str("cpu", global.cpu);
		builder.Address("addr", argv[0]);
		return finish(builder, 1);
	}

	if (group == "frame-advance")
	{
		Builder builder("frame-advance");
		if (!argv.empty() && !IsOption(argv[0]))
		{
			u64 count = 0;
			if (!PcsxrooArgs::ParseNumber(argv[0], count) || count == 0)
			{
				error = "invalid frame count";
				return false;
			}

			builder.Num("count", count);
		}

		own_timeout(builder, DEFAULT_FRAME_ADVANCE_MS);
		return finish(builder, 1);
	}

	// --- breakpoints ---
	if (group == "bp")
	{
		if (!Need(argv, 0, "a bp verb", error))
			return false;

		const std::string verb = argv[0];
		argv.erase(argv.begin());

		Builder builder("bp." + verb);
		builder.Str("cpu", global.cpu);

		if (verb == "add")
		{
			const bool temporary = PcsxrooArgs::TakeFlag(argv, "--temporary");
			const bool disabled = PcsxrooArgs::TakeFlag(argv, "--disabled");

			if (!TakeString(argv, "--cond", value, present, error))
				return false;
			if (present)
				builder.Str("condition", value);

			if (!TakeString(argv, "--desc", value, present, error))
				return false;
			if (present)
				builder.Str("description", value);

			if (!NoLeftovers(argv, ANY_NUMBER, error) || !Need(argv, 0, "an address", error))
				return false;

			builder.Address("addr", argv[0]);
			if (temporary)
				builder.Bool("temporary", true);
			if (disabled)
				builder.Bool("enabled", false);

			return finish(builder, 1);
		}

		if (verb == "list")
		{
			if (PcsxrooArgs::TakeFlag(argv, "--include-temp"))
				builder.Bool("include_temp", true);

			return finish(builder, 0);
		}

		if (verb == "clear")
			return finish(builder, 0);

		if (verb == "remove" || verb == "enable" || verb == "disable")
		{
			if (!NoLeftovers(argv, ANY_NUMBER, error) || !Need(argv, 0, "an address", error))
				return false;

			builder.Address("addr", argv[0]);
			return finish(builder, 1);
		}

		error = "unknown bp verb: " + verb;
		return false;
	}

	// --- memchecks ---
	if (group == "mc")
	{
		if (!Need(argv, 0, "an mc verb", error))
			return false;

		const std::string verb = argv[0];
		argv.erase(argv.begin());

		Builder builder("mc." + verb);
		builder.Str("cpu", global.cpu);

		if (verb == "add")
		{
			std::string on;
			if (!TakeString(argv, "--on", on, present, error))
				return false;
			if (!present)
			{
				error = "--on is required (read, write, change; comma separated)";
				return false;
			}

			std::vector<std::string> conditions;
			size_t start = 0;
			while (start <= on.size())
			{
				const size_t comma = on.find(',', start);
				const std::string piece = on.substr(start, comma - start);
				if (!piece.empty())
					conditions.push_back(piece);

				if (comma == std::string::npos)
					break;

				start = comma + 1;
			}

			builder.StrArray("on", conditions);

			if (PcsxrooArgs::TakeFlag(argv, "--log"))
				builder.StrArray("result", {"break", "log"});

			if (!TakeString(argv, "--cond", value, present, error))
				return false;
			if (present)
				builder.Str("condition", value);

			if (!TakeString(argv, "--desc", value, present, error))
				return false;
			if (present)
				builder.Str("description", value);

			if (!NoLeftovers(argv, ANY_NUMBER, error) || !Need(argv, 1, "a start and end address", error))
				return false;

			builder.Address("start", argv[0]);
			builder.Address("end", argv[1]);
			return finish(builder, 2);
		}

		if (verb == "list" || verb == "clear")
			return finish(builder, 0);

		if (verb == "remove")
		{
			if (!NoLeftovers(argv, ANY_NUMBER, error) || !Need(argv, 1, "a start and end address", error))
				return false;

			builder.Address("start", argv[0]);
			builder.Address("end", argv[1]);
			return finish(builder, 2);
		}

		error = "unknown mc verb: " + verb;
		return false;
	}

	// --- registers ---
	if (group == "reg")
	{
		if (!Need(argv, 0, "a reg verb", error))
			return false;

		const std::string verb = argv[0];
		argv.erase(argv.begin());

		Builder builder("reg." + verb);
		builder.Str("cpu", global.cpu);

		if (verb == "list")
			return finish(builder, 0);

		if (verb == "dump")
		{
			if (!TakeString(argv, "--category", value, present, error))
				return false;
			if (present)
				builder.Str("category", value);

			return finish(builder, 0);
		}

		if (verb == "get")
		{
			if (!NoLeftovers(argv, ANY_NUMBER, error) || !Need(argv, 0, "a register name", error))
				return false;

			builder.Str("name", argv[0]);
			return finish(builder, 1);
		}

		if (verb == "set")
		{
			if (!NoLeftovers(argv, ANY_NUMBER, error) || !Need(argv, 1, "a register name and a value", error))
				return false;

			builder.Str("name", argv[0]);
			builder.Str("value", argv[1]);
			return finish(builder, 2);
		}

		error = "unknown reg verb: " + verb;
		return false;
	}

	// --- memory ---
	if (group == "mem")
	{
		if (!Need(argv, 0, "a mem verb", error))
			return false;

		const std::string verb = argv[0];
		argv.erase(argv.begin());

		const bool base64 = PcsxrooArgs::TakeFlag(argv, "--base64");
		if (base64 && verb != "read" && verb != "write")
		{
			error = "--base64 applies only to mem read and mem write";
			return false;
		}

		Builder builder("mem." + verb);
		builder.Str("cpu", global.cpu);

		if (verb == "read")
		{
			if (!NoLeftovers(argv, ANY_NUMBER, error) || !Need(argv, 0, "an address", error))
				return false;

			builder.Address("addr", argv[0]);

			u64 size = 4;
			if (argv.size() > 1 && !PcsxrooArgs::ParseNumber(argv[1], size))
			{
				error = "invalid size";
				return false;
			}

			builder.Num("size", size);
			if (base64)
				builder.Str("format", "base64");

			return finish(builder, 2);
		}

		if (verb == "write")
		{
			if (!NoLeftovers(argv, ANY_NUMBER, error) || !Need(argv, 1, "an address and data", error))
				return false;

			builder.Address("addr", argv[0]);
			builder.Str("data", argv[1]);
			if (base64)
				builder.Str("format", "base64");

			return finish(builder, 2);
		}

		if (verb == "fill" || verb == "dump")
		{
			const char* what = verb == "fill" ? "an address, a size and a pattern" : "an address, a size and a path";
			if (!NoLeftovers(argv, ANY_NUMBER, error) || !Need(argv, 2, what, error))
				return false;

			u64 size = 0;
			if (!PcsxrooArgs::ParseNumber(argv[1], size))
			{
				error = "invalid size";
				return false;
			}

			builder.Address("addr", argv[0]);
			builder.Num("size", size);
			builder.Str(verb == "fill" ? "pattern" : "path", argv[2]);
			return finish(builder, 3);
		}

		if (verb == "search")
		{
			if (!TakeString(argv, "--type", value, present, error))
				return false;
			if (present)
				builder.Str("type", value);

			u64 session = 0;
			if (!TakeNumber(argv, "--session", session, present, error))
				return false;

			if (present)
			{
				builder.Num("session", session);
			}
			else
			{
				// A first pass needs somewhere to look; a chained one inherits the range.
				if (!TakeString(argv, "--range", value, present, error))
					return false;

				const size_t colon = value.find(':');
				if (!present || colon == std::string::npos)
				{
					error = "--range START:END is required for a new search";
					return false;
				}

				builder.Address("start", value.substr(0, colon));
				builder.Address("end", value.substr(colon + 1));
			}

			u64 max_results = 0;
			if (!TakeNumber(argv, "--max", max_results, present, error))
				return false;
			if (present)
				builder.Num("max_results", max_results);

			// Comparisons read as flags so the command line stays close to how the search is
			// described out loud: "--eq 2", "--increased-by 1", "--changed".
			struct ComparisonFlag
			{
				const char* flag;
				const char* name;
				bool takes_value;
			};

			static constexpr ComparisonFlag comparisons[] = {
				{"--eq", "eq", true}, {"--ne", "ne", true}, {"--gt", "gt", true}, {"--gte", "gte", true},
				{"--lt", "lt", true}, {"--lte", "lte", true}, {"--increased-by", "increased_by", true},
				{"--decreased-by", "decreased_by", true}, {"--changed-by", "changed_by", true},
				{"--increased", "increased", false}, {"--decreased", "decreased", false},
				{"--changed", "changed", false}, {"--not-changed", "not_changed", false},
				{"--unknown", "unknown", false}};

			bool chose = false;
			for (const ComparisonFlag& comparison : comparisons)
			{
				if (comparison.takes_value)
				{
					if (!TakeString(argv, comparison.flag, value, present, error))
						return false;
					if (!present)
						continue;

					if (!AddSearchValue(builder, value, error))
						return false;
				}
				else if (!PcsxrooArgs::TakeFlag(argv, comparison.flag))
				{
					continue;
				}

				builder.Str("comparison", comparison.name);
				chose = true;
				break;
			}

			if (!chose)
			{
				error = "pass a comparison, for example --eq 2, --increased-by 1, --changed or --unknown";
				return false;
			}

			// A second comparison is left in argv, and finish reports it.
			return finish(builder, 0);
		}

		error = "unknown mem verb: " + verb;
		return false;
	}

	// --- code ---
	if (group == "dis")
	{
		Builder builder("dis");
		builder.Str("cpu", global.cpu);

		if (PcsxrooArgs::TakeFlag(argv, "--raw"))
			builder.Bool("simplify", false);

		if (!NoLeftovers(argv, ANY_NUMBER, error) || !Need(argv, 0, "an address", error))
			return false;

		builder.Address("addr", argv[0]);

		u64 count = 16;
		if (argv.size() > 1 && !PcsxrooArgs::ParseNumber(argv[1], count))
		{
			error = "invalid instruction count";
			return false;
		}

		builder.Num("count", count);
		return finish(builder, 2);
	}

	if (group == "asm")
	{
		if (!NoLeftovers(argv, ANY_NUMBER, error) || !Need(argv, 1, "an address and an instruction", error))
			return false;

		Builder builder("asm");
		builder.Str("cpu", global.cpu);
		builder.Address("addr", argv[0]);
		builder.StrArray("instructions", std::vector<std::string>(argv.begin() + 1, argv.end()));
		return finish(builder, ANY_NUMBER);
	}

	if (group == "sym")
	{
		if (!NoLeftovers(argv, ANY_NUMBER, error) || !Need(argv, 1, "a sym verb and an argument", error))
			return false;

		const std::string verb = argv[0];
		Builder builder("sym." + verb);
		builder.Str("cpu", global.cpu);

		if (verb == "lookup")
			builder.Address("addr", argv[1]);
		else if (verb == "find")
			builder.Str("name", argv[1]);
		else
		{
			error = "unknown sym verb: " + verb;
			return false;
		}

		return finish(builder, 2);
	}

	// --- context ---
	if (group == "stack" || group == "threads" || group == "modules")
		return simple(group.c_str());

	if (group == "eval")
	{
		if (!Need(argv, 0, "an expression", error))
			return false;

		Builder builder("eval");
		builder.Str("cpu", global.cpu);
		builder.Str("expression", argv[0]);
		return finish(builder, 1);
	}

	// --- state and observation ---
	if (group == "savestate" || group == "loadstate")
	{
		Builder builder(group);

		u64 slot = 0;
		bool has_slot = false;
		if (!TakeNumber(argv, "--slot", slot, has_slot, error))
			return false;

		std::string path;
		bool has_path = false;
		if (!TakeString(argv, "--path", path, has_path, error))
			return false;

		// Both used to be accepted, and the slot silently won.
		if (has_slot == has_path)
		{
			error = "pass exactly one of --slot N and --path PATH";
			return false;
		}

		if (has_slot)
			builder.Num("slot", slot);
		else
			builder.Str("path", path);

		if (group == "savestate" && PcsxrooArgs::TakeFlag(argv, "--wait-flush"))
			builder.Bool("wait_flush", true);

		return finish(builder, 0);
	}

	if (group == "screenshot")
	{
		if (!NoLeftovers(argv, ANY_NUMBER, error) || !Need(argv, 0, "a path", error))
			return false;

		Builder builder("screenshot");
		builder.Str("path", argv[0]);
		return finish(builder, 1);
	}

	if (group == "patch")
	{
		if (argv.empty() || argv[0] != "reload")
		{
			error = "the only patch verb is reload";
			return false;
		}

		argv.erase(argv.begin());
		return simple("patch.reload");
	}

	error = "unknown command: " + group;
	return false;
}
