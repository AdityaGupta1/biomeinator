#include "util/rng.h"

#include <catch2/catch_approx.hpp>
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
    // Fixed seed-42 answers anchor the helpers independently of nextUint/nextFloat.
    constexpr std::array expectedStates{
        1223963391u, 2785308739u, 4004461565u, 2892551605u, 3669186258u,
    };
    constexpr std::array expectedUnits{
        0.9539031386375427f, 0.01733797788619995f, 0.6845090985298157f,
        0.40951091051101685f, 0.7005435228347778f,
    };
    constexpr std::array expectedScaled{ 6.7544587f, -3.3136167f, 3.8584728f, 0.9022423f, 4.0308429f };
    constexpr std::array expectedPositive{ 6, 0, 4, 2, 4 };
    constexpr std::array expectedNegative{ -4, -8, -5, -6, -5 };
    constexpr std::array expectedCrossingZero{ 12, -8, 6, 0, 6 };
    auto unitRng = initRng(42u);
    auto maxRng = initRng(42u);
    auto scaledRng = initRng(42u);
    auto positiveRng = initRng(42u);
    auto negativeRng = initRng(42u);
    auto crossingRng = initRng(42u);
    auto singletonRng = initRng(42u);
    for (size_t sample = 0; sample < expectedStates.size(); ++sample)
    {
        CAPTURE(sample);
        CHECK(unitRng.nextFloat() == expectedUnits[sample]);
        CHECK(maxRng.nextFloat(8.f) == expectedUnits[sample] * 8.f);
        CHECK(scaledRng.nextFloat(-3.5f, 7.25f) ==
              Catch::Approx(expectedScaled[sample]).epsilon(0).margin(1e-6));
        CHECK(positiveRng.nextInt(7) == expectedPositive[sample]);
        CHECK(negativeRng.nextInt(-8, -3) == expectedNegative[sample]);
        CHECK(crossingRng.nextInt(-8, 13) == expectedCrossingZero[sample]);
        CHECK(singletonRng.nextInt(-1, 0) == -1);
        for (const auto* helperRng : { &unitRng, &maxRng, &scaledRng, &positiveRng, &negativeRng,
                                      &crossingRng, &singletonRng })
        {
            CHECK(helperRng->seed == expectedStates[sample]);
        }
    }

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

        const int negative = rng.nextInt(-13, -8);
        CHECK(negative >= -13);
        CHECK(negative < -8);
        CHECK(rng.nextInt(-1, 0) == -1);
        CHECK(rng.nextInt(0, 1) == 0);
    }
}

TEST_CASE("RNG probability endpoints are exact", "[unit][rng]")
{
    constexpr std::array expected{ false, true, false, true, false };
    constexpr std::array expectedStates{
        1223963391u, 2785308739u, 4004461565u, 2892551605u, 3669186258u,
    };
    auto interiorRng = initRng(42u);
    for (size_t sample = 0; sample < expected.size(); ++sample)
    {
        CHECK(interiorRng.chance(0.5f) == expected[sample]);
        CHECK(interiorRng.seed == expectedStates[sample]);
    }

    RandomNumberGenerator rng = initRng(123u);
    for (int i = 0; i < 100; ++i)
    {
        CHECK_FALSE(rng.chance(0.f));
        CHECK(rng.chance(1.f));
    }
}
