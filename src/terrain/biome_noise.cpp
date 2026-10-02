// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "biome_noise.h"
#include "oasis_shaping.h"
#include "terrain_formation.h"

#include "debug.h"
#include "rendering/common/common_settings.h"
#include "util/rng.h"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <vector>

#include <FastNoise/FastNoise.h>

using namespace glm;
namespace FN = FastNoise;

namespace BiomeNoiseFields
{

static FN::SmartNode<FN::Generator> fnTemperature;
static FN::SmartNode<FN::Generator> fnHumidity;
static FN::SmartNode<FN::Generator> fnPeak;
static FN::SmartNode<FN::Generator> fnInland;
static FN::SmartNode<FN::Generator> fnErosion;
inline constexpr float baseNoiseScale = 1000.f;
// Climate (temperature, humidity) and relief (peak, inland, erosion) can scale independently:
// enlarging climate regions alone doesn't move coastlines or mountains.
inline constexpr float climateNoiseScale = baseNoiseScale * 3.f;
inline constexpr float reliefNoiseScale = baseNoiseScale * 1.f;

// Shared by fillGrids and sampleAt so single-point samples match the grids
static int noiseFieldSeed;
static ivec2 noiseOffsetXZ;

void init(uint32_t worldSeed)
{
    noiseFieldSeed = static_cast<int>(worldSeed ^ hash(719023919));
    RandomNumberGenerator rng = initRng(worldSeed ^ hash(8810091029));
    noiseOffsetXZ = ivec2(rng.nextInt(-4096, 4096), rng.nextInt(-4096, 4096));

    {
        auto source = FN::New<FN::Simplex>();
        source->SetSeedOffset(186729341);
        source->SetScale(1.5f * reliefNoiseScale);
        auto fractal = FN::New<FN::FractalFBm>();
        fractal->SetSource(source);
        fractal->SetOctaveCount(3);
        fnErosion = fractal;
    }
    OasisShaping::init(worldSeed);

    {
        auto fnSimplex = FN::New<FN::Simplex>();
        fnSimplex->SetSeedOffset(5689481209);
        fnSimplex->SetScale(2.5f * climateNoiseScale);
        fnSimplex->SetOutputMin(-0.7f);
        fnSimplex->SetOutputMax(0.7f);
        auto fnWarp = FN::New<FN::DomainWarpGradient>();
        fnWarp->SetSource(fnSimplex);
        fnWarp->SetScale(0.06f * climateNoiseScale);
        fnWarp->SetWarpAmplitude(0.02f * climateNoiseScale);
        auto fnFractal = FN::New<FN::FractalFBm>();
        fnFractal->SetSource(fnWarp);
        fnFractal->SetOctaveCount(3);

        fnTemperature = fnFractal;
    }

    {
        auto fnSimplex = FN::New<FN::Simplex>();
        fnSimplex->SetSeedOffset(680199230);
        fnSimplex->SetScale(1.5f * climateNoiseScale);
        fnSimplex->SetOutputMin(-0.7f);
        fnSimplex->SetOutputMax(0.7f);
        auto fnWarp = FN::New<FN::DomainWarpGradient>();
        fnWarp->SetSource(fnSimplex);
        fnWarp->SetScale(0.04f * climateNoiseScale);
        fnWarp->SetWarpAmplitude(0.03f * climateNoiseScale);
        auto fnFractal = FN::New<FN::FractalFBm>();
        fnFractal->SetSource(fnWarp);
        fnFractal->SetOctaveCount(3);

        fnHumidity = fnFractal;
    }

    {
        auto fnSimplex = FN::New<FN::Simplex>();
        fnSimplex->SetSeedOffset(901992021);
        fnSimplex->SetScale(2.5f * reliefNoiseScale);
        fnSimplex->SetOutputMin(0.0f);
        fnSimplex->SetOutputMax(1.0f);
        auto fnFractalRidged = FN::New<FN::FractalRidged>();
        fnFractalRidged->SetSource(fnSimplex);
        fnFractalRidged->SetOctaveCount(5);
        auto fnMultiply = FN::New<FN::Multiply>();
        fnMultiply->SetLHS(fnFractalRidged);
        fnMultiply->SetRHS(0.7f);

        fnPeak = fnMultiply;
    }

    {
        auto fnSimplex = FN::New<FN::Simplex>();
        fnSimplex->SetSeedOffset(76123912);
        fnSimplex->SetScale(5.f * reliefNoiseScale);
        fnSimplex->SetOutputMin(-1.0f);
        fnSimplex->SetOutputMax(1.0f);
        auto fnWarp = FN::New<FN::DomainWarpGradient>();
        fnWarp->SetSource(fnSimplex);
        fnWarp->SetScale(0.04f * reliefNoiseScale);
        fnWarp->SetWarpAmplitude(0.02f * reliefNoiseScale);
        auto fnFractal = FN::New<FN::FractalFBm>();
        fnFractal->SetSource(fnWarp);
        fnFractal->SetOctaveCount(5);

        fnInland = fnFractal;
    }
}

glm::ivec2 getNoiseOffsetXZ()
{
    return noiseOffsetXZ;
}

// Pairs each grid field with its generator, so every batch fill covers the same fields.
template<typename Fill>
static void forEachField(const BiomeNoiseGrids& grids, const Fill& fill)
{
    fill(grids.temperature, fnTemperature);
    fill(grids.humidity, fnHumidity);
    fill(grids.peak, fnPeak);
    fill(grids.inland, fnInland);
    fill(grids.erosion, fnErosion);
}

void fillGrids(const BiomeNoiseGrids& grids, vec2 startXZ, glm::uvec2 numSamples, float stepBlocks)
{
    forEachField(grids, [&](float* data, const FN::SmartNode<FN::Generator>& fn)
    {
        fn->GenUniformGrid2D(data,
                             startXZ.x + noiseOffsetXZ.x,
                             startXZ.y + noiseOffsetXZ.y /*z*/,
                             numSamples.x,
                             numSamples.y,
                             stepBlocks,
                             stepBlocks,
                             noiseFieldSeed);
    });
}

void fillPositions(const BiomeNoiseGrids& grids, const float* xPositions, const float* zPositions, uint32_t numSamples)
{
    forEachField(grids, [&](float* data, const FN::SmartNode<FN::Generator>& fn)
    {
        fn->GenPositionArray2D(data, numSamples, xPositions, zPositions,
                              noiseOffsetXZ.x, noiseOffsetXZ.y, noiseFieldSeed);
    });
}

BiomeNoise sampleAt(vec2 posXZ_WS)
{
    const float x = posXZ_WS.x + noiseOffsetXZ.x;
    const float z = posXZ_WS.y + noiseOffsetXZ.y /*z*/;
    return {
        .temperature = fnTemperature->GenSingle2D(x, z, noiseFieldSeed),
        .humidity = fnHumidity->GenSingle2D(x, z, noiseFieldSeed),
        .peak = fnPeak->GenSingle2D(x, z, noiseFieldSeed),
        .inland = fnInland->GenSingle2D(x, z, noiseFieldSeed),
        .erosion = fnErosion->GenSingle2D(x, z, noiseFieldSeed),
    };
}

BiomeNoise noiseAt(const BiomeNoiseGrids& grids, uint32_t idx)
{
    return {
        .temperature = grids.temperature[idx],
        .humidity = grids.humidity[idx],
        .peak = grids.peak[idx],
        .inland = grids.inland[idx],
        .erosion = grids.erosion[idx],
    };
}

using TerrainFormations::widenedSmoothstep;

// Factors below take a widen argument: 1 gives the ramps labels and landforms use; larger values
// give softer versions of the same ramps for styles (roughness, detail) that must change slowly
// across the ground. See knowledge/terrain/biome_system.md.

// Relief of any kind ramps in from the shoreline, so hills can rise straight out of beaches.
static float landWeight(const BiomeNoise& n, float widen = 1.f)
{
    return widenedSmoothstep(0.f, 0.35f, n.inland, widen);
}

// Landforms that need solid ground behind the coast (terraces, karst) start farther inland and
// finish sooner than general relief, so they never sit on the beach band.
static float interiorWeight(const BiomeNoise& n, float widen = 1.f)
{
    return widenedSmoothstep(0.1f, 0.3f, n.inland, widen);
}

float ruggedWeight(const BiomeNoise& n, float widen)
{
    return 1.f - widenedSmoothstep(-0.1f, 0.5f, n.erosion, widen);
}

static float highlandReliefWeight(const BiomeNoise& n)
{
    return ruggedWeight(n) * smoothstep(0.25f, 1.05f, n.inland);
}

static float peak01(const BiomeNoise& n)
{
    return clamp((n.peak + 1.f) * 0.5f, 0.f, 1.f);
}

float mountainPeakWeight(const BiomeNoise& n)
{
    return highlandReliefWeight(n) * pow(peak01(n), 4.f);
}

inline constexpr float highlandThreshold = 0.1f;

bool isHighland(const BiomeNoise& n)
{
    return mountainPeakWeight(n) >= highlandThreshold;
}

static float terraceWeight(const BiomeNoise& n, float widen)
{
    return TerrainFormations::smoothBand(n.erosion, -0.18f, 0.02f, 0.27f, 0.48f, widen) * interiorWeight(n, widen);
}

float dryClimateWeight(const BiomeNoise& n, float widen)
{
    return widenedSmoothstep(0.12f, 0.38f, n.temperature, widen) *
           (1.f - widenedSmoothstep(-0.25f, 0.02f, n.humidity, widen));
}

static float floodSuitability(const BiomeNoise& n, float)
{
    return computeFloodFactor(n);
}

static float tianziSuitability(const BiomeNoise& n, float widen)
{
    // Karst occupies humid, temperate-to-warm rugged regions. Cold or dry mountain
    // climates retain ordinary peaks instead of being intercepted by erosion alone.
    const float temperate = TerrainFormations::smoothBand(n.temperature, -0.55f, -0.05f, 0.65f, 1.05f, widen);
    const float humid = widenedSmoothstep(-0.1f, 0.3f, n.humidity, widen);
    const float preserved = 1.f - widenedSmoothstep(-0.65f, -0.1f, n.erosion, widen);
    return temperate * humid * preserved * interiorWeight(n, widen);
}

static float mesaSuitability(const BiomeNoise& n, float widen)
{
    return terraceWeight(n, widen) * dryClimateWeight(n, widen);
}

static float redDesertSuitability(const BiomeNoise& n, float widen)
{
    return dryClimateWeight(n, widen) * landWeight(n, widen) * ruggedWeight(n, widen);
}

struct TerrainRegimeData
{
    Biome biome;
    float (*suitability)(const BiomeNoise&, float widen);
    // Label boundary.
    float threshold;
    // Stored relative to the threshold so recalibrating a threshold (e.g. from a target area
    // share) keeps the ramps valid.
    // Suitability span from the threshold to full landform weight.
    float strengthRange;
    // Lower-priority regimes fade out over this fraction of the threshold, just below it. It
    // must stay below 1: suitabilities bottom out at 0, so a wider fade would suppress them
    // everywhere, even far from this regime.
    float fadeFraction;
    // Density amplitude (roughness) where this regime's terrain lies. Unset regimes keep the
    // roughness their relief implies.
    std::optional<float> amplitude;
    // Widening of the suitability ramps for the style weight that blends that roughness in. Wider
    // gives softer edges; roughness contrasts of tens of blocks become walls when they change
    // over only a few blocks.
    float styleWiden;

