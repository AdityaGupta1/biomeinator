#include "util/rng.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

TEST_CASE("hash output is stable for representative seeds", "[unit][rng]")
{
    CHECK(hash(0u) == 129708002u);
    CHECK(hash(1u) == 2831084092u);
    CHECK(hash(42u) == 1223963391u);
    CHECK(hash(UINT32_MAX) == 3861530882u);
}

TEST_CASE("RNG produces a stable deterministic sequence", "[unit][rng]")
{
    constexpr std::array expected{
        1223963391u, 2785308739u, 4004461565u, 2892551605u, 3669186258u,
    };

    RandomNumberGenerator first = initRng(42u);
    RandomNumberGenerator second = initRng(42u);
    for (const uint32_t value : expected)
    {
        CHECK(first.nextUint() == value);
        CHECK(second.nextUint() == value);
    }
}

TEST_CASE("multi-part RNG seeds follow the documented hash folding", "[unit][rng]")
{
    CHECK(initRng(10u, 20u).seed == (10u ^ hash(20u)));
    CHECK(initRng(10u, 20u, 30u).seed == (10u ^ hash(20u ^ hash(30u))));
    CHECK(initRng(10u, 20u, 30u, 40u).seed == (10u ^ hash(20u ^ hash(30u ^ hash(40u)))));
}

TEST_CASE("RNG range helpers stay within their half-open intervals", "[unit][rng]")
{
    RandomNumberGenerator rng = initRng(987654321u);
    for (int i = 0; i < 10000; ++i)
    {
        const float unit = rng.nextFloat();
        CHECK(unit >= 0.f);
        CHECK(unit < 1.f);

        const float scaled = rng.nextFloat(-3.5f, 7.25f);
        CHECK(scaled >= -3.5f);
        CHECK(scaled < 7.25f);

        const int integer = rng.nextInt(-8, 13);
        CHECK(integer >= -8);
        CHECK(integer < 13);
    }
}

TEST_CASE("RNG probability endpoints are exact", "[unit][rng]")
{
    RandomNumberGenerator rng = initRng(123u);
    for (int i = 0; i < 100; ++i)
    {
        CHECK_FALSE(rng.chance(0.f));
        CHECK(rng.chance(1.f));
    }
}
