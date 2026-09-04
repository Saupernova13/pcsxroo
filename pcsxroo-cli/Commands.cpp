// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsxroo-cli/Commands.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <fmt/format.h>

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
		"  --json            print the raw JSON response\n"
		"  --timeout MS      request timeout (default 5000); for wait and step this is the\n"
		"                    command's own timeout too\n"
		"\n"
		"session:\n"
		"  version                          server and emulator versions\n"
		"  status                           VM state, PCs, game identity, last stop\n"
		"  wait [--since N]                 block until the next stop\n"
		"  events                           stream stop events until interrupted\n"
		"  launch <iso|elf> [--pause-on-entry] [--emulator PATH] [--ready-timeout MS]\n"
		"\n"
		"execution:\n"
		"  run | pause | reset | shutdown\n"
		"  step into|over|out\n"
		"  run-to <addr>\n"
		"  frame-advance [count]\n"
		"\n"
		"breakpoints:\n"
		"  bp add <addr> [--cond EXPR] [--desc TEXT] [--temporary] [--disabled]\n"
		"  bp remove <addr> | bp list [--include-temp] | bp enable <addr>\n"
		"  bp disable <addr> | bp clear\n"
		"\n"
		"memchecks:\n"
		"  mc add <start> <end> --on read,write,change [--log] [--cond EXPR] [--desc TEXT]\n"
		"  mc remove <start> <end> | mc list | mc clear\n"
		"\n"
		"registers (need a paused VM):\n"
		"  reg list | reg get <name> | reg set <name> <value>\n"
		"  reg dump [--category NAME]\n"
		"\n"
		"memory:\n"
		"  mem read <addr> [size] [--base64]\n"
		"  mem write <addr> <hex> [--base64]\n"
		"  mem fill <addr> <size> <pattern-hex>\n"
		"  mem dump <addr> <size> <path>\n"
		"\n"
		"code and symbols:\n"
		"  dis <addr> [count] [--raw]       disassemble\n"
		"  asm <addr> \"<instruction>\"       assemble and write\n"
		"  sym lookup <addr> | sym find <name>\n"
		"\n"
		"context:\n"
		"  stack | threads | modules | eval \"<expression>\"\n"
		"\n"
		"state and observation:\n"
		"  savestate --slot N | --path P [--wait-flush]\n"
		"  loadstate --slot N | --path P\n"
		"  screenshot <path>\n"
		"  patch reload\n"
		"\n"
		"exit codes: 0 ok, 1 server error, 2 usage, 3 cannot connect, 4 timed out\n");
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
	if (group == "run" || group == "pause" || group == "reset" || group == "shutdown")
		return simple(group.c_str());

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
