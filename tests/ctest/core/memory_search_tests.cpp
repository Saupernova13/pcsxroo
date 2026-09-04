// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "DebugServer/MemorySearch.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace
{
	// A flat buffer mapped at a fixed base, so the tests can state exact addresses.
	class BufferMemory final : public MemoryInterface
	{
	public:
		static constexpr u32 BASE = 0x100000;

		explicit BufferMemory(size_t size)
			: m_data(size, 0)
		{
		}

		void PokeU32(u32 addr, u32 value) { std::memcpy(&m_data[addr - BASE], &value, sizeof(value)); }
		void PokeF32(u32 addr, float value) { std::memcpy(&m_data[addr - BASE], &value, sizeof(value)); }

		bool ReadBytes(u32 address, void* dest, u32 size) override
		{
			if (address < BASE || (address - BASE) + size > m_data.size())
				return false;

			std::memcpy(dest, &m_data[address - BASE], size);
			return true;
		}

		bool WriteBytes(u32 address, const void* src, u32 size) override
		{
			if (address < BASE || (address - BASE) + size > m_data.size())
				return false;

			std::memcpy(&m_data[address - BASE], src, size);
			return true;
		}

		bool CompareBytes(u32 address, const void* src, u32 size) override
		{
			if (address < BASE || (address - BASE) + size > m_data.size())
				return false;

			return std::memcmp(&m_data[address - BASE], src, size) == 0;
		}

		// The searcher only uses ReadBytes; the rest is here to satisfy the interface.
		u8 Read8(u32 a, bool* v = nullptr) override { u8 x{}; const bool ok = ReadBytes(a, &x, 1); if (v) *v = ok; return x; }
		u16 Read16(u32 a, bool* v = nullptr) override { u16 x{}; const bool ok = ReadBytes(a, &x, 2); if (v) *v = ok; return x; }
		u32 Read32(u32 a, bool* v = nullptr) override { u32 x{}; const bool ok = ReadBytes(a, &x, 4); if (v) *v = ok; return x; }
		u64 Read64(u32 a, bool* v = nullptr) override { u64 x{}; const bool ok = ReadBytes(a, &x, 8); if (v) *v = ok; return x; }
		u128 Read128(u32 a, bool* v = nullptr) override { u128 x{}; const bool ok = ReadBytes(a, &x, 16); if (v) *v = ok; return x; }
		bool Write8(u32 a, u8 x) override { return WriteBytes(a, &x, 1); }
		bool Write16(u32 a, u16 x) override { return WriteBytes(a, &x, 2); }
		bool Write32(u32 a, u32 x) override { return WriteBytes(a, &x, 4); }
		bool Write64(u32 a, u64 x) override { return WriteBytes(a, &x, 8); }
		bool Write128(u32 a, u128 x) override { return WriteBytes(a, &x, 16); }

	private:
		std::vector<u8> m_data;
	};

	MemorySearch::Query MakeQuery(MemorySearch::Comparison comparison, u32 value)
	{
		MemorySearch::Query query;
		query.type = MemorySearch::ValueType::U32;
		query.comparison = comparison;
		query.start = BufferMemory::BASE;
		query.end = BufferMemory::BASE + 0x1000;
		query.max_results = 1000;
		query.value.resize(sizeof(value));
		std::memcpy(query.value.data(), &value, sizeof(value));
		return query;
	}
} // namespace

TEST(MemorySearch, FindsAnExactU32)
{
	BufferMemory memory(0x1000);
	memory.PokeU32(0x100010, 2);
	memory.PokeU32(0x100020, 2);
	memory.PokeU32(0x100030, 3);

	std::vector<MemorySearch::Hit> hits;
	std::string error;
	ASSERT_TRUE(MemorySearch::RunFirstPass(memory, MakeQuery(MemorySearch::Comparison::Eq, 2), hits, error))
		<< error;

	ASSERT_EQ(hits.size(), 2u);
	EXPECT_EQ(hits[0].addr, 0x100010u);
	EXPECT_EQ(hits[1].addr, 0x100020u);
}

TEST(MemorySearch, RespectsMaxResults)
{
	BufferMemory memory(0x1000);
	for (u32 addr = BufferMemory::BASE; addr < BufferMemory::BASE + 0x40; addr += 4)
		memory.PokeU32(addr, 7);

	MemorySearch::Query query = MakeQuery(MemorySearch::Comparison::Eq, 7);
	query.max_results = 4;

	std::vector<MemorySearch::Hit> hits;
	std::string error;
	ASSERT_TRUE(MemorySearch::RunFirstPass(memory, query, hits, error)) << error;
	EXPECT_EQ(hits.size(), 4u);
}

// The chained case, which is what makes this useful for finding a per-frame counter.
TEST(MemorySearch, FilterPassNarrowsToAddressesThatIncreasedByOne)
{
	BufferMemory memory(0x1000);
	memory.PokeU32(0x100010, 10);
	memory.PokeU32(0x100020, 10);

	std::vector<MemorySearch::Hit> pass1;
	std::string error;
	ASSERT_TRUE(
		MemorySearch::RunFirstPass(memory, MakeQuery(MemorySearch::Comparison::Unknown, 0), pass1, error))
		<< error;
	ASSERT_FALSE(pass1.empty());

	memory.PokeU32(0x100010, 11); // increased by 1
	memory.PokeU32(0x100020, 10); // unchanged

	std::vector<MemorySearch::Hit> pass2;
	ASSERT_TRUE(MemorySearch::RunFilterPass(
		memory, MakeQuery(MemorySearch::Comparison::IncreasedBy, 1), pass1, pass2, error))
		<< error;

	ASSERT_EQ(pass2.size(), 1u);
	EXPECT_EQ(pass2[0].addr, 0x100010u);
}

