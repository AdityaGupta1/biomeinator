#include "util/ring_buffer.h"

#include <catch2/catch_test_macros.hpp>

#include <array>

TEST_CASE("RingBuffer starts empty and reports its fixed capacity", "[unit][ring_buffer]")
{
    RingBuffer<int, 3> buffer;

    CHECK(buffer.getSize() == 0);
    CHECK(buffer.getOffset() == 0);
    CHECK(buffer.getMaxSize() == 3);
}

TEST_CASE("RingBuffer advances and wraps its write offset", "[unit][ring_buffer]")
{
    RingBuffer<int, 3> buffer;

    buffer.push(10);
    CHECK(buffer.getData()[0] == 10);
    CHECK(buffer.getSize() == 1);
    CHECK(buffer.getOffset() == 1);

    buffer.push(20);
    buffer.push(30);
    CHECK(buffer.getData() == std::array{ 10, 20, 30 });
    CHECK(buffer.getSize() == 3);
    CHECK(buffer.getOffset() == 0);

    buffer.push(40);
    CHECK(buffer.getData() == std::array{ 40, 20, 30 });
    CHECK(buffer.getSize() == 3);
    CHECK(buffer.getOffset() == 1);

    buffer.push(50);
    CHECK(buffer.getData() == std::array{ 40, 50, 30 });
    CHECK(buffer.getSize() == 3);
    CHECK(buffer.getOffset() == 2);
}

TEST_CASE("RingBuffer clear resets occupancy and the next write position", "[unit][ring_buffer]")
{
    RingBuffer<int, 2> buffer;
    buffer.push(1);
    buffer.push(2);
    buffer.push(3);

    buffer.clear();
    CHECK(buffer.getSize() == 0);
    CHECK(buffer.getOffset() == 0);

    buffer.push(9);
    CHECK(buffer.getData()[0] == 9);
    CHECK(buffer.getSize() == 1);
    CHECK(buffer.getOffset() == 1);
}
