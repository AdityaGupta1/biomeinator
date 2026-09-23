#include "util/math.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>

TEST_CASE("floorDiv rounds toward negative infinity", "[unit][math]")
{
    struct Case
    {
        int numerator;
        int denominator;
        int expected;
    };

    constexpr std::array cases{
        Case{ 0, 16, 0 },    Case{ 15, 16, 0 },   Case{ 16, 16, 1 },   Case{ 17, 16, 1 }, Case{ -1, 16, -1 },
        Case{ -15, 16, -1 }, Case{ -16, 16, -1 }, Case{ -17, 16, -2 }, Case{ 7, 3, 2 },   Case{ -7, 3, -3 },
    };

    for (const Case& testCase : cases)
    {
        CAPTURE(testCase.numerator, testCase.denominator);
        CHECK(MathUtil::floorDiv(testCase.numerator, testCase.denominator) == testCase.expected);
    }
}

TEST_CASE("isPowerOfTwo recognizes only positive powers of two", "[unit][math]")
{
    CHECK_FALSE(MathUtil::isPowerOfTwo(0));
    CHECK(MathUtil::isPowerOfTwo(1));
    CHECK(MathUtil::isPowerOfTwo(2));
    CHECK_FALSE(MathUtil::isPowerOfTwo(3));
    CHECK(MathUtil::isPowerOfTwo(1024));
    CHECK_FALSE(MathUtil::isPowerOfTwo(1025));
}

TEST_CASE("rounding helpers preserve aligned values and round up", "[unit][math]")
{
    CHECK(MathUtil::roundUpToPow2(0, 8) == 0);
    CHECK(MathUtil::roundUpToPow2(1, 8) == 8);
    CHECK(MathUtil::roundUpToPow2(8, 8) == 8);
    CHECK(MathUtil::roundUpToPow2(9, 8) == 16);

    CHECK(MathUtil::roundUp(0, 6) == 0);
    CHECK(MathUtil::roundUp(1, 6) == 6);
    CHECK(MathUtil::roundUp(6, 6) == 6);
    CHECK(MathUtil::roundUp(7, 6) == 12);
}
