// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsxroo-cli/Commands.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <fmt/format.h>

#include <cstdlib>

namespace
{
	using Allocator = rapidjson::Document::AllocatorType;

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
		"  --timeout MS      request timeout (default 5000). For wait and step this also\n"
		"                    sets the command's own timeout.\n"
		"\n"
		"starting up:\n"
		"  launch [game] [--bios] [--pause-on-entry] [--emulator PATH]\n"
		"                                   [--ready-timeout MS]\n"
		"                                   start the emulator; with no game it comes up\n"
		"                                   idle with the server listening and no VM\n"
		"  boot <game>                      boot a game in an emulator already running\n"
		"  boot --bios                      boot the PS2 BIOS with no disc\n"
		"  boot --elf PATH                  boot an ELF directly\n"
		"      both accept [--pause-on-entry] [--fast-boot]\n"
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
		"\n"
		"code and symbols:\n"
		"  dis <addr> [count] [--raw]       disassemble; --raw skips simplification\n"
		"  asm <addr> \"<instruction>\"       assemble and write in place\n"
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
		"main+0x40 or [0x1B1F038].\n"
		"\n"
		"exit codes: 0 ok, 1 server error, 2 usage, 3 cannot connect, 4 timed out.\n"
		"3 and 4 differ on purpose: \"no breakpoint yet\" is not \"the emulator is gone\".\n"
		"\n"
		"full reference: docs/pcsxroo/cli.md   agent guide: docs/pcsxroo/agent-guide.md\n");
}

