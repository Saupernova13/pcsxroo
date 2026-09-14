// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsxroo/server/MemorySearch.h"

#include <cmath>
#include <cstring>

namespace
{
	using namespace MemorySearch;

	constexpr u32 MAX_RANGE_BYTES = 64 * 1024 * 1024;
	constexpr size_t CHUNK_BYTES = 64 * 1024;

	bool IsFloat(ValueType type)
	{
		return type == ValueType::F32 || type == ValueType::F64;
	}

	bool IsSigned(ValueType type)
	{
		return type == ValueType::I8 || type == ValueType::I16 || type == ValueType::I32 ||
			   type == ValueType::I64;
	}

	// Reads one value out of a byte buffer, returning both an integer view (for equality and
	// the stored raw) and a numeric view used for ordering and deltas.
	void Decode(ValueType type, const u8* bytes, u64& raw, double& numeric)
	{
		raw = 0;
		const size_t size = ValueSize(type);
		std::memcpy(&raw, bytes, size);

		switch (type)
		{
			case ValueType::F32:
			{
				float value = 0.0f;
				std::memcpy(&value, bytes, sizeof(value));
				numeric = static_cast<double>(value);
				break;
			}
			case ValueType::F64:
				std::memcpy(&numeric, bytes, sizeof(numeric));
				break;
			case ValueType::I8:
				numeric = static_cast<double>(static_cast<s8>(raw));
				break;
			case ValueType::I16:
				numeric = static_cast<double>(static_cast<s16>(raw));
				break;
			case ValueType::I32:
				numeric = static_cast<double>(static_cast<s32>(raw));
				break;
			case ValueType::I64:
				numeric = static_cast<double>(static_cast<s64>(raw));
				break;
			default:
				numeric = static_cast<double>(raw);
				break;
		}
	}

	// The needle, decoded the same way as memory, so comparisons are like for like.
	bool DecodeNeedle(const Query& query, u64& raw, double& numeric)
	{
		const size_t size = ValueSize(query.type);
		if (size == 0 || query.value.size() < size)
			return false;

		Decode(query.type, query.value.data(), raw, numeric);
		return true;
	}

	bool MatchesAgainstNeedle(const Query& query, u64 raw, double numeric, u64 needle_raw, double needle_numeric)
	{
		switch (query.comparison)
		{
			case Comparison::Unknown:
				return true;
			case Comparison::Eq:
				// Exact bit equality for floats too: that is what a memory searcher means by
				// "equals", and an epsilon here would match unrelated addresses.
				return IsFloat(query.type) ? (numeric == needle_numeric) : (raw == needle_raw);
			case Comparison::Ne:
				return IsFloat(query.type) ? (numeric != needle_numeric) : (raw != needle_raw);
			case Comparison::Gt:
				return numeric > needle_numeric;
			case Comparison::Gte:
				return numeric >= needle_numeric;
			case Comparison::Lt:
				return numeric < needle_numeric;
			case Comparison::Lte:
				return numeric <= needle_numeric;
			default:
				// Delta comparisons need a previous value; a first pass cannot answer them.
				return false;
		}
	}

	bool MatchesAgainstPrevious(const Query& query, double now, double before, double needle)
	{
		switch (query.comparison)
		{
			case Comparison::Increased:
				return now > before;
			case Comparison::Decreased:
				return now < before;
			case Comparison::Changed:
				return now != before;
			case Comparison::NotChanged:
				return now == before;
			case Comparison::IncreasedBy:
				return (now - before) == needle;
			case Comparison::DecreasedBy:
				return (before - now) == needle;
			case Comparison::ChangedBy:
				return std::fabs(now - before) == needle;
			default:
				return false;
		}
	}

	bool NeedsPrevious(Comparison comparison)
	{
		switch (comparison)
		{
			case Comparison::Increased:
			case Comparison::IncreasedBy:
			case Comparison::Decreased:
			case Comparison::DecreasedBy:
			case Comparison::Changed:
			case Comparison::ChangedBy:
			case Comparison::NotChanged:
				return true;
			default:
				return false;
		}
	}

	bool ValidateRange(const Query& query, std::string& error)
	{
		if (query.end <= query.start)
		{
			error = "end must be greater than start";
			return false;
		}

		if ((query.end - query.start) > MAX_RANGE_BYTES)
		{
			error = "range is larger than 64 MiB";
			return false;
		}

		if (ValueSize(query.type) == 0)
		{
			error = "unsupported value type for a range scan";
			return false;
		}

		return true;
	}
} // namespace

size_t MemorySearch::ValueSize(ValueType type)
{
	switch (type)
	{
		case ValueType::U8:
		case ValueType::I8:
			return 1;
		case ValueType::U16:
		case ValueType::I16:
			return 2;
		case ValueType::U32:
		case ValueType::I32:
		case ValueType::F32:
			return 4;
		case ValueType::U64:
		case ValueType::I64:
		case ValueType::F64:
			return 8;
		case ValueType::Bytes:
		default:
			return 0;
	}
}

