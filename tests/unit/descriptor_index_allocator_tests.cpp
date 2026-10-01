#include "rendering/buffer/descriptor_index_allocator.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

TEST_CASE("DescriptorIndexAllocator allocates sequential handles through exhaustion",
          "[unit][descriptor_index_allocator]")
{
    DescriptorIndexAllocator allocator;
    allocator.reset(4, 1000, 5000, 32);

    for (uint32_t index = 0; index < 4; ++index)
    {
        const auto allocation = allocator.allocate();
        REQUIRE(allocation);
        CHECK(allocation->index == index);
        CHECK(allocation->cpuHandle == 1000 + index * 32);
        CHECK(allocation->gpuHandle == 5000 + index * 32);
    }

    CHECK_FALSE(allocator.allocate());
    CHECK(allocator.getFreeCount() == 0);
    CHECK(allocator.validateInvariants());
}

TEST_CASE("DescriptorIndexAllocator reuses released indices in LIFO order", "[unit][descriptor_index_allocator]")
{
    DescriptorIndexAllocator allocator;
    allocator.reset(4, 1000, 5000, 32);
    for (int i = 0; i < 4; ++i)
    {
        REQUIRE(allocator.allocate());
    }

    REQUIRE(allocator.release(1));
    REQUIRE(allocator.release(3));
    const auto firstReuse = allocator.allocate();
    const auto secondReuse = allocator.allocate();
    REQUIRE(firstReuse);
    REQUIRE(secondReuse);
    CHECK(firstReuse->index == 3);
    CHECK(secondReuse->index == 1);
    CHECK(allocator.validateInvariants());
}

TEST_CASE("DescriptorIndexAllocator validates paired handles", "[unit][descriptor_index_allocator]")
{
    DescriptorIndexAllocator allocator;
    allocator.reset(3, 1024, 4096, 16);
    const auto allocation = allocator.allocate();
    REQUIRE(allocation);

    CHECK_FALSE(allocator.release(allocation->cpuHandle + 1, allocation->gpuHandle));
    CHECK_FALSE(allocator.release(allocation->cpuHandle, allocation->gpuHandle + 16));
    CHECK_FALSE(allocator.release(1000, allocation->gpuHandle));
    CHECK_FALSE(allocator.release(allocation->cpuHandle, 4080));
    CHECK(allocator.getFreeCount() == 2);

    REQUIRE(allocator.release(allocation->cpuHandle, allocation->gpuHandle));
    CHECK(allocator.getFreeCount() == 3);
    CHECK(allocator.validateInvariants());
}

TEST_CASE("DescriptorIndexAllocator rejects invalid and duplicate releases", "[unit][descriptor_index_allocator]")
{
    DescriptorIndexAllocator allocator;
    allocator.reset(2, 100, 200, 8);
    CHECK_FALSE(allocator.release(0));
    CHECK_FALSE(allocator.release(2));
    CHECK_FALSE(allocator.release(UINT32_MAX));

    const auto allocation = allocator.allocate();
    REQUIRE(allocation);
    REQUIRE(allocator.release(allocation->index));
    CHECK_FALSE(allocator.release(allocation->index));
    CHECK(allocator.getFreeCount() == 2);
    CHECK(allocator.validateInvariants());
}

TEST_CASE("DescriptorIndexAllocator survives deterministic allocation churn",
          "[unit][descriptor_index_allocator][stress]")
{
    constexpr uint32_t capacity = 257;
    constexpr uint32_t seed = 0xD35C1234u;
    DescriptorIndexAllocator allocator;
    allocator.reset(capacity, 0x100000, 0x800000, 32);
    std::vector<uint8_t> liveMask(capacity, 0);
    std::vector<DescriptorIndexAllocation> live;
    std::mt19937 rng(seed);
    INFO("seed=" << seed);

    for (int operation = 0; operation < 20000; ++operation)
    {
        const bool shouldAllocate = live.empty() || (live.size() < capacity && rng() % 100 < 60);
        if (shouldAllocate)
        {
            const auto allocation = allocator.allocate();
            REQUIRE(allocation);
            REQUIRE(allocation->index < capacity);
            REQUIRE(liveMask[allocation->index] == 0);
            liveMask[allocation->index] = 1;
            live.push_back(*allocation);
        }
        else
        {
            const size_t liveIndex = rng() % live.size();
            const DescriptorIndexAllocation allocation = live[liveIndex];
            const bool releaseByHandles = (rng() & 1u) != 0;
            const bool didRelease = releaseByHandles ? allocator.release(allocation.cpuHandle, allocation.gpuHandle)
                                                     : allocator.release(allocation.index);
            REQUIRE(didRelease);
            REQUIRE(liveMask[allocation.index] == 1);
            liveMask[allocation.index] = 0;
            live[liveIndex] = live.back();
            live.pop_back();
        }

        REQUIRE(allocator.validateInvariants());
        CHECK(allocator.getFreeCount() == capacity - live.size());
    }

    std::shuffle(live.begin(), live.end(), rng);
    for (const DescriptorIndexAllocation allocation : live)
    {
        REQUIRE(allocator.release(allocation.index));
    }
    CHECK(allocator.getFreeCount() == capacity);
    CHECK(allocator.validateInvariants());
}
