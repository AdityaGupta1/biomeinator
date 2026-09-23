#include "rendering/camera_math.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <numbers>
#include <random>

namespace
{

void checkVec3(const glm::vec3& actual, const glm::vec3& expected, float margin = 1e-5f)
{
    CHECK(actual.x == Catch::Approx(expected.x).margin(margin));
    CHECK(actual.y == Catch::Approx(expected.y).margin(margin));
    CHECK(actual.z == Catch::Approx(expected.z).margin(margin));
}

} // namespace

TEST_CASE("split camera positions use floor semantics for negative coordinates", "[unit][camera_math]")
{
    const CameraMath::SplitPosition position = CameraMath::split({ 1.75f, -0.25f, -2.f });

    CHECK(position.integer == glm::ivec3(1, -1, -2));
    checkVec3(position.fractional, { 0.75f, 0.75f, 0.f });
    checkVec3(CameraMath::combine(position), { 1.75f, -0.25f, -2.f });

    CameraMath::SplitPosition exactBoundary{
        .integer = { 10, -10, 0 },
        .fractional = { 1.f, -1.f, 2.f },
    };
    CameraMath::normalize(exactBoundary);
    CHECK(exactBoundary.integer == glm::ivec3(11, -11, 2));
    checkVec3(exactBoundary.fractional, { 0.f, 0.f, 0.f });
}

TEST_CASE("split camera positions preserve local precision at large coordinates", "[unit][camera_math]")
{
    CameraMath::SplitPosition position{
        .integer = { 1'500'000'000, -1'500'000'000, 42 },
        .fractional = { 0.25f, 0.75f, 0.f },
    };

    CameraMath::add(position, { -0.5f, 0.5f, 1.f });

    CHECK(position.integer == glm::ivec3(1'499'999'999, -1'499'999'999, 43));
    checkVec3(position.fractional, { 0.75f, 0.25f, 0.f });
    checkVec3(CameraMath::relativeTo(position, { 1'499'999'990, -1'500'000'010, 40 }), { 9.75f, 11.25f, 3.f });
}

TEST_CASE("split camera positions stay normalized under deterministic movement churn", "[unit][camera_math][stress]")
{
    constexpr uint32_t seed = 0xCA6E1234u;
    CameraMath::SplitPosition position = CameraMath::split({ 0.25f, 0.5f, 0.75f });
    glm::dvec3 expected(0.25, 0.5, 0.75);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> displacement(-25.f, 25.f);
    INFO("seed=" << seed);

    for (int operation = 0; operation < 20000; ++operation)
    {
        const glm::vec3 delta(displacement(rng), displacement(rng), displacement(rng));
        CameraMath::add(position, delta);
        expected += glm::dvec3(delta);

        CAPTURE(operation);
        CHECK(position.fractional.x >= 0.f);
        CHECK(position.fractional.y >= 0.f);
        CHECK(position.fractional.z >= 0.f);
        CHECK(position.fractional.x < 1.f);
        CHECK(position.fractional.y < 1.f);
        CHECK(position.fractional.z < 1.f);
    }

    const glm::dvec3 reconstructed = glm::dvec3(position.integer) + glm::dvec3(position.fractional);
    CHECK(reconstructed.x == Catch::Approx(expected.x).margin(0.01));
    CHECK(reconstructed.y == Catch::Approx(expected.y).margin(0.01));
    CHECK(reconstructed.z == Catch::Approx(expected.z).margin(0.01));
}

TEST_CASE("camera basis vectors are orthonormal across randomized angles", "[unit][camera_math][stress]")
{
    constexpr uint32_t seed = 0xBA515123u;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> phiDistribution(-1.5f, 1.5f);
    std::uniform_real_distribution<float> thetaDistribution(-10.f, 10.f);
    INFO("seed=" << seed);

    const CameraMath::Basis defaultBasis = CameraMath::makeBasis(0.f, std::numbers::pi_v<float>);
    checkVec3(defaultBasis.forward, { 0.f, 0.f, -1.f });
    checkVec3(defaultBasis.right, { 1.f, 0.f, 0.f });
    checkVec3(defaultBasis.up, { 0.f, 1.f, 0.f });

    for (int sample = 0; sample < 5000; ++sample)
    {
        const CameraMath::Basis basis = CameraMath::makeBasis(phiDistribution(rng), thetaDistribution(rng));
        CAPTURE(sample);
        CHECK(glm::length(basis.forward) == Catch::Approx(1.f).margin(1e-5f));
        CHECK(glm::length(basis.right) == Catch::Approx(1.f).margin(1e-5f));
        CHECK(glm::length(basis.up) == Catch::Approx(1.f).margin(1e-5f));
        CHECK(glm::dot(basis.forward, basis.right) == Catch::Approx(0.f).margin(1e-5f));
        CHECK(glm::dot(basis.forward, basis.up) == Catch::Approx(0.f).margin(1e-5f));
        CHECK(glm::dot(basis.right, basis.up) == Catch::Approx(0.f).margin(1e-5f));
        checkVec3(glm::cross(basis.right, basis.forward), basis.up, 1e-5f);
    }
}

TEST_CASE("camera frustum side normals are normalized inward and symmetric", "[unit][camera_math]")
{
    const CameraMath::Basis basis = CameraMath::makeBasis(0.f, std::numbers::pi_v<float>);
    const auto normals = CameraMath::frustumSideNormals(basis, std::numbers::pi_v<float> / 2.f, 1.f);
    const float invSqrt2 = 1.f / std::sqrt(2.f);

    checkVec3(normals[0], { invSqrt2, 0.f, -invSqrt2 });
    checkVec3(normals[1], { -invSqrt2, 0.f, -invSqrt2 });
    checkVec3(normals[2], { 0.f, invSqrt2, -invSqrt2 });
    checkVec3(normals[3], { 0.f, -invSqrt2, -invSqrt2 });

    for (const glm::vec3 normal : normals)
    {
        CHECK(glm::length(normal) == Catch::Approx(1.f).margin(1e-5f));
        CHECK(glm::dot(normal, basis.forward) > 0.f);
    }
    checkVec3(glm::normalize(normals[0] + normals[1]), basis.forward);
    checkVec3(glm::normalize(normals[2] + normals[3]), basis.forward);
}
