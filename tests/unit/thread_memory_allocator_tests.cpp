#include "multithreading/thread_memory_allocator.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace
{

struct alignas(16) AlignedValue
{
    std::array<uint32_t, 4> values;
};

} // namespace

TEST_CASE("ThreadMemoryAllocator aligns allocations for their element type", "[unit][thread_memory_allocator]")
{
    ThreadMemoryAllocator allocator;
    allocator.request<uint8_t>(1);
    const uint16_t* words = allocator.request<uint16_t>(2);
    const uint64_t* integers = allocator.request<uint64_t>(2);
    const AlignedValue* aligned = allocator.request<AlignedValue>(1);

    CHECK(reinterpret_cast<uintptr_t>(words) % alignof(uint16_t) == 0);
    CHECK(reinterpret_cast<uintptr_t>(integers) % alignof(uint64_t) == 0);
    CHECK(reinterpret_cast<uintptr_t>(aligned) % alignof(AlignedValue) == 0);
}

TEST_CASE("ThreadMemoryAllocator keeps earlier pointers alive across growth", "[unit][thread_memory_allocator]")
{
    ThreadMemoryAllocator allocator;
    uint32_t* original = allocator.request<uint32_t>(32);
    for (uint32_t index = 0; index < 32; ++index)
    {
        original[index] = 0xABCD0000u + index;
    }

    uint8_t* large = allocator.request<uint8_t>(200000);
    std::fill_n(large, 200000, uint8_t{ 0x5A });

    for (uint32_t index = 0; index < 32; ++index)
    {
        CHECK(original[index] == 0xABCD0000u + index);
    }
    CHECK(large[0] == 0x5A);
    CHECK(large[199999] == 0x5A);
}

TEST_CASE("ThreadMemoryAllocator clear reuses the current backing buffer from offset zero",
          "[unit][thread_memory_allocator]")
{
    ThreadMemoryAllocator allocator;
    allocator.request<uint8_t>(70000);
    uint64_t* beforeClear = allocator.request<uint64_t>(4);

    allocator.clear();
    uint64_t* afterClear = allocator.request<uint64_t>(4);

    CHECK(afterClear < beforeClear);
    CHECK(reinterpret_cast<uintptr_t>(afterClear) % alignof(uint64_t) == 0);
}

TEST_CASE("ThreadMemoryAllocator survives alternating growth and reuse cycles",
          "[unit][thread_memory_allocator][stress]")
{
    ThreadMemoryAllocator allocator;
    constexpr std::array<size_t, 6> sizes{ 1, 1024, 70000, 17, 300000, 65536 };

    for (int cycle = 0; cycle < 100; ++cycle)
    {
        std::vector<std::pair<uint8_t*, size_t>> allocations;
        for (const size_t size : sizes)
        {
            uint8_t* allocation = allocator.request<uint8_t>(size);
            allocation[0] = static_cast<uint8_t>(cycle);
            allocation[size - 1] = static_cast<uint8_t>(cycle);
            allocations.emplace_back(allocation, size);
        }

        for (const auto& [allocation, size] : allocations)
        {
            CHECK(allocation[0] == static_cast<uint8_t>(cycle));
            CHECK(allocation[size - 1] == static_cast<uint8_t>(cycle));
        }
        allocator.clear();
    }
}

TEST_CASE("ThreadMemoryAllocator preserves randomized aligned allocations until clear",
          "[unit][thread_memory_allocator][stress]")
{
    struct Allocation
    {
        uint8_t* bytes;
        size_t sizeBytes;
        uint8_t marker;
    };

    constexpr uint32_t seed = 0x7A110C8u;
    ThreadMemoryAllocator allocator;
    std::mt19937 rng(seed);
    INFO("seed=" << seed);

    for (int cycle = 0; cycle < 200; ++cycle)
    {
        allocator.clear();
        std::vector<Allocation> allocations;
        const int allocationCount = 1 + rng() % 48;
        allocations.reserve(allocationCount);

        for (int allocationIndex = 0; allocationIndex < allocationCount; ++allocationIndex)
        {
            size_t requestedBytes = 1 + rng() % 8192;
            if (rng() % 20 == 0)
            {
                requestedBytes += 65536 + rng() % 131072;
            }
            const uint8_t marker = static_cast<uint8_t>((cycle * 53 + allocationIndex * 17) % 251 + 1);

            const auto makeAllocation = [&]<typename T>()
            {
                const size_t count = (requestedBytes + sizeof(T) - 1) / sizeof(T);
                T* const ptr = allocator.request<T>(count);
                REQUIRE(reinterpret_cast<uintptr_t>(ptr) % alignof(T) == 0);

                const size_t sizeBytes = count * sizeof(T);
                uint8_t* const bytes = reinterpret_cast<uint8_t*>(ptr);
                std::fill_n(bytes, sizeBytes, marker);
                allocations.push_back({ bytes, sizeBytes, marker });
            };

            switch (rng() % 4)
            {
                case 0:
                    makeAllocation.template operator()<uint8_t>();
                    break;
                case 1:
                    makeAllocation.template operator()<uint32_t>();
                    break;
                case 2:
                    makeAllocation.template operator()<uint64_t>();
                    break;
                default:
                    makeAllocation.template operator()<AlignedValue>();
                    break;
            }
        }

        CAPTURE(cycle, allocationCount);
        for (const Allocation& allocation : allocations)
        {
            CHECK(std::all_of(allocation.bytes,
                              allocation.bytes + allocation.sizeBytes,
                              [&](uint8_t byte) { return byte == allocation.marker; }));
        }
    }
}
