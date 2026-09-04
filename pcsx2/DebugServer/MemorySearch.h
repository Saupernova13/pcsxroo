// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/MemoryInterface.h"
#include "common/Pcsx2Types.h"

#include <string>
#include <string_view>
#include <vector>

// A core-side memory searcher.
//
// The Qt debugger's searcher is not reused: it is built on QVariant, QMap and tr(), so
// lifting it would be a larger and riskier change than reimplementing the comparisons. This
// supports the same comparison set and value types, plus chained filtering, which is what
// makes it useful for finding a per-frame counter.
namespace MemorySearch
{
	enum class ValueType
	{
		U8, U16, U32, U64,
		I8, I16, I32, I64,
		F32, F64,
		Bytes
	};

	enum class Comparison
	{
		Eq, Ne, Gt, Gte, Lt, Lte,
		Increased, IncreasedBy,
		Decreased, DecreasedBy,
		Changed, ChangedBy,
		NotChanged,
		// Matches every address; how a chained search starts when the value is unknown.
		Unknown
	};

	struct Hit
	{
		u32 addr = 0;
		u64 raw = 0;       // integer reading, and the bit pattern for float types
		double as_double = 0.0;
	};

	struct Query
	{
		ValueType type = ValueType::U32;
		Comparison comparison = Comparison::Eq;
		u32 start = 0;
		u32 end = 0;
		std::vector<u8> value;      // the needle, in guest byte order
		size_t max_results = 1000;
	};

	// Scans [start, end) and collects everything matching. Reads in chunks rather than one
	// value at a time, so a 32 MiB sweep is not 8 million virtual calls.
	bool RunFirstPass(MemoryInterface& memory, const Query& query, std::vector<Hit>& out, std::string& error);

	// Re-reads only the addresses in previous and keeps the ones that still match. This is
	// what "increased by 1 again" means.
	bool RunFilterPass(MemoryInterface& memory, const Query& query, const std::vector<Hit>& previous,
		std::vector<Hit>& out, std::string& error);

	bool ParseValueType(std::string_view text, ValueType& out);
	bool ParseComparison(std::string_view text, Comparison& out);

	// Size in bytes of one value of this type; 0 for Bytes, whose width comes from the needle.
	size_t ValueSize(ValueType type);
} // namespace MemorySearch
