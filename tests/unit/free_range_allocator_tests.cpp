#include "rendering/buffer/free_range_allocator.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <random>
#include <vector>

namespace
{

size_t countFreeBytes(const std::vector<uint8_t>& occupied)
{
    return static_cast<size_t>(std::count(occupied.begin(), occupied.end(), uint8_t{ 0 }));
}

size_t largestFreeRun(const std::vector<uint8_t>& occupied)
{
    size_t largest = 0;
    size_t current = 0;
    for (const uint8_t byte : occupied)
    {
        if (byte == 0)
        {
            largest = std::max(largest, ++current);
        }
        else
        {
            current = 0;
        }
    }
    return largest;
}

} // namespace

TEST_CASE("FreeRangeAllocator aligns allocations and reports exhaustion", "[unit][free_range_allocator]")
{
    FreeRangeAllocator allocator(8);
    allocator.reset(32);

    REQUIRE(allocator.validateInvariants());
    CHECK(allocator.getCapacityBytes() == 32);
    CHECK(allocator.getFreeBytes() == 32);

    const auto first = allocator.allocate(1);
    const auto second = allocator.allocate(9);
    const auto third = allocator.allocate(8);
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(third);
    CHECK(*first == (FreeRange{ 0, 8 }));
    CHECK(*second == (FreeRange{ 8, 16 }));
    CHECK(*third == (FreeRange{ 24, 8 }));
    CHECK_FALSE(allocator.allocate(1));
    CHECK(allocator.getFreeBytes() == 0);
    CHECK(allocator.validateInvariants());
}

TEST_CASE("FreeRangeAllocator coalesces every free order back into one range", "[unit][free_range_allocator]")
{
    constexpr std::array freeOrders{
        std::array{ 0, 1, 2, 3 },
        std::array{ 3, 2, 1, 0 },
        std::array{ 1, 3, 0, 2 },
    };

    for (const auto& order : freeOrders)
    {
        FreeRangeAllocator allocator;
        allocator.reset(64);
        std::array<FreeRange, 4> allocations;
        for (FreeRange& allocation : allocations)
        {
            const auto range = allocator.allocate(16);
            REQUIRE(range);
            allocation = *range;
        }

        for (const int index : order)
        {
            REQUIRE(allocator.release(allocations[index]));
            REQUIRE(allocator.validateInvariants());
        }

        CHECK(allocator.isCompletelyFree());
        CHECK(allocator.getFreeBytes() == 64);
        const auto wholeBuffer = allocator.allocate(64);
        REQUIRE(wholeBuffer);
        CHECK(*wholeBuffer == (FreeRange{ 0, 64 }));
    }
}

TEST_CASE("FreeRangeAllocator uses the smallest adequate free range", "[unit][free_range_allocator]")
{
    FreeRangeAllocator allocator(8);
    allocator.reset(160);

    const auto first = allocator.allocate(16);
    const auto smallHole = allocator.allocate(40);
    const auto separator = allocator.allocate(16);
    const auto largeHole = allocator.allocate(64);
    REQUIRE(first);
    REQUIRE(smallHole);
    REQUIRE(separator);
    REQUIRE(largeHole);

    REQUIRE(allocator.release(*smallHole));
    REQUIRE(allocator.release(*largeHole));
    const auto bestFit = allocator.allocate(33);
    REQUIRE(bestFit);
    CHECK(*bestFit == (FreeRange{ 16, 40 }));
    CHECK(allocator.validateInvariants());
}

TEST_CASE("FreeRangeAllocator growth extends or creates the trailing free range", "[unit][free_range_allocator]")
{
    SECTION("existing tail is extended")
    {
        FreeRangeAllocator allocator;
        allocator.reset(64);
        REQUIRE(allocator.allocate(32));
        REQUIRE(allocator.grow(128));
        CHECK(allocator.getFreeTailBytes() == 96);

        const auto tail = allocator.allocate(96);
        REQUIRE(tail);
        CHECK(*tail == (FreeRange{ 32, 96 }));
    }

    SECTION("a full allocator gains a new tail")
    {
        FreeRangeAllocator allocator;
        allocator.reset(64);
        REQUIRE(allocator.allocate(64));
        REQUIRE(allocator.grow(96));
        CHECK(allocator.getFreeTailBytes() == 32);

        const auto tail = allocator.allocate(32);
        REQUIRE(tail);
        CHECK(*tail == (FreeRange{ 64, 32 }));
    }
}