    constexpr float fullStrength() const
    {
        return threshold + strengthRange;
    }
    constexpr float fadeWidth() const
    {
        return threshold * fadeFraction;
    }
};

// Swamp terrain comes from flood cells (see swamp_shaping); its row sets the label, the fade of
// lower-priority regimes, and the flood strength at which pond floors reach full depth.
static constexpr std::array<TerrainRegimeData, static_cast<size_t>(TerrainRegime::COUNT)> regimes{{
    { Biome::SWAMP, floodSuitability, floodTintThreshold, floodFullStrength - floodTintThreshold, 0.4f, std::nullopt, 1.f },
    { Biome::TIANZI_MOUNTAINS, tianziSuitability, 0.35f, 0.5f, 0.43f, 7.f, 2.0f },
    { Biome::MESA, mesaSuitability, 0.15f, 0.35f, 0.67f, 10.f, 4.5f },
    { Biome::RED_DESERT, redDesertSuitability, 0.1f, 0.5f, 0.5f, 12.f, 7.5f },
}};
static_assert(std::ranges::all_of(regimes, [](const TerrainRegimeData& regime)
{
    return regime.threshold > 0.f && regime.strengthRange > 0.f && regime.fadeFraction > 0.f &&
           regime.fadeFraction < 1.f && regime.styleWiden >= 1.f;
}));

// See NaturalTerrain::regimeWeights, regimeCoverage and regimeStyle.
struct RegimeEvaluation
{
    RegimeWeights landform;
    RegimeWeights coverage;
    RegimeWeights style;
};

static RegimeEvaluation evaluateRegimes(const BiomeNoise& n)
{
    // Ramping the complete suitability (rather than separately fading each axis) keeps
    // coastal and climate boundaries from cutting through full-strength landforms.
    RegimeEvaluation result;
    float unclaimed = 1.f;
    for (size_t regimeIdx = 0; regimeIdx < regimes.size(); ++regimeIdx)
    {
        const TerrainRegimeData& regime = regimes[regimeIdx];
        const float suitability = regime.suitability(n, 1.f);
        const float claim = smoothstep(regime.threshold - regime.fadeWidth(), regime.threshold, suitability);
        result.landform.weights[regimeIdx] = unclaimed * smoothstep(regime.threshold, regime.fullStrength(), suitability);
        result.coverage.weights[regimeIdx] = unclaimed * claim;
        unclaimed *= 1.f - claim;
        // Styles read the noise through softer ramps, never the label: full by the label edge,
        // fading out well beyond it. Regimes with styles sit in disjoint climates or share
        // similar styles, so no priority suppression is needed.
        if (regime.amplitude)
        {
            result.style.weights[regimeIdx] = smoothstep(regime.threshold - regime.fadeWidth(), regime.threshold,
                                                         regime.suitability(n, regime.styleWiden));
        }
    }
    return result;
}


static const TerrainRegimeData* findClaimingRegime(const BiomeNoise& n)
{
    for (const TerrainRegimeData& regime : regimes)
    {
        if (regime.suitability(n, 1.f) > regime.threshold)
        {
            return &regime;
        }
    }
    return nullptr;
}


static float terraceHeight(float height, vec2 pos)
{
    // Irregular elevation intervals avoid repeating identical shelves up the hillside.
    // Shared anchors keep the remap continuous when either the interval or biome changes.
    // A slow world-position offset shifts the shelf elevations regionally; climate must not,
    // since rescaling or equalizing the climate fields would then move every shelf.
    constexpr float spacing = 42.f;
    const float offset = 12.f * TerrainFormations::valueNoise(pos / 700.f, noiseFieldSeed ^ 0x7E2F0u);
    const float localHeight = height - offset;
    const auto anchor = [](int index)
    {
        RandomNumberGenerator rng = initRng(noiseFieldSeed ^ 0x7E22ACEu, index);
        return SEA_LEVEL + spacing * (index + rng.nextFloat(-0.32f, 0.32f));
    };
    const TerrainFormations::Band band = TerrainFormations::jitteredBand(
        localHeight, static_cast<int>(floor((localHeight - SEA_LEVEL) / spacing)), anchor);
    const float t = clamp((localHeight - band.low) / (band.high - band.low), 0.f, 1.f);
    RandomNumberGenerator rng = initRng(noiseFieldSeed ^ 0x51E1Fu, band.index);
    const float rampStart = rng.nextFloat(0.15f, 0.4f);
    const float rampEnd = rng.nextFloat(0.75f, 0.95f);
    // Retain a slope across the shelf instead of flattening each tread completely.
    return offset + mix(band.low, band.high, mix(t, smoothstep(rampStart, rampEnd, t), 0.8f));
}

NaturalTerrain computeNaturalTerrain(const BiomeNoise& n, vec2 posXZ_WS)
{
    const float peak = peak01(n);
    const float land = landWeight(n);
    const float rugged = ruggedWeight(n);
    const RegimeEvaluation regimeEvaluation = evaluateRegimes(n);
    const RegimeWeights& weights = regimeEvaluation.landform;
    const float terraces = weights[TerrainRegime::MESA];
    const float tianzi = weights[TerrainRegime::TIANZI];
    const vec2 pos = posXZ_WS + vec2(noiseOffsetXZ);

    // Elevation comes only from peak, erosion and inland; climate selects landform styles
    // below but never raises or lowers the ground. Gating relief by climate would shift
    // elevation by hundreds of blocks where humidity crosses the dry threshold.
    const float inlandHeight = 1.f / (1.f + expf(-10.f * n.inland + 0.1f)) + 0.03f * n.inland - 0.7f;
    const float foundation = 140.f + inlandHeight * 90.f;
    // The modest shared ground still connects all profiles. Strong peaks rise only in
    // preserved highlands, without roughening flat lowlands.
    const float mountainRelief = mix(8.f, 75.f, rugged) * pow(peak, 2.5f);
    const float highland = highlandReliefWeight(n);
    const float peakRelief = 160.f * mountainPeakWeight(n);
    // Complementary weights blend complete profiles: as Tianzi weight removes the extra
    // peak relief, the same weight supplies the stacked formations below.
    float height = foundation + land * (mountainRelief + (1.f - tianzi) * peakRelief);
    if (terraces > 0.f)
    {
        // Mesa reshapes the shared elevation within bounded offsets instead of replacing it:
        // buttes and gullies of about +/-21 blocks, then shelves that move a column by at
        // most one terrace band. A partial weight at the regime edge therefore cannot open a
        // pit into neighboring relief; high ground becomes a tall terraced massif.
        const float plateau = TerrainFormations::plateauRelief(pos, noiseFieldSeed ^ 0xBA01u);
        height += terraces * land * 42.f * (plateau - 0.5f);
        height = mix(height, terraceHeight(height, pos), 0.45f * terraces);
    }
    const float coastPull = smoothstep(0.2f, 0.f, abs(n.inland)) * 0.9f;
    height = mix(height, static_cast<float>(SEA_LEVEL + 8), coastPull);
    const float formationBase = height;

    float uplift = 0.f;
    ivec2 formationSite{};
    if (tianzi > 0.f)
    {
        // Three independently sited tiers. Broad summits and narrow rises leave plantable
        // shelves between crowns.
        constexpr std::array<TerrainFormations::Profile, 3> tiers{{
            { 92.f, 34.f, 52.f, 42.f, 8.f, 0.84f, 0.85f },
            { 54.f, 24.f, 30.f, 38.f, 4.f, 0.80f, 0.85f },
            { 37.f, 16.2f, 19.f, 32.f, 2.f, 0.78f, 0.85f },
        }};
        uplift = tianzi * TerrainFormations::sampleStacked(pos, noiseFieldSeed ^ 0x75423u, tiers, &formationSite);
    }
    // Quartz spires are the red desert's formation. They reuse the same finite-support
    // sampler with a narrow summit and a broad foot, not a new noise field.
    const float spireWeight = weights[TerrainRegime::RED_DESERT];
    if (spireWeight > 0.f)
    {
        constexpr TerrainFormations::Profile spires{ 116.f, 6.5f, 34.f, 42.f, 24.f, 0.04f, 1.f };
        uplift += spireWeight * TerrainFormations::sample(pos, noiseFieldSeed ^ 0x91337u, spires);
    }
    height += uplift;

    // Roughness follows relief, blended toward each regime's roughness by its noise-driven style
    // weight. It must change slowly across the ground: density amplitude sets how far 3D noise
    // pushes the surface, so a contrast over a few blocks stands up as a wall.
    float amplitude = mix(12.f, 38.f, rugged);
    amplitude += 40.f * highland * smoothstep(0.1f, 0.65f, n.peak);
    for (size_t regimeIdx = 0; regimeIdx < regimes.size(); ++regimeIdx)
    {
        if (regimes[regimeIdx].amplitude)
        {
            amplitude = mix(amplitude, *regimes[regimeIdx].amplitude, regimeEvaluation.style.weights[regimeIdx]);
        }
    }
    amplitude = mix(amplitude, 4.f, smoothstep(5.f, 30.f, uplift));
    amplitude /= 1.f + 3.f * smoothstep(0.4f, -0.1f, abs(n.inland));
    return { height, 1.f / amplitude, formationBase, uplift, formationSite, weights, regimeEvaluation.coverage,
             regimeEvaluation.style };
}

float computeFloodFactor(const BiomeNoise& biomeNoise)
{
    const float temperatureFactor = smoothstep(-0.1f, 0.35f, biomeNoise.temperature);
    const float humidityFactor = smoothstep(0.0f, 0.45f, biomeNoise.humidity);
    // Wetlands need the same eroded, low-relief ground that terrain flattens.
    const float flatFactor = min(smoothstep(-0.1f, -0.55f, biomeNoise.peak),
                                smoothstep(0.37f, 0.f, ruggedWeight(biomeNoise)));
    const float inlandFactor = smoothstep(0.2f, 0.3f, biomeNoise.inland);

    // min, not product: the factor is limited by its worst axis, instead of requiring every axis
    // to be near-perfect at once.
    return min(min(temperatureFactor, humidityFactor), min(flatFactor, inlandFactor));
}

bool isClaimedByRegime(const BiomeNoise& n)
{
    return findClaimingRegime(n) != nullptr;
}

// Cells are a jittered grid of sites with random weights (a power diagram), so neighboring cells
// differ in size. Lookups are warped by noise at two scales, so borders curve and fray instead of
// following straight polygon edges.
inline constexpr float climateCellSize = 384.f;
inline constexpr float climateCellMaxWeightRadius = 0.45f * climateCellSize;
// Heavy sites can claim ground past their immediate neighbors. In cell units, the home cell's site
// has power distance at most 2 * 0.85^2 + 0.45^2; sites 3 cells away, or at both (+-2, +-2)
// corners, are always farther than that, so they are never searched.
inline constexpr int climateCellSearchRadius = 2;

struct WarpOctave
{
    float scale;
    float amplitude;
};
inline constexpr std::array<WarpOctave, 2> climateCellWarpOctaves{{ { 120.f, 55.f }, { 18.f, 7.f } }};
inline constexpr float climateCellMaxWarp = []
{
    float maxWarp = 0.f;
    for (const WarpOctave& octave : climateCellWarpOctaves)
    {
        maxWarp += octave.amplitude;
    }
    return maxWarp;
}();
// Each cell's sampled climate is shifted by up to this much on each axis
inline constexpr float climateCellClimateOffset = 0.1f;

static uint32_t climateCellSeed()
{
    return static_cast<uint32_t>(noiseFieldSeed) ^ hash(402913377);
}

// TerrainFormations::valueNoise with the corners hashed once per region instead of per sample
ClimateCellContext::Lattice::Lattice(vec2 minPos, vec2 maxPos, uint32_t seed)
    : minCorner(ivec2(glm::floor(minPos)))
{
    const ivec2 maxCorner = ivec2(glm::floor(maxPos)) + 1;
    width = maxCorner.x - minCorner.x + 1;
    height = maxCorner.y - minCorner.y + 1;
    values.resize(static_cast<size_t>(width) * height);
    for (int z = 0; z < height; ++z)
    {
        for (int x = 0; x < width; ++x)
        {
            values[x + z * width] = TerrainFormations::valueNoiseCorner(minCorner + ivec2(x, z), seed);
        }
    }
}

float ClimateCellContext::Lattice::sample(vec2 pos) const
{
    const ivec2 cell = ivec2(glm::floor(pos));
    const ivec2 local = cell - minCorner;
    ASSERT(local.x >= 0 && local.y >= 0 && local.x + 1 < width && local.y + 1 < height);
    const int idx = local.x + local.y * width;
    return TerrainFormations::valueNoiseBlend(
        pos - vec2(cell), values[idx], values[idx + 1], values[idx + width], values[idx + width + 1]);
}

ClimateCellContext::ClimateCellContext(vec2 minXZ_WS, vec2 maxXZ_WS)
    : warpLattices(
          [&]
          {
              const uint32_t seed = climateCellSeed();
              const auto make = [&](uint32_t octaveIdx, uint32_t axis)
              {
                  const float scale = climateCellWarpOctaves[octaveIdx].scale;
                  return Lattice(minXZ_WS / scale, maxXZ_WS / scale, seed ^ hash(octaveIdx * 2 + axis + 1));
              };
              return std::array<Lattice, 4>{ make(0, 0), make(0, 1), make(1, 0), make(1, 1) };
          }())
{
    const uint32_t seed = climateCellSeed();
    minCell = ivec2(glm::floor((minXZ_WS - climateCellMaxWarp) / climateCellSize)) - climateCellSearchRadius;
    const ivec2 maxCell = ivec2(glm::floor((maxXZ_WS + climateCellMaxWarp) / climateCellSize)) + climateCellSearchRadius;
    numCellsX = maxCell.x - minCell.x + 1;
    numCellsZ = maxCell.y - minCell.y + 1;
    sites.reserve(static_cast<size_t>(numCellsX) * numCellsZ);
    for (int z = minCell.y; z <= maxCell.y; ++z)
    {
        for (int x = minCell.x; x <= maxCell.x; ++x)
        {
            RandomNumberGenerator rng = initRng(seed, static_cast<uint32_t>(x), static_cast<uint32_t>(z));
            const float siteX = rng.nextFloat(0.15f, 0.85f);
            const float siteZ = rng.nextFloat(0.15f, 0.85f);
            const float weight = rng.nextFloat(-1.f, 1.f) * climateCellMaxWeightRadius * climateCellMaxWeightRadius;
            const float temperatureOffset = rng.nextFloatAbs(climateCellClimateOffset);
            const float humidityOffset = rng.nextFloatAbs(climateCellClimateOffset);
            sites.push_back({ (vec2(x, z) + vec2(siteX, siteZ)) * climateCellSize, weight,
                              ClimateTarget{ .temperature = temperatureOffset, .humidity = humidityOffset }, std::nullopt });
        }
    }
}

const ClimateCellContext::Site& ClimateCellContext::findSite(vec2 lookupPosXZ_WS, ivec2* outCellId) const
{
    vec2 warpedPos = lookupPosXZ_WS;
    for (uint32_t octaveIdx = 0; octaveIdx < climateCellWarpOctaves.size(); ++octaveIdx)
    {
        const WarpOctave& octave = climateCellWarpOctaves[octaveIdx];
        const vec2 latticePos = lookupPosXZ_WS / octave.scale;
        warpedPos += octave.amplitude *
                     vec2(warpLattices[octaveIdx * 2].sample(latticePos), warpLattices[octaveIdx * 2 + 1].sample(latticePos));
    }

    const ivec2 homeCell = ivec2(glm::floor(warpedPos / climateCellSize));
    const Site* best = nullptr;
    ivec2 bestCell{};
    float bestPowerDistance = std::numeric_limits<float>::max();
    for (int dz = -climateCellSearchRadius; dz <= climateCellSearchRadius; ++dz)
    {
        for (int dx = -climateCellSearchRadius; dx <= climateCellSearchRadius; ++dx)
        {
            if (glm::abs(dx) == climateCellSearchRadius && glm::abs(dz) == climateCellSearchRadius)
            {
                continue;
            }
            const ivec2 cell = homeCell + ivec2(dx, dz);
            const ivec2 local = cell - minCell;
            ASSERT(local.x >= 0 && local.y >= 0 && local.x < numCellsX && local.y < numCellsZ);
            const Site& site = sites[local.x + local.y * numCellsX];
            const vec2 offset = warpedPos - site.posXZ_WS;
            const float powerDistance = dot(offset, offset) - site.weight;
            if (powerDistance < bestPowerDistance)
            {
                bestPowerDistance = powerDistance;
                best = &site;
                bestCell = cell;
            }
        }
    }
    if (outCellId)
    {
        *outCellId = bestCell;
    }
    return *best;
}

ClimateTarget ClimateCellContext::climateAt(vec2 lookupPosXZ_WS) const
{
    const Site& site = findSite(lookupPosXZ_WS);
    if (!site.climate)
    {
        const float x = site.posXZ_WS.x + noiseOffsetXZ.x;
        const float z = site.posXZ_WS.y + noiseOffsetXZ.y /*z*/;
        site.climate = ClimateTarget{
            .temperature = fnTemperature->GenSingle2D(x, z, noiseFieldSeed) + site.climateOffset.temperature,
            .humidity = fnHumidity->GenSingle2D(x, z, noiseFieldSeed) + site.climateOffset.humidity,
        };
    }
    return *site.climate;
}

uint32_t ClimateCellContext::cellHashAt(vec2 lookupPosXZ_WS) const
{
    ivec2 cellId;
    findSite(lookupPosXZ_WS, &cellId);
    return hash(static_cast<uint32_t>(cellId.x) ^ hash(static_cast<uint32_t>(cellId.y)));
}

Biome biomeFromNoise(const BiomeNoise& biomeNoise, const ClimateCellContext* cellContext, vec2 cellLookupPosXZ_WS)
{
    const TerrainRegimeData* regime = findClaimingRegime(biomeNoise);
    if (regime)
    {
        return regime->biome;
    }
    const ClimateTarget climate = cellContext
        ? cellContext->climateAt(cellLookupPosXZ_WS)
        : ClimateTarget{ .temperature = biomeNoise.temperature, .humidity = biomeNoise.humidity };
    return Biomes::getClosestBiome(biomeNoise, climate);
}

// Rect fills go tile by tile, so a context's lattices and sites stay small however large the rect
inline constexpr uint32_t rectTileTexels = 128;

template<typename FillTile>
static void forEachRectTile(glm::ivec2 originBlocksXZ_WS, glm::uvec2 numTexels, uint32_t texelSizeBlocks,
                            const FillTile& fillTile)
{
    for (uint32_t tileZ = 0; tileZ < numTexels.y; tileZ += rectTileTexels)
    {
        for (uint32_t tileX = 0; tileX < numTexels.x; tileX += rectTileTexels)
        {
            const uvec2 tileStart(tileX, tileZ);
            const uvec2 tileEnd = glm::min(tileStart + rectTileTexels, numTexels);
            const vec2 firstCenter = vec2(originBlocksXZ_WS) + (vec2(tileStart) + 0.5f) * static_cast<float>(texelSizeBlocks);
            const vec2 lastCenter = vec2(originBlocksXZ_WS) + (vec2(tileEnd) - 0.5f) * static_cast<float>(texelSizeBlocks);
            fillTile(tileStart, tileEnd, ClimateCellContext(firstCenter, lastCenter));
        }
    }
}

void fillBiomeRect(Biome* outBiomes, glm::ivec2 originBlocksXZ_WS, glm::uvec2 numTexels, uint32_t texelSizeBlocks,
                   bool climateCells)
{
    const uint32_t numSamples = numTexels.x * numTexels.y;
    std::vector<float> noise(BiomeNoiseGrids::numFields * numSamples);
    const BiomeNoiseGrids grids = BiomeNoiseGrids::fromBuffer(noise.data(), numSamples);

    const vec2 texelCentersStartXZ = vec2(originBlocksXZ_WS) + texelSizeBlocks * 0.5f;
    fillGrids(grids, texelCentersStartXZ, numTexels, static_cast<float>(texelSizeBlocks));
    const auto oases = OasisShaping::makeContext(originBlocksXZ_WS, glm::ivec2(numTexels) * static_cast<int>(texelSizeBlocks));

    forEachRectTile(originBlocksXZ_WS, numTexels, texelSizeBlocks,
                    [&](uvec2 tileStart, uvec2 tileEnd, const ClimateCellContext& cellContext)
    {
        for (uint32_t z = tileStart.y; z < tileEnd.y; ++z)
        {
            for (uint32_t x = tileStart.x; x < tileEnd.x; ++x)
            {
                const uint32_t idx = x + z * numTexels.x;
                const vec2 pos = texelCentersStartXZ + vec2(x, z) * static_cast<float>(texelSizeBlocks);
                outBiomes[idx] = OasisShaping::sample(pos, oases).vegetation
                    ? Biome::OASIS
                    : biomeFromNoise(noiseAt(grids, idx), climateCells ? &cellContext : nullptr, pos);
            }
        }
    });
}

void fillClimateCellHashRect(uint32_t* outHashes, glm::ivec2 originBlocksXZ_WS, glm::uvec2 numTexels,
                             uint32_t texelSizeBlocks)
{
    const vec2 texelCentersStartXZ = vec2(originBlocksXZ_WS) + texelSizeBlocks * 0.5f;
    forEachRectTile(originBlocksXZ_WS, numTexels, texelSizeBlocks,
                    [&](uvec2 tileStart, uvec2 tileEnd, const ClimateCellContext& cellContext)
    {
        for (uint32_t z = tileStart.y; z < tileEnd.y; ++z)
        {
            for (uint32_t x = tileStart.x; x < tileEnd.x; ++x)
            {
                const vec2 pos = texelCentersStartXZ + vec2(x, z) * static_cast<float>(texelSizeBlocks);
                outHashes[x + z * numTexels.x] = cellContext.cellHashAt(pos);
            }
        }
    });
}

} // namespace BiomeNoiseFields
