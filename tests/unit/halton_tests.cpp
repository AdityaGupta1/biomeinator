#include "util/halton.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>

TEST_CASE("HaltonSequence emits the known base-2/base-3 prefix", "[unit][halton]")
{
    constexpr std::array expected{
        DirectX::XMFLOAT2{ 1.f / 2.f, 1.f / 3.f }, DirectX::XMFLOAT2{ 1.f / 4.f, 2.f / 3.f },
        DirectX::XMFLOAT2{ 3.f / 4.f, 1.f / 9.f }, DirectX::XMFLOAT2{ 1.f / 8.f, 4.f / 9.f },
        DirectX::XMFLOAT2{ 5.f / 8.f, 7.f / 9.f },
    };

    HaltonSequence sequence;
    sequence.init(static_cast<uint32_t>(expected.size()));
    for (const DirectX::XMFLOAT2 expectedValue : expected)
    {
        const DirectX::XMFLOAT2 actual = sequence.next();
        CHECK(actual.x == Catch::Approx(expectedValue.x));
        CHECK(actual.y == Catch::Approx(expectedValue.y));
    }
}

TEST_CASE("HaltonSequence wraps at its configured length", "[unit][halton]")
{
    HaltonSequence sequence;
    sequence.init(3);

    const DirectX::XMFLOAT2 first = sequence.next();
    sequence.next();
    sequence.next();
    const DirectX::XMFLOAT2 wrapped = sequence.next();

    CHECK(wrapped.x == first.x);
    CHECK(wrapped.y == first.y);
}

TEST_CASE("HaltonSequence treats zero length as disabled jitter", "[unit][halton]")
{
    HaltonSequence sequence;
    sequence.init(0);

    for (int i = 0; i < 4; ++i)
    {
        const DirectX::XMFLOAT2 value = sequence.next();
        CHECK(value.x == 0.f);
        CHECK(value.y == 0.f);
    }
}

TEST_CASE("HaltonSequence reinitialization resets the cursor", "[unit][halton]")
{
    HaltonSequence sequence;
    sequence.init(8);
    const DirectX::XMFLOAT2 first = sequence.next();
    sequence.next();

    sequence.init(8);
    const DirectX::XMFLOAT2 restarted = sequence.next();
    CHECK(restarted.x == first.x);
    CHECK(restarted.y == first.y);
}