TEST(MemorySearch, FilterPassFindsWhatChangedAtAll)
{
	BufferMemory memory(0x1000);
	memory.PokeU32(0x100010, 1);
	memory.PokeU32(0x100020, 1);

	std::vector<MemorySearch::Hit> pass1;
	std::string error;
	ASSERT_TRUE(
		MemorySearch::RunFirstPass(memory, MakeQuery(MemorySearch::Comparison::Unknown, 0), pass1, error));

	memory.PokeU32(0x100020, 99);

	std::vector<MemorySearch::Hit> pass2;
	ASSERT_TRUE(MemorySearch::RunFilterPass(
		memory, MakeQuery(MemorySearch::Comparison::Changed, 0), pass1, pass2, error));

	ASSERT_EQ(pass2.size(), 1u);
	EXPECT_EQ(pass2[0].addr, 0x100020u);
}

TEST(MemorySearch, FilterPassKeepsWhatDidNotChange)
{
	BufferMemory memory(0x1000);
	memory.PokeU32(0x100010, 5);
	memory.PokeU32(0x100020, 5);

	std::vector<MemorySearch::Hit> pass1;
	std::string error;
	ASSERT_TRUE(
		MemorySearch::RunFirstPass(memory, MakeQuery(MemorySearch::Comparison::Unknown, 0), pass1, error));

	memory.PokeU32(0x100010, 6);

	std::vector<MemorySearch::Hit> pass2;
	ASSERT_TRUE(MemorySearch::RunFilterPass(
		memory, MakeQuery(MemorySearch::Comparison::NotChanged, 0), pass1, pass2, error));

	const bool kept_unchanged =
		std::any_of(pass2.begin(), pass2.end(), [](const MemorySearch::Hit& h) { return h.addr == 0x100020u; });
	const bool dropped_changed =
		std::none_of(pass2.begin(), pass2.end(), [](const MemorySearch::Hit& h) { return h.addr == 0x100010u; });

	EXPECT_TRUE(kept_unchanged);
	EXPECT_TRUE(dropped_changed);
}

TEST(MemorySearch, MatchesFloats)
{
	BufferMemory memory(0x1000);
	memory.PokeF32(0x100010, 2.0f);
	memory.PokeF32(0x100020, 1.0f);

	MemorySearch::Query query;
	query.type = MemorySearch::ValueType::F32;
	query.comparison = MemorySearch::Comparison::Eq;
	query.start = BufferMemory::BASE;
	query.end = BufferMemory::BASE + 0x1000;
	query.max_results = 1000;
	const float needle = 2.0f;
	query.value.resize(sizeof(needle));
	std::memcpy(query.value.data(), &needle, sizeof(needle));

	std::vector<MemorySearch::Hit> hits;
	std::string error;
	ASSERT_TRUE(MemorySearch::RunFirstPass(memory, query, hits, error)) << error;
	ASSERT_EQ(hits.size(), 1u);
	EXPECT_EQ(hits[0].addr, 0x100010u);
}

TEST(MemorySearch, RejectsAnInvertedRange)
{
	BufferMemory memory(0x1000);
	MemorySearch::Query query = MakeQuery(MemorySearch::Comparison::Eq, 1);
	query.start = BufferMemory::BASE + 0x100;
	query.end = BufferMemory::BASE;

	std::vector<MemorySearch::Hit> hits;
	std::string error;
	EXPECT_FALSE(MemorySearch::RunFirstPass(memory, query, hits, error));
	EXPECT_FALSE(error.empty());
}

// A delta comparison has nothing to compare against on a first pass, and saying so beats
// silently returning nothing.
TEST(MemorySearch, RejectsADeltaComparisonOnTheFirstPass)
{
	BufferMemory memory(0x1000);
	std::vector<MemorySearch::Hit> hits;
	std::string error;

	EXPECT_FALSE(
		MemorySearch::RunFirstPass(memory, MakeQuery(MemorySearch::Comparison::Increased, 0), hits, error));
	EXPECT_FALSE(error.empty());
}

TEST(MemorySearch, ParsesProtocolNames)
{
	MemorySearch::ValueType type;
	ASSERT_TRUE(MemorySearch::ParseValueType("u32", type));
	EXPECT_EQ(type, MemorySearch::ValueType::U32);
	ASSERT_TRUE(MemorySearch::ParseValueType("f64", type));
	EXPECT_EQ(type, MemorySearch::ValueType::F64);
	EXPECT_FALSE(MemorySearch::ParseValueType("u24", type));

	MemorySearch::Comparison comparison;
	ASSERT_TRUE(MemorySearch::ParseComparison("increased_by", comparison));
	EXPECT_EQ(comparison, MemorySearch::Comparison::IncreasedBy);
	ASSERT_TRUE(MemorySearch::ParseComparison("unknown", comparison));
	EXPECT_EQ(comparison, MemorySearch::Comparison::Unknown);
	EXPECT_FALSE(MemorySearch::ParseComparison("sideways", comparison));
}
