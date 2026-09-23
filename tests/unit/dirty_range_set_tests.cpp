#include "rendering/buffer/dirty_range_set.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <random>
#include <vector>

TEST_CASE("DirtyRangeSet rejects empty and reversed ranges", "[unit][dirty_range_set]")
{
    DirtyRangeSet ranges;
    CHECK_FALSE(ranges.insert(4, 4));
    CHECK_FALSE(ranges.insert(8, 3));
    CHECK(ranges.empty());
    CHECK(ranges.validateInvariants());
}

TEST_CASE("DirtyRangeSet keeps disjoint ranges sorted", "[unit][dirty_range_set]")
{
    DirtyRangeSet ranges;
    REQUIRE(ranges.insert(20, 25));
    REQUIRE(ranges.insert(2, 5));
    REQUIRE(ranges.insert(10, 15));

    CHECK(ranges.getRanges() == (std::vector<DirtyRange>{ { 2, 5 }, { 10, 15 }, { 20, 25 } }));
    CHECK(ranges.validateInvariants());
}

TEST_CASE("DirtyRangeSet merges adjacency overlap nesting and bridges", "[unit][dirty_range_set]")
{
    DirtyRangeSet ranges;
    REQUIRE(ranges.insert(10, 20));
    REQUIRE(ranges.insert(30, 40));
    REQUIRE(ranges.insert(12, 18));
    CHECK(ranges.getRanges() == (std::vector<DirtyRange>{ { 10, 20 }, { 30, 40 } }));

    REQUIRE(ranges.insert(20, 24));
    CHECK(ranges.getRanges() == (std::vector<DirtyRange>{ { 10, 24 }, { 30, 40 } }));

    REQUIRE(ranges.insert(23, 31));
    CHECK(ranges.getRanges() == (std::vector<DirtyRange>{ { 10, 40 } }));
    CHECK(ranges.validateInvariants());
}

TEST_CASE("DirtyRangeSet result is independent of insertion order", "[unit][dirty_range_set]")
{
    constexpr std::array input{
        DirtyRange{ 10, 20 }, DirtyRange{ 30, 40 }, DirtyRange{ 18, 32 }, DirtyRange{ 5, 8 }, DirtyRange{ 8, 10 },
    };
    std::array<size_t, input.size()> order{ 0, 1, 2, 3, 4 };

    do
    {
        DirtyRangeSet ranges;
        for (const size_t index : order)
        {
            REQUIRE(ranges.insert(input[index].begin, input[index].end));
        }
        CHECK(ranges.getRanges() == (std::vector<DirtyRange>{ { 5, 40 } }));
        REQUIRE(ranges.validateInvariants());
    } while (std::next_permutation(order.begin(), order.end()));
}

TEST_CASE("DirtyRangeSet matches a byte-mask model under deterministic churn", "[unit][dirty_range_set][stress]")
{
    constexpr uint32_t capacity = 1024;
    constexpr uint32_t seed = 0xD1721234u;
    DirtyRangeSet ranges;
    std::vector<uint8_t> dirty(capacity, 0);
    std::mt19937 rng(seed);
    INFO("seed=" << seed);

    for (int operation = 0; operation < 5000; ++operation)
    {
        const uint32_t first = rng() % capacity;
        const uint32_t second = rng() % capacity;
        const uint32_t begin = std::min(first, second);
        const uint32_t end = std::max(first, second) + 1;
        REQUIRE(ranges.insert(begin, end));
        std::fill(dirty.begin() + begin, dirty.begin() + end, 1);
        REQUIRE(ranges.validateInvariants());
    }

    std::vector<DirtyRange> expected;
    uint32_t index = 0;
    while (index < capacity)
    {
        while (index < capacity && dirty[index] == 0)
        {
            ++index;
        }
        const uint32_t begin = index;
        while (index < capacity && dirty[index] != 0)
        {
            ++index;
        }
        if (begin < index)
        {
            expected.push_back({ begin, index });
        }
    }
    CHECK(ranges.getRanges() == expected);

    ranges.clear();
    CHECK(ranges.empty());
    CHECK(ranges.validateInvariants());
}