bool MemorySearch::RunFirstPass(MemoryInterface& memory, const Query& query, std::vector<Hit>& out,
	std::string& error)
{
	out.clear();

	if (!ValidateRange(query, error))
		return false;

	if (NeedsPrevious(query.comparison))
	{
		error = "this comparison needs a previous result set; start with unknown";
		return false;
	}

	u64 needle_raw = 0;
	double needle_numeric = 0.0;
	if (query.comparison != Comparison::Unknown && !DecodeNeedle(query, needle_raw, needle_numeric))
	{
		error = "the search value does not match the value type";
		return false;
	}

	const size_t size = ValueSize(query.type);
	std::vector<u8> chunk(CHUNK_BYTES + size);

	// 64-bit so that a range ending near the top of the address space cannot wrap the cursor
	// back to zero, which scanned the whole of memory forever.
	for (u64 base = query.start; base < query.end;)
	{
		const u32 remaining = static_cast<u32>(query.end - base);
		// Overlap by one value so a match straddling a chunk boundary is not missed.
		const u32 want = std::min<u32>(remaining, static_cast<u32>(CHUNK_BYTES + size - 1));

		if (!memory.ReadBytes(static_cast<u32>(base), chunk.data(), want))
		{
			// Unmapped regions are normal in a 32 MiB sweep; skip rather than fail.
			base += CHUNK_BYTES;
			continue;
		}

		for (u32 offset = 0; offset + size <= want; offset += static_cast<u32>(size))
		{
			u64 raw = 0;
			double numeric = 0.0;
			Decode(query.type, chunk.data() + offset, raw, numeric);

			if (!MatchesAgainstNeedle(query, raw, numeric, needle_raw, needle_numeric))
				continue;

			out.push_back({static_cast<u32>(base + offset), raw, numeric});
			if (out.size() >= query.max_results)
				return true;
		}

		base += CHUNK_BYTES;
	}

	return true;
}

bool MemorySearch::RunFilterPass(MemoryInterface& memory, const Query& query, const std::vector<Hit>& previous,
	std::vector<Hit>& out, std::string& error)
{
	out.clear();

	const size_t size = ValueSize(query.type);
	if (size == 0)
	{
		error = "unsupported value type";
		return false;
	}

	u64 needle_raw = 0;
	double needle_numeric = 0.0;
	const bool has_needle = DecodeNeedle(query, needle_raw, needle_numeric);

	// Only unknown, increased, decreased, changed and not_changed work without a value. The
	// rest used to compare against zero when the value was missing, and quietly answered a
	// question nobody asked.
	switch (query.comparison)
	{
		case Comparison::Unknown:
		case Comparison::Increased:
		case Comparison::Decreased:
		case Comparison::Changed:
		case Comparison::NotChanged:
			break;
		default:
			if (!has_needle)
			{
				error = "this comparison needs a value";
				return false;
			}
			break;
	}

	u8 bytes[8] = {};
	for (const Hit& hit : previous)
	{
		if (!memory.ReadBytes(hit.addr, bytes, static_cast<u32>(size)))
			continue;

		u64 raw = 0;
		double numeric = 0.0;
		Decode(query.type, bytes, raw, numeric);

		const bool matched = NeedsPrevious(query.comparison)
								 ? MatchesAgainstPrevious(query, numeric, hit.as_double, needle_numeric)
								 : MatchesAgainstNeedle(query, raw, numeric, needle_raw, needle_numeric);

		if (!matched)
			continue;

		out.push_back({hit.addr, raw, numeric});
		if (out.size() >= query.max_results)
			break;
	}

	return true;
}

bool MemorySearch::ParseValueType(std::string_view text, ValueType& out)
{
	if (text == "u8") { out = ValueType::U8; return true; }
	if (text == "u16") { out = ValueType::U16; return true; }
	if (text == "u32") { out = ValueType::U32; return true; }
	if (text == "u64") { out = ValueType::U64; return true; }
	if (text == "i8") { out = ValueType::I8; return true; }
	if (text == "i16") { out = ValueType::I16; return true; }
	if (text == "i32") { out = ValueType::I32; return true; }
	if (text == "i64") { out = ValueType::I64; return true; }
	if (text == "f32") { out = ValueType::F32; return true; }
	if (text == "f64") { out = ValueType::F64; return true; }
	if (text == "bytes") { out = ValueType::Bytes; return true; }

	return false;
}

bool MemorySearch::ParseComparison(std::string_view text, Comparison& out)
{
	if (text == "eq") { out = Comparison::Eq; return true; }
	if (text == "ne") { out = Comparison::Ne; return true; }
	if (text == "gt") { out = Comparison::Gt; return true; }
	if (text == "gte") { out = Comparison::Gte; return true; }
	if (text == "lt") { out = Comparison::Lt; return true; }
	if (text == "lte") { out = Comparison::Lte; return true; }
	if (text == "increased") { out = Comparison::Increased; return true; }
	if (text == "increased_by") { out = Comparison::IncreasedBy; return true; }
	if (text == "decreased") { out = Comparison::Decreased; return true; }
	if (text == "decreased_by") { out = Comparison::DecreasedBy; return true; }
	if (text == "changed") { out = Comparison::Changed; return true; }
	if (text == "changed_by") { out = Comparison::ChangedBy; return true; }
	if (text == "not_changed") { out = Comparison::NotChanged; return true; }
	if (text == "unknown") { out = Comparison::Unknown; return true; }

	return false;
}