bool PcsxrooCommands::Build(std::vector<std::string> argv, const PcsxrooArgs::Global& global, Request& out,
	std::string& error)
{
	if (argv.empty())
	{
		error = "no command given";
		return false;
	}

	const std::string group = argv[0];
	argv.erase(argv.begin());

	auto simple = [&](const char* cmd) {
		Builder builder(cmd);
		builder.Str("cpu", global.cpu);
		out.cmd = builder.Cmd();
		out.json = builder.Finish();
		return true;
	};

	// --- session ---
	if (group == "version")
		return simple("version");
	if (group == "status")
		return simple("status");

	if (group == "wait")
	{
		Builder builder("wait");
		std::string value;
		if (PcsxrooArgs::TakeOption(argv, "--since", value, error))
		{
			u64 since = 0;
			if (!PcsxrooArgs::ParseNumber(value, since))
			{
				error = "invalid --since value";
				return false;
			}

			builder.Num("since", since);
		}
		else if (!error.empty())
		{
			return false;
		}

		builder.Num("timeout_ms", global.timeout_explicit ? global.timeout_ms : 60000);
		out.cmd = builder.Cmd();
		out.json = builder.Finish();
		return true;
	}

	// --- execution ---
	// "resume" reads better than "run" when unpausing, and agents reach for both.
	if (group == "resume")
		return simple("run");

	if (group == "run" || group == "pause" || group == "reset" || group == "shutdown")
		return simple(group.c_str());

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

		std::string value;
		if (PcsxrooArgs::TakeOption(argv, "--elf", value, error))
			builder.Str("elf", value);
		else if (!error.empty())
			return false;

		if (!argv.empty())
			builder.Str("path", argv[0]);
		else if (!bios && value.empty())
		{
			error = "pass a game path, --elf PATH, or --bios";
			return false;
		}

		out.cmd = builder.Cmd();
		out.json = builder.Finish();
		return true;
	}

	if (group == "input")
	{
		if (!Need(argv, 0, "an input verb", error))
			return false;

		const std::string verb = argv[0];
		argv.erase(argv.begin());

		if (verb == "list" || verb == "release")
		{
			Builder builder("input." + verb);
			std::string value;
			if (PcsxrooArgs::TakeOption(argv, "--pad", value, error))
			{
				u64 pad = 0;
				if (!PcsxrooArgs::ParseNumber(value, pad))
				{
					error = "invalid --pad";
					return false;
				}

				builder.Num("pad", pad);
			}
			else if (!error.empty())
			{
				return false;
			}

			out.cmd = builder.Cmd();
			out.json = builder.Finish();
			return true;
		}

		if (verb != "press" && verb != "set")
		{
			error = "unknown input verb: " + verb;
			return false;
		}

		Builder builder("input." + verb);

		std::string value;
		if (PcsxrooArgs::TakeOption(argv, "--pad", value, error))
		{
			u64 pad = 0;
			if (!PcsxrooArgs::ParseNumber(value, pad))
			{
				error = "invalid --pad";
				return false;
			}

			builder.Num("pad", pad);
		}
		else if (!error.empty())
		{
			return false;
		}

		if (verb == "press" && PcsxrooArgs::TakeOption(argv, "--frames", value, error))
		{
			u64 frames = 0;
			if (!PcsxrooArgs::ParseNumber(value, frames))
			{
				error = "invalid --frames";
				return false;
			}

			builder.Num("duration_frames", frames);
		}
		else if (!error.empty())
		{
			return false;
		}

		// Analog sticks as "--left-stick X,Y" with each component in -1..1.
		for (const char* stick : {"--left-stick", "--right-stick"})
		{
			if (!PcsxrooArgs::TakeOption(argv, stick, value, error))
			{
				if (!error.empty())
					return false;

				continue;
			}

			const size_t comma = value.find(',');
			if (comma == std::string::npos)
			{
				error = std::string(stick) + " must be X,Y";
				return false;
			}

			builder.Stick(std::string(stick) == "--left-stick" ? "left" : "right",
				std::strtof(value.substr(0, comma).c_str(), nullptr),
				std::strtof(value.substr(comma + 1).c_str(), nullptr));
		}

		// Everything left is a button name; none is how "set" releases a pad.
		std::vector<std::string> buttons;
		for (const std::string& argument : argv)
		{
			if (!argument.empty() && argument[0] == '-')
			{
				error = "unexpected option: " + argument;
				return false;
			}

			buttons.push_back(argument);
		}

		if (verb == "press" && buttons.empty())
		{
			error = "press needs at least one button; see input list";
			return false;
		}

		builder.StrArray("buttons", buttons);
		out.cmd = builder.Cmd();
		out.json = builder.Finish();
		return true;
	}

	if (group == "step")
	{
		if (!Need(argv, 0, "step mode (into, over or out)", error))
			return false;

		Builder builder("step");
		builder.Str("cpu", global.cpu);
		builder.Str("mode", argv[0]);
		builder.Num("timeout_ms", global.timeout_explicit ? global.timeout_ms : 5000);
		out.cmd = builder.Cmd();
		out.json = builder.Finish();
		return true;
	}

	if (group == "run-to")
	{
		if (!Need(argv, 0, "an address", error))
			return false;

		Builder builder("run-to");
		builder.Str("cpu", global.cpu);
		builder.Address("addr", argv[0]);
		out.cmd = builder.Cmd();
		out.json = builder.Finish();
		return true;
	}

	if (group == "frame-advance")
	{
		Builder builder("frame-advance");
		if (!argv.empty())
		{
			u64 count = 0;
			if (!PcsxrooArgs::ParseNumber(argv[0], count))
			{
				error = "invalid frame count";
				return false;
			}

			builder.Num("count", count);
		}

		out.cmd = builder.Cmd();
		out.json = builder.Finish();
		return true;
	}

	// --- breakpoints ---
	if (group == "bp")
	{
		if (!Need(argv, 0, "a bp verb", error))
			return false;

		const std::string verb = argv[0];
		argv.erase(argv.begin());

		if (verb == "add")
		{
			Builder builder("bp.add");
			builder.Str("cpu", global.cpu);

			const bool temporary = PcsxrooArgs::TakeFlag(argv, "--temporary");
			const bool disabled = PcsxrooArgs::TakeFlag(argv, "--disabled");

			std::string value;
			if (PcsxrooArgs::TakeOption(argv, "--cond", value, error))
				builder.Str("condition", value);
			else if (!error.empty())
				return false;

			if (PcsxrooArgs::TakeOption(argv, "--desc", value, error))
				builder.Str("description", value);
			else if (!error.empty())
				return false;

			if (!Need(argv, 0, "an address", error))
				return false;

			builder.Address("addr", argv[0]);
			if (temporary)
				builder.Bool("temporary", true);
			if (disabled)
				builder.Bool("enabled", false);

			out.cmd = builder.Cmd();
			out.json = builder.Finish();
			return true;
		}

		if (verb == "list")
		{
			Builder builder("bp.list");
			builder.Str("cpu", global.cpu);
			if (PcsxrooArgs::TakeFlag(argv, "--include-temp"))
				builder.Bool("include_temp", true);

			out.cmd = builder.Cmd();
			out.json = builder.Finish();
			return true;
		}

		if (verb == "clear")
		{
			Builder builder("bp.clear");
			builder.Str("cpu", global.cpu);
			out.cmd = builder.Cmd();
			out.json = builder.Finish();
			return true;
		}

		if (verb == "remove" || verb == "enable" || verb == "disable")
		{
			if (!Need(argv, 0, "an address", error))
				return false;

			Builder builder("bp." + verb);
			builder.Str("cpu", global.cpu);
			builder.Address("addr", argv[0]);
			out.cmd = builder.Cmd();
			out.json = builder.Finish();
			return true;
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

		if (verb == "add")
		{
			Builder builder("mc.add");
			builder.Str("cpu", global.cpu);

			std::string on;
			if (!PcsxrooArgs::TakeOption(argv, "--on", on, error))
			{
				if (error.empty())
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

			std::string value;
			if (PcsxrooArgs::TakeOption(argv, "--cond", value, error))
				builder.Str("condition", value);
			else if (!error.empty())
				return false;

			if (PcsxrooArgs::TakeOption(argv, "--desc", value, error))
				builder.Str("description", value);
			else if (!error.empty())
				return false;

			if (!Need(argv, 1, "a start and end address", error))
				return false;

			builder.Address("start", argv[0]);
			builder.Address("end", argv[1]);
			out.cmd = builder.Cmd();
			out.json = builder.Finish();
			return true;
		}

		if (verb == "list" || verb == "clear")
		{
			Builder builder("mc." + verb);
			builder.Str("cpu", global.cpu);
			out.cmd = builder.Cmd();
			out.json = builder.Finish();
			return true;
		}

		if (verb == "remove")
		{
			if (!Need(argv, 1, "a start and end address", error))
				return false;

			Builder builder("mc.remove");
			builder.Str("cpu", global.cpu);
			builder.Address("start", argv[0]);
			builder.Address("end", argv[1]);
			out.cmd = builder.Cmd();
			out.json = builder.Finish();
			return true;
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

		if (verb == "list")
		{
			Builder builder("reg.list");
			builder.Str("cpu", global.cpu);
			out.cmd = builder.Cmd();
			out.json = builder.Finish();
			return true;
		}

		if (verb == "dump")
		{
			Builder builder("reg.dump");
			builder.Str("cpu", global.cpu);

			std::string value;
			if (PcsxrooArgs::TakeOption(argv, "--category", value, error))
				builder.Str("category", value);
			else if (!error.empty())
				return false;

			out.cmd = builder.Cmd();
			out.json = builder.Finish();
			return true;
		}

		if (verb == "get")
		{
			if (!Need(argv, 0, "a register name", error))
				return false;

			Builder builder("reg.get");
			builder.Str("cpu", global.cpu);
			builder.Str("name", argv[0]);
			out.cmd = builder.Cmd();
			out.json = builder.Finish();
			return true;
		}

		if (verb == "set")
		{
			if (!Need(argv, 1, "a register name and a value", error))
				return false;

			Builder builder("reg.set");
			builder.Str("cpu", global.cpu);
			builder.Str("name", argv[0]);
			builder.Str("value", argv[1]);
			out.cmd = builder.Cmd();
			out.json = builder.Finish();
			return true;
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

		if (verb == "read")
		{
			if (!Need(argv, 0, "an address", error))
				return false;

			Builder builder("mem.read");
			builder.Str("cpu", global.cpu);
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

			out.cmd = builder.Cmd();
			out.json = builder.Finish();
			return true;
		}

		if (verb == "write")
		{
			if (!Need(argv, 1, "an address and data", error))
				return false;

			Builder builder("mem.write");
			builder.Str("cpu", global.cpu);
			builder.Address("addr", argv[0]);
			builder.Str("data", argv[1]);
			if (base64)
				builder.Str("format", "base64");

			out.cmd = builder.Cmd();
			out.json = builder.Finish();
			return true;
		}

		if (verb == "fill")
		{
			if (!Need(argv, 2, "an address, a size and a pattern", error))
				return false;

			u64 size = 0;
			if (!PcsxrooArgs::ParseNumber(argv[1], size))
			{
				error = "invalid size";
				return false;
			}

			Builder builder("mem.fill");
			builder.Str("cpu", global.cpu);
			builder.Address("addr", argv[0]);
			builder.Num("size", size);
			builder.Str("pattern", argv[2]);
			out.cmd = builder.Cmd();
			out.json = builder.Finish();
			return true;
		}

		if (verb == "search")
		{
			Builder builder("mem.search");
			builder.Str("cpu", global.cpu);

			std::string value;
			if (PcsxrooArgs::TakeOption(argv, "--type", value, error))
				builder.Str("type", value);
			else if (!error.empty())
				return false;

			if (PcsxrooArgs::TakeOption(argv, "--session", value, error))
			{
				u64 session = 0;
				if (!PcsxrooArgs::ParseNumber(value, session))
				{
					error = "invalid --session";
					return false;
				}

				builder.Num("session", session);
			}
			else if (!error.empty())
			{
				return false;
			}
			else
			{
				// A first pass needs somewhere to look; a chained one inherits the range.
				if (!PcsxrooArgs::TakeOption(argv, "--range", value, error))
				{
					if (error.empty())
						error = "--range START:END is required for a new search";
					return false;
				}

				const size_t colon = value.find(':');
				if (colon == std::string::npos)
				{
					error = "--range must be START:END";
					return false;
				}

				builder.Address("start", value.substr(0, colon));
				builder.Address("end", value.substr(colon + 1));
			}

			if (PcsxrooArgs::TakeOption(argv, "--max", value, error))
			{
				u64 max_results = 0;
				if (!PcsxrooArgs::ParseNumber(value, max_results))
				{
					error = "invalid --max";
					return false;
				}

				builder.Num("max_results", max_results);
			}
			else if (!error.empty())
			{
				return false;
			}

			// Comparisons read as flags so the command line stays close to how the search is
			// described out loud: "--eq 2", "--increased-by 1", "--changed".
			struct ComparisonFlag
			{
				const char* flag;
				const char* name;
				bool takes_value;
			};

			static const ComparisonFlag comparisons[] = {
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
					if (!PcsxrooArgs::TakeOption(argv, comparison.flag, value, error))
					{
						if (!error.empty())
							return false;

						continue;
					}

					u64 needle = 0;
					if (!PcsxrooArgs::ParseNumber(value, needle))
					{
						error = std::string("invalid value for ") + comparison.flag;
						return false;
					}

					builder.Num("value", needle);
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

			out.cmd = builder.Cmd();
			out.json = builder.Finish();
			return true;
		}

		if (verb == "dump")
		{
			if (!Need(argv, 2, "an address, a size and a path", error))
				return false;

			u64 size = 0;
			if (!PcsxrooArgs::ParseNumber(argv[1], size))
			{
				error = "invalid size";
				return false;
			}

			Builder builder("mem.dump");
			builder.Str("cpu", global.cpu);
			builder.Address("addr", argv[0]);
			builder.Num("size", size);
			builder.Str("path", argv[2]);
			out.cmd = builder.Cmd();
			out.json = builder.Finish();
			return true;
		}

		error = "unknown mem verb: " + verb;
		return false;
	}

	// --- code ---
	if (group == "dis")
	{
		if (!Need(argv, 0, "an address", error))
			return false;

		Builder builder("dis");
		builder.Str("cpu", global.cpu);

		if (PcsxrooArgs::TakeFlag(argv, "--raw"))
			builder.Bool("simplify", false);

		builder.Address("addr", argv[0]);

		u64 count = 16;
		if (argv.size() > 1 && !PcsxrooArgs::ParseNumber(argv[1], count))
		{
			error = "invalid instruction count";
			return false;
		}

		builder.Num("count", count);
		out.cmd = builder.Cmd();
		out.json = builder.Finish();
		return true;
	}

	if (group == "asm")
	{
		if (!Need(argv, 1, "an address and an instruction", error))
			return false;

		Builder builder("asm");
		builder.Str("cpu", global.cpu);
		builder.Address("addr", argv[0]);
		builder.StrArray("instructions", std::vector<std::string>(argv.begin() + 1, argv.end()));
		out.cmd = builder.Cmd();
		out.json = builder.Finish();
		return true;
	}

	if (group == "sym")
	{
		if (!Need(argv, 1, "a sym verb and an argument", error))
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

		out.cmd = builder.Cmd();
		out.json = builder.Finish();
		return true;
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
		out.cmd = builder.Cmd();
		out.json = builder.Finish();
		return true;
	}

	// --- state and observation ---
	if (group == "savestate" || group == "loadstate")
	{
		Builder builder(group);
		std::string value;

		if (PcsxrooArgs::TakeOption(argv, "--slot", value, error))
		{
			u64 slot = 0;
			if (!PcsxrooArgs::ParseNumber(value, slot))
			{
				error = "invalid slot";
				return false;
			}

			builder.Num("slot", slot);
		}
		else if (!error.empty())
		{
			return false;
		}
		else if (PcsxrooArgs::TakeOption(argv, "--path", value, error))
		{
			builder.Str("path", value);
		}
		else if (!error.empty())
		{
			return false;
		}
		else
		{
			error = "pass --slot N or --path PATH";
			return false;
		}

		if (group == "savestate" && PcsxrooArgs::TakeFlag(argv, "--wait-flush"))
			builder.Bool("wait_flush", true);

		out.cmd = builder.Cmd();
		out.json = builder.Finish();
		return true;
	}

	if (group == "screenshot")
	{
		if (!Need(argv, 0, "a path", error))
			return false;

		Builder builder("screenshot");
		builder.Str("path", argv[0]);
		out.cmd = builder.Cmd();
		out.json = builder.Finish();
		return true;
	}

	if (group == "patch")
	{
		if (argv.empty() || argv[0] != "reload")
		{
			error = "the only patch verb is reload";
			return false;
		}

		return simple("patch.reload");
	}

	error = "unknown command: " + group;
	return false;
}