TEST_CASE("FreeRangeAllocator preserves alignment with non-multiple capacities", "[unit][free_range_allocator]")
{
    FreeRangeAllocator allocator(12);
    allocator.reset(64);
    REQUIRE(allocator.validateInvariants());

    const auto allocation = allocator.allocate(1);
    REQUIRE(allocation);
    CHECK(*allocation == (FreeRange{ 0, 12 }));
    CHECK(allocator.getFreeTailBytes() == 52);

    REQUIRE(allocator.grow(100));
    CHECK(allocator.getFreeTailBytes() == 88);
    REQUIRE(allocator.validateInvariants());

    REQUIRE(allocator.release(*allocation));
    CHECK(allocator.isCompletelyFree());
    CHECK(allocator.validateInvariants());
}

TEST_CASE("FreeRangeAllocator rejects invalid and duplicate releases without corruption",
          "[unit][free_range_allocator]")
{
    FreeRangeAllocator allocator;
    allocator.reset(64);
    const auto allocation = allocator.allocate(16);
    REQUIRE(allocation);

    CHECK_FALSE(allocator.release({ 0, 0 }));
    CHECK_FALSE(allocator.release({ 63, 2 }));
    CHECK_FALSE(allocator.release({ 32, 40 }));
    REQUIRE(allocator.release(*allocation));
    CHECK_FALSE(allocator.release(*allocation));
    CHECK_FALSE(allocator.grow(32));

    CHECK(allocator.validateInvariants());
    CHECK(allocator.isCompletelyFree());

    FreeRangeAllocator alignedAllocator(8);
    alignedAllocator.reset(64);
    const auto alignedAllocation = alignedAllocator.allocate(16);
    REQUIRE(alignedAllocation);
    CHECK_FALSE(alignedAllocator.release({ 1, 8 }));
    CHECK_FALSE(alignedAllocator.release({ 0, 7 }));
    REQUIRE(alignedAllocator.release(*alignedAllocation));
    CHECK(alignedAllocator.validateInvariants());
    CHECK(alignedAllocator.isCompletelyFree());
}

TEST_CASE("FreeRangeAllocator survives deterministic allocation churn", "[unit][free_range_allocator][stress]")
{
    constexpr size_t capacity = 4096;
    constexpr size_t alignment = 8;
    constexpr uint32_t seed = 0xB10C1234u;

    FreeRangeAllocator allocator(alignment);
    allocator.reset(capacity);
    std::vector<uint8_t> occupied(capacity, 0);
    std::vector<FreeRange> live;
    std::mt19937 rng(seed);
    INFO("seed=" << seed);

    for (int operation = 0; operation < 20000; ++operation)
    {
        const bool shouldAllocate = live.empty() || rng() % 100 < 62;
        if (shouldAllocate)
        {
            const size_t requestedSize = 1 + rng() % 160;
            const size_t allocationSize = allocator.getAllocationSize(requestedSize);
            const std::optional<FreeRange> range = allocator.allocate(requestedSize);
            if (range)
            {
                REQUIRE(range->offsetBytes % alignment == 0);
                REQUIRE(range->sizeBytes == allocationSize);
                REQUIRE(range->offsetBytes + range->sizeBytes <= capacity);
                for (size_t byte = range->offsetBytes; byte < range->offsetBytes + range->sizeBytes; ++byte)
                {
                    REQUIRE(occupied[byte] == 0);
                    occupied[byte] = 1;
                }
                live.push_back(*range);
            }
            else
            {
                CHECK(largestFreeRun(occupied) < allocationSize);
            }
        }
        else
        {
            const size_t liveIndex = rng() % live.size();
            const FreeRange range = live[liveIndex];
            REQUIRE(allocator.release(range));
            for (size_t byte = range.offsetBytes; byte < range.offsetBytes + range.sizeBytes; ++byte)
            {
                REQUIRE(occupied[byte] == 1);
                occupied[byte] = 0;
            }
            live[liveIndex] = live.back();
            live.pop_back();
        }

        REQUIRE(allocator.validateInvariants());
        CHECK(allocator.getFreeBytes() == countFreeBytes(occupied));
    }

    std::shuffle(live.begin(), live.end(), rng);
    for (const FreeRange range : live)
    {
        REQUIRE(allocator.release(range));
    }
    CHECK(allocator.validateInvariants());
    CHECK(allocator.isCompletelyFree());
    CHECK(allocator.getFreeBytes() == capacity);
}
