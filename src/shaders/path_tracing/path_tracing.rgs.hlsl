// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "../rendering/common/common_hitgroups.h"
#include "../rendering/common/common_structs.h"
#include "../rendering/common/common_registers.h"

#include "common/nvapi_includes.hlsli"

#include "common/global_params.hlsli"
#include "common/path_tracing_common.hlsli"
#include "common/payload.hlsli"
#include "light/dome_light.hlsli"
#include "light/fog.hlsli"
#include "light/light_sampling.hlsli"
#include "common/light_tree_sampling.hlsli"
#include "materials/materials.hlsli"
#include "restir/path_reservoir.hlsli"
#include "restir/reconnection.hlsli"
#include "util/color.hlsli"
#include "util/math.hlsli"

StructuredBuffer<GbufferData> gbufferIn : REGISTER_T(PT, GBUFFER_IN);

// Thin diffuse transmission fraction applied to TRIANGLE_FLAG_DIFFUSE_TRANSMISSION hits
static const float foliageDiffuseTransmission = 0.4f;

RWStructuredBuffer<float4> pathTracingRawBufferOut : REGISTER_U(PT, PATH_TRACING_RAW_BUFFER_OUT);
RWStructuredBuffer<float4> ptDiffuseAlbedoRawBufferOut : REGISTER_U(PT, PT_DIFFUSE_ALBEDO_RAW_BUFFER_OUT);
RWStructuredBuffer<PathReservoir> reservoirsOut : REGISTER_U(PT, RESERVOIRS_OUT);

// Every random draw comes from a stream keyed by the path seed, a vertex (or segment) index and its
// purpose. Random replay can then reproduce one vertex's BSDF or light draw on its own, and draws
// that are not part of the path parameterization (fog march, anyhit alpha, roulette) never shift
// the path's own sequence.
#define PATH_RNG_BSDF 0
#define PATH_RNG_NEE_AREA 1
#define PATH_RNG_NEE_DOME 2
#define PATH_RNG_FOG 3
#define PATH_RNG_RAY 4
#define PATH_RNG_ROULETTE 5
#define PATH_RNG_SHADOW_AREA 6
#define PATH_RNG_SHADOW_DOME 7

RandomNumberGenerator pathRng(const uint pathSeed, const uint idx, const uint purpose)
{
    return initRng(pathSeed, idx, purpose);
}

// Detects hitting a water backface without having crossed a water front face or started
// underwater — happens when partially loaded chunks leave water volumes open. Paths are
// terminated at such hits: continuing would trace the open water interior flagged as air
// (fog in-scatter below sea level, unattenuated dome light) which glows and flickers.
bool isOrphanWaterBackfaceHit(const Payload payload)
{
    if (!bool(payload.flags & PAYLOAD_FLAG_DID_HIT) || !bool(payload.flags & PAYLOAD_FLAG_BACKFACE_HIT) ||
        bool(payload.flags & PAYLOAD_FLAG_UNDERWATER))
    {
        return false;
    }

    if (payload.waterEntryT != RAY_DEFAULT_TMAX)
    {
        return false;
    }

    const InstanceData instanceData = instanceDatas[payload.hitInfo.instanceId];
    const PerTriangleData perTriData = perTriDatas[instanceData.perTriDatasBufferOffset + payload.hitInfo.triangleIdx];
    return bool(perTriData.flags & TRIANGLE_FLAG_IS_WATER);
}

bool isFogActive(const bool isUnderwater)
{
    return sceneParams.voxelMode == 1 && renderParams.fogSigmaS > 0.f && !isUnderwater;
}

// Adds the segment's fog in-scatter to pathColor and folds fog transmittance into throughput.
// Returns the segment's fog transmittance (1 if fog is inactive for this segment).
float applySegmentFog(const Payload payload, inout RandomNumberGenerator rng, const float3 origin_WS, const float3 dir,
    const uint numInScatterSteps, inout float3 throughput, inout float3 pathColor)
{
    if (!isFogActive(bool(payload.flags & PAYLOAD_FLAG_UNDERWATER)))
    {
        return 1.f;
    }

    const float segmentDist = getSegmentVolumeDistance(payload, origin_WS, dir);

    float fogTransmittance;
    const float3 inScatter = computeFogInScatter(origin_WS, dir, segmentDist, numInScatterSteps, rng, fogTransmittance);
    pathColor += throughput * inScatter;
    throughput *= fogTransmittance;
    return fogTransmittance;
}

Payload initPayloadFromGbuffer(const GbufferData gbufferData)
{
    Payload payload;
    payload.hitInfo = gbufferData.hitInfo;
    payload.materialIdx = gbufferData.materialIdx;
    payload.flags = gbufferData.payloadFlags;
    payload.pathWeight = float3(1.f, 1.f, 1.f);
    payload.rng = initRng(0);
    payload.waterEntryT = RAY_DEFAULT_TMAX;
    payload.waterExitT = RAY_DEFAULT_TMAX;
    payload.rayCone.angle = getRayConePixelAngle();
    payload.rayCone.width = bool(payload.flags & PAYLOAD_FLAG_DID_HIT)
        ? payload.rayCone.angle * distance(cameraParams.pos_WS, payload.hitInfo.hitPos_WS)
        : 0.f;
    return payload;
}

// The path selected by a reservoir, to be rebuilt from its seed instead of sampling a new path tree
struct ReplayTarget
{
    bool active;
    PathReservoir path;
    uint pathLength;
    uint rcVertexIdx;
    uint pathTechnique;
};

ReplayTarget makeReplayTarget(const PathReservoir path)
{
    ReplayTarget replay;
    replay.active = true;
    replay.path = path;
    replay.pathLength = getPathLength(path.flags);
    replay.rcVertexIdx = getRcVertexIdx(path.flags);
    replay.pathTechnique = getPathTechnique(path.flags);
    return replay;
}

ReplayTarget noReplay()
{
    ReplayTarget replay;
    replay.active = false;
    replay.path = makeEmptyPathReservoir();
    replay.pathLength = 0;
    replay.rcVertexIdx = 0;
    replay.pathTechnique = 0;
    return replay;
}

// The reconnection vertex chosen so far while tracing a path tree
struct RcState
{
    uint vertexIdx; // 0 = none yet
    HitInfo hit;
    bool hitIsBackface;
    float3 wi; // direction sampled at the rc vertex
};

// Routes a complete path's contribution to the reservoir when ReSTIR PT is on, otherwise
// straight into pathColor. Terms that are not resampled paths (primary emission, primary miss,
// fog in-scatter) always go straight into pathColor.
void addPathCandidate(inout PathTreeReservoir reservoir, const PathCandidate candidate, const bool useRestirPt, inout float3 pathColor)
{
    if (useRestirPt)
    {
        reservoir.addCandidate(candidate);
    }
    else
    {
        pathColor += candidate.F;
    }
}

PathCandidate makePathCandidate(const float3 F, const float rrProduct, const uint pathLength, const uint pathTechnique)
{
    PathCandidate candidate;
    candidate.F = F;
    candidate.rrProduct = rrProduct;
    candidate.pathLength = pathLength;
    candidate.pathTechnique = pathTechnique;
    candidate.rcVertexIdx = 0;
    candidate.rcHitIsBackface = false;
    candidate.rcHit.hitPos_WS = 0.f;
    candidate.rcHit.instanceId = 0;
    candidate.rcHit.hitNor_WS = 0.f;
    candidate.rcHit.triangleIdx = 0;
    candidate.rcHit.uv = 0.f;
    candidate.rcHit.pad0 = 0;
    candidate.rcHit.pad1 = 0;
    candidate.rcWi = 0.f;
    candidate.rcRadiance = 0.f;
    candidate.rcLightPdf = 0.f;
    return candidate;
}

// Reconnection data for a candidate whose rc vertex x_j was found before its light vertex x_k. When
// j == k - 1 the stored direction is the final segment's and the stored radiance excludes the path
// MIS weight, which replay recomputes from its own bsdf pdf against rcLightPdf.
void setCandidateRcFromState(inout PathCandidate candidate, const RcState rc, const float3 finalSegmentDir,
    const float3 radianceIfRcPrecedesLight, const float3 radianceOtherwise, const float lightPdf)
{
    const bool rcPrecedesLight = (rc.vertexIdx + 1 == candidate.pathLength);
    candidate.rcVertexIdx = rc.vertexIdx;
    candidate.rcHit = rc.hit;
    candidate.rcHitIsBackface = rc.hitIsBackface;
    candidate.rcWi = rcPrecedesLight ? finalSegmentDir : rc.wi;
    candidate.rcRadiance = rcPrecedesLight ? radianceIfRcPrecedesLight : radianceOtherwise;
    candidate.rcLightPdf = lightPdf;
}

// Reconnection data for a candidate that reconnects straight to its light vertex. For the dome, the
// hit is unused and the direction identifies the vertex.
void setCandidateRcAtLightVertex(inout PathCandidate candidate, const HitInfo lightHit, const bool lightHitIsBackface,
    const float3 domeDir, const float3 emission)
{
    candidate.rcVertexIdx = candidate.pathLength;
    candidate.rcHit = lightHit;
    candidate.rcHitIsBackface = lightHitIsBackface;
    candidate.rcWi = domeDir;
    candidate.rcRadiance = emission;
}

// Occlusion ray from a path vertex to the reconnection vertex (or the dome), returning the segment's
// transmittance: passthrough tint, water absorption and fog. A light point sampled by NEE carries its
// triangle's geometric normal, so TMax can stop short of the light's plane as traceToLight does. A
// BSDF-sampled hit only has its shading normal, so the ray is instead aimed from the offset origin
// exactly at the target and stopped just short of it, which reproduces the original closest-hit ray.
bool traceReconnectionRay(const float3 surfPos_WS,
                          const float3 surfNor_WS,
                          const float3 wi_WS,
                          const bool toDome,
                          const bool targetNorIsGeometric,
                          const float3 targetPos_WS,
                          const float3 targetNor_WS,
                          const RayCone rayCone,
                          const bool canPassthrough,
                          const bool startUnderwater,
                          const bool applyFog,
                          const RandomNumberGenerator rng,
                          out float3 transmittance)
{
    transmittance = 0.f;

    RayDesc ray;
    setRayOriginAndDirection(ray, surfPos_WS, surfNor_WS, wi_WS, true /*faceforwardNormal*/);
    ray.TMin = 0.f;

    float segmentDist;
    if (toDome)
    {
        ray.TMax = RAY_DEFAULT_TMAX;
        segmentDist = getDistanceToVoxelBounds(ray.Origin, ray.Direction);
    }
    else if (targetNorIsGeometric)
    {
        const float tTargetPlane = dot(targetNor_WS, targetPos_WS - ray.Origin) / dot(targetNor_WS, wi_WS);
        ray.TMax = tTargetPlane - rayOriginOffsetEpsilon(targetPos_WS);
        if (ray.TMax <= ray.TMin)
        {
            return false;
        }
        segmentDist = distance(ray.Origin, targetPos_WS);
    }
    else
    {
        segmentDist = distance(ray.Origin, targetPos_WS);
        ray.Direction = (targetPos_WS - ray.Origin) / segmentDist;
        ray.TMax = segmentDist - rayOriginOffsetEpsilon(targetPos_WS);
        if (ray.TMax <= ray.TMin)
        {
            return false;
        }
    }

    Payload payload;
    payload.flags =
        PAYLOAD_FLAG_DID_HIT |
        (canPassthrough ? PAYLOAD_FLAG_REFRACTION_PASSTHROUGH : 0) |
        (startUnderwater ? PAYLOAD_FLAG_UNDERWATER : 0);
    payload.pathWeight = float3(1.f, 1.f, 1.f);
    payload.rng = rng;
    payload.waterEntryT = startUnderwater ? 0.f : RAY_DEFAULT_TMAX;
    payload.waterExitT = RAY_DEFAULT_TMAX;
    payload.rayCone = rayCone;
    const uint rayFlags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER;
    TraceRay(raytracingAcs, rayFlags, 0xFF, HITGROUP_LIGHTS, 0, 0, ray, payload);

    if (bool(payload.flags & PAYLOAD_FLAG_DID_HIT)) // only the miss shader clears this
    {
        return false;
    }

    const float fogTransmittance =
        (applyFog && isFogActive(startUnderwater)) ? computeFogTransmittance(ray.Origin, ray.Direction, segmentDist) : 1.f;
    transmittance = payload.pathWeight * computePassthroughAbsorption(payload, segmentDist) * fogTransmittance;
    return true;
}

// Completes a replayed path at x_{j-1} by connecting to the stored reconnection vertex x_j. Returns
// the product of every factor from x_{j-1}'s scatter onward, to be multiplied by the throughput at
// x_{j-1}. The Jacobian is 1 when the path is replayed at its own pixel. The connecting segment is
// traced the way the original technique traced it: an NEE light vertex gets a shadow ray (own rng
// stream, no fog), anything else the BSDF segment's stream and fog.
float3 evaluateReconnection(const ReplayTarget replay,
                            const Payload payload,
                            const Material surfMaterial,
                            const float2 uv,
                            const float3 wo_WS,
                            const float3 surfPos_WS,
                            const float3 surfNor_WS,
                            const TexSampleCtx surfTexCtx,
                            const bool canPassthrough,
                            const uint pathSeed,
                            const uint vertexIdx,
                            const uint pathDepth)
{
    const PathReservoir path = replay.path;
    const bool rcIsLightVertex = (replay.rcVertexIdx == replay.pathLength);
    const bool rcIsDome = rcIsLightVertex && isDomeTechnique(replay.pathTechnique);
    const bool rcIsNeeLightVertex = rcIsLightVertex &&
        (replay.pathTechnique == PATH_TECHNIQUE_NEE_AREA || replay.pathTechnique == PATH_TECHNIQUE_NEE_DOME);
    RandomNumberGenerator rayRng;
    if (rcIsNeeLightVertex)
    {
        rayRng = pathRng(pathSeed, vertexIdx, rcIsDome ? PATH_RNG_SHADOW_DOME : PATH_RNG_SHADOW_AREA);
    }
    else
    {
        rayRng = pathRng(pathSeed, pathDepth, PATH_RNG_RAY);
    }

    const float3 wi_WS = rcIsDome ? path.rcWi : normalize(path.rcHit.hitPos_WS - surfPos_WS);

    const BsdfEval prevEval = evaluateBsdf(surfMaterial, uv, wo_WS, wi_WS, surfNor_WS, surfTexCtx);
    if (!any(prevEval.value > 0.f))
    {
        return 0.f;
    }
    const float3 prevFactor = prevEval.value * absCosTheta(wi_WS, surfNor_WS);

    const bool isUnderwater = bool(payload.flags & PAYLOAD_FLAG_UNDERWATER);
    float3 transmittance;
    if (!traceReconnectionRay(surfPos_WS, surfNor_WS, wi_WS, rcIsDome, rcIsNeeLightVertex, path.rcHit.hitPos_WS, path.rcHit.hitNor_WS,
            payload.rayCone, canPassthrough, isUnderwater, !rcIsNeeLightVertex, rayRng, transmittance))
    {
        return 0.f;
    }

    if (rcIsLightVertex)
    {
        float lightPdf = rcIsDome
            ? domeLightPdf(wi_WS, surfNor_WS)
            : lightPdfRtsl(path.rcHit, surfPos_WS, surfNor_WS, wi_WS, surfMaterial.acceptsBacksideLight());

        // With their path MIS weights applied, NEE and BSDF sampling of the light vertex both reduce to
        // f * cos * Le / (p_light + p_bsdf)
        return prevFactor * transmittance * path.rcRadiance / (lightPdf + prevEval.pdf);
    }

    if (prevEval.pdf <= 0.f)
    {
        return 0.f;
    }

    // Reconstruct x_j facing the reconnection ray, as ClosestHit_Primary orients hits
    HitInfo rcHit = path.rcHit;
    bool rcIsBackface = bool(path.flags & PATH_FLAGS_RC_HIT_BACKFACE);
    if (dot(rcHit.hitNor_WS, -wi_WS) < 0.f)
    {
        rcHit.hitNor_WS = -rcHit.hitNor_WS;
        rcIsBackface = !rcIsBackface;
    }
    const InstanceData rcInstanceData = instanceDatas[rcHit.instanceId];
    const PerTriangleData rcPerTriData = perTriDatas[rcInstanceData.perTriDatasBufferOffset + rcHit.triangleIdx];
    Material rcMaterial = materials[rcInstanceData.materialIdx];
    if (rcIsBackface)
    {
        rcMaterial.ior = 1.f / rcMaterial.ior;
    }
    if (bool(rcPerTriData.flags & TRIANGLE_FLAG_DIFFUSE_TRANSMISSION))
    {
        rcMaterial.diffuseTransmission = foliageDiffuseTransmission;
    }
    const float rcConeWidth = getRayConeWidthAtDistance(payload.rayCone, distance(surfPos_WS, rcHit.hitPos_WS));
    const TexSampleCtx rcTexCtx = makeTintedTexSampleCtx(rcPerTriData, rcConeWidth, rcHit.hitPos_WS.xz);
    rcMaterial.baseColor = getMaterialBaseColor(rcMaterial, rcHit.uv, rcTexCtx).rgb;
    rcMaterial.baseColorTextureId = TEXTURE_ID_INVALID;

    const BsdfEval rcEval = evaluateBsdf(rcMaterial, rcHit.uv, -wi_WS, path.rcWi, rcHit.hitNor_WS, rcTexCtx);
    const float3 rcFactor = rcEval.value * absCosTheta(path.rcWi, rcHit.hitNor_WS);

    // When x_j precedes the light vertex, the final segment's path MIS weight is recomputed here
    const bool rcPrecedesLight = (replay.rcVertexIdx + 1 == replay.pathLength);
    const float rcDenominator = rcPrecedesLight ? (path.rcLightPdf + rcEval.pdf) : rcEval.pdf;
    if (rcDenominator <= 0.f)
    {
        return 0.f;
    }

    return prevFactor / prevEval.pdf * transmittance * rcFactor / rcDenominator * path.rcRadiance;
}

// Traces one path tree from the primary hit in the gbuffer. In initial sampling mode every complete
// path is fed to the reservoir (ReSTIR PT) or summed into pathColor (other modes), and the terms
// that are not resampled paths (primary emission and miss, fog in-scatter) go straight to pathColor.
// In replay mode only the target path is rebuilt from its seed and its integrand is returned;
// nothing is accumulated and no roulette is applied.
float3 pathTraceRay(inout Payload payload,
                    inout PathTreeReservoir reservoir,
                    const ReplayTarget replay,
                    const uint2 pixelIdx,
                    const uint pathSplitIdx,
                    const uint pathSeed,
                    out float3 pathColor,
                    out float3 ptDiffuseAlbedo)
{
    pathColor = 0.f;
    ptDiffuseAlbedo = 0.f;

    const SamplingMode samplingMode = (SamplingMode)renderParams.samplingMode;
    const bool useRestirPt = (samplingMode == SamplingMode::RESTIR_PT);
    const bool useRtsl = (samplingMode == SamplingMode::RTSL || useRestirPt);
    const bool doMis = (samplingMode == SamplingMode::MIS || useRtsl);
    const bool isReplay = replay.active;

    RayDesc ray;
    ray.Direction = getPrimaryRayDirection(pixelIdx); // same direction as gbuffer ray, used for calculating wo_WS the first time

    float3 throughput = 1.f; // includes the roulette division
    float rrProduct = 1.f;   // roulette survival product, so the stored integrand can exclude it
    float3 rcThroughput = 1.f; // factors applied after the reconnection vertex's scatter (no roulette)

    const float3 primarySegmentAbsorption = computeSegmentAbsorption(payload, cameraParams.pos_WS, ray.Direction);

    // The primary segment is identical for both path splits and collect sums them, so
    // in-scattered radiance is added only by split 0 (same as emission and the dome light miss).
    RandomNumberGenerator primaryFogRng = pathRng(pathSeed, 0, PATH_RNG_FOG);
    applySegmentFog(payload, primaryFogRng, cameraParams.pos_WS, ray.Direction,
        (pathSplitIdx == 0 && !isReplay) ? renderParams.fogMarchSteps : 0u, throughput, pathColor);

    throughput *= primarySegmentAbsorption;

    if (isOrphanWaterBackfaceHit(payload))
    {
        return 0.f;
    }

    if (!bool(payload.flags & PAYLOAD_FLAG_DID_HIT))
    {
        if (!isReplay)
        {
            const float3 domeLightColor = (pathSplitIdx == 0) ? getDomeLightColor(ray.Direction) : 0.f;
            pathColor += throughput * domeLightColor;
            if (sceneParams.voxelMode == 1)
            {
                // Give the sky an albedo so DLSS doesn't see it as black. Uses the unattenuated dome
                // light rather than throughput, which would fold in fog transmittance.
                ptDiffuseAlbedo = applyReinhard(domeLightColor);
            }
        }
        return 0.f;
    }

    if (payload.materialIdx == MATERIAL_IDX_INVALID)
    {
        return 0.f;
    }

    // data of last "real" bounce (i.e. not passthrough)
    bool bounceWasSpecular = false; // TODO: pack this and bounceAcceptedBacksideLight together (and see if they can be eliminated entirely)
    bool bounceAcceptedBacksideLight = false;
    float bounceBsdfPdf = 0.f;
    float bounceLobeRoughness = 0.f;
    float3 surfPos_WS, surfNor_WS;
    // the real bounce before that, for the reconnection test between the two
    float3 prevSurfPos_WS = cameraParams.pos_WS;
    float3 prevSurfNor_WS = 0.f;

    bool hasEncounteredNonDeltaSurface = false;

    // Emission seen at the primary hit, kept apart from the scattered part of the albedo guide so the
    // specular look-through below can modulate the scattered part alone; folded in after the loop.
    float3 ptEmissiveAlbedo = 0.f;

    if (sceneParams.voxelMode == 1 && debugParams.colorChunks == 1)
    {
        float3 surfPos_WS = payload.hitInfo.hitPos_WS;
        surfPos_WS.xz += cameraParams.globalInstanceOffset.xz;
        const int2 chunkPosBlocksXZ_WS = int2(floor(surfPos_WS.xz / 16.f)); // should be chunkSizeXZ instead of 16.f but whatever
        const float3 chunkColor = (chunkPosBlocksXZ_WS.x + chunkPosBlocksXZ_WS.y /*z*/) % 2 == 0 ? float3(1.f, 0.5f, 0.5f) : float3(0.5f, 1.f, 1.f);
        throughput *= chunkColor;
    }

    Material surfMaterial = getMaterialFromPayload(payload);

    // Vertex indices follow the papers: x1 is the primary hit, passthrough hits are not vertices
    uint vertexIdx = 1;
    const float footprintThreshold =
        reconnectionFootprintThreshold(cameraParams.pos_WS, payload.hitInfo.hitPos_WS, payload.hitInfo.hitNor_WS);
    RcState rc;
    rc.vertexIdx = 0;
    rc.hit = payload.hitInfo;
    rc.hitIsBackface = false;
    rc.wi = 0.f;

    const uint effectiveMaxPathDepth = renderParams.maxPathDepth;
    for (uint pathDepth = 0; pathDepth < effectiveMaxPathDepth; ++pathDepth)
    {
        const InstanceData instanceData = instanceDatas[payload.hitInfo.instanceId];
        const PerTriangleData perTriData = perTriDatas[instanceData.perTriDatasBufferOffset + payload.hitInfo.triangleIdx];
        if (bool(perTriData.flags & TRIANGLE_FLAG_DIFFUSE_TRANSMISSION))
        {
            surfMaterial.diffuseTransmission = foliageDiffuseTransmission;
        }
        const bool hitWasWater = bool(perTriData.flags & TRIANGLE_FLAG_IS_WATER);
        const TexSampleCtx surfTexCtx =
            makeTintedTexSampleCtx(perTriData, payload.rayCone.width, payload.hitInfo.hitPos_WS.xz);

        // On the first bounce, emission is handled only by pathSplitIdx 0 to prevent having to handle it twice and multiply by Fresnel reflectance
        float3 Le = 0.f;
        if ((pathSplitIdx == 0 || pathDepth > 0) && surfMaterial.hasEmission())
        {
            Le = getMaterialEmissiveColor(surfMaterial, payload.hitInfo.uv, surfTexCtx);
        }

        const float3 wo_WS = -ray.Direction;

        if (pathDepth == 0 && bool(renderParams.doPathSplitting))
        {
            const bool didSplitMaterial = trySplitMaterial(
                surfMaterial, payload.hitInfo.uv, payload.hitInfo.hitNor_WS, wo_WS, surfTexCtx, pathSplitIdx, throughput);
            if (!didSplitMaterial && pathSplitIdx == 1)
            {
                break;
            }
        }

        // Resolve the sampled base color once per bounce so downstream albedo and BSDF reads take
        // the constant-color path instead of re-sampling the base and aux textures every call.
        surfMaterial.baseColor = getMaterialBaseColor(surfMaterial, payload.hitInfo.uv, surfTexCtx).rgb;
        surfMaterial.baseColorTextureId = TEXTURE_ID_INVALID;

        // In voxel mode all terrain shares one material with hasDiffuse=true; emissive blocks
        // like LAMP/LAVA have zero diffuse in the texture, so skip scatter work in that case
        // to avoid pointless shadow rays and BSDF sampling.
        bool isPureEmitter = false;
        if (any(Le > 0))
        {
            if (pathDepth == 0)
            {
                if (!isReplay)
                {
                    const float3 emissiveContrib = throughput * Le;
                    pathColor += emissiveContrib;
                    ptEmissiveAlbedo = applyReinhard(emissiveContrib);
                }
            }
            else
            {
                // x_vertexIdx is a light vertex reached by BSDF sampling; the path MIS weight against
                // light sampling belongs to this path only, not to the throughput continuing past it
                float lightPdf = 0.f;
                float misWeight = 1.f;
                if (doMis && !bounceWasSpecular)
                {
                    lightPdf = useRtsl
                        ? lightPdfRtsl(payload.hitInfo, surfPos_WS, surfNor_WS, ray.Direction, bounceAcceptedBacksideLight)
                        : lightPdfUniform(payload.hitInfo, surfPos_WS, ray.Direction);
                    misWeight = balanceHeuristic(bounceBsdfPdf, lightPdf);
                }
                const float3 F = throughput * Le * misWeight;

                if (isReplay)
                {
                    if (replay.pathTechnique == PATH_TECHNIQUE_BSDF_EMISSION && vertexIdx == replay.pathLength && replay.rcVertexIdx == 0)
                    {
                        return F;
                    }
                }
                else
                {
                    PathCandidate candidate = makePathCandidate(F, rrProduct, vertexIdx, PATH_TECHNIQUE_BSDF_EMISSION);
                    if (rc.vertexIdx != 0)
                    {
                        setCandidateRcFromState(candidate, rc, ray.Direction, rcThroughput * Le, rcThroughput * Le * misWeight, lightPdf);
                    }
                    else if (useRestirPt && isReconnectionVertex(bounceLobeRoughness, bounceBsdfPdf, surfPos_WS, surfNor_WS,
                                 payload.hitInfo.hitPos_WS, payload.hitInfo.hitNor_WS, 0.f, false, false, footprintThreshold))
                    {
                        setCandidateRcAtLightVertex(candidate, payload.hitInfo, bool(payload.flags & PAYLOAD_FLAG_BACKFACE_HIT), 0.f, Le);
                    }
                    addPathCandidate(reservoir, candidate, useRestirPt, pathColor);
                }
            }

            const bool isDiffuseOnly = surfMaterial.hasDiffuse()
                && !surfMaterial.hasGlossyReflection()
                && !surfMaterial.hasGlossyTransmission();
            if (isDiffuseOnly)
            {
                const float3 baseColor = getMaterialBaseColor(surfMaterial, payload.hitInfo.uv, surfTexCtx).rgb;
                isPureEmitter = !any(baseColor > 0.f);
            }
        }

        const bool isLastBounce = (pathDepth == effectiveMaxPathDepth - 1);
        if (!surfMaterial.canScatter() || isLastBounce || isPureEmitter)
        {
            break;
        }

        const bool isDeltaSurface = surfMaterial.isDelta();

        // canPassthrough = has the path encountered a non-delta surface (including this one)
        // isPassthrough = this intersection has glossy transmission and should be passed through
        const bool canPassthrough = bool(renderParams.refractionIndirectPassthrough) && (!isDeltaSurface || hasEncounteredNonDeltaSurface);
        const bool isPassthrough = canPassthrough && surfMaterial.isDeltaTransmission();

        // If this is a passthrough "bounce", we don't care about its hit pos/nor and want to instead preserve the last
        // "real" bounce's information. This is important for matching MIS weights with direct light sampling, which
        // traces only one ray and ignores passthrough surfaces in the anyhit shader.
        if (!isPassthrough)
        {
            if (vertexIdx > 1)
            {
                prevSurfPos_WS = surfPos_WS;
                prevSurfNor_WS = surfNor_WS;
            }
            surfNor_WS = payload.hitInfo.hitNor_WS;
            surfPos_WS = payload.hitInfo.hitPos_WS;
        }

        const uint coherenceHint =
            (pathDepth == 0 ? (1 << 2) : 0) |
            (isPassthrough ? (1 << 1) : 0) |
            ((!isDeltaSurface && surfMaterial.canScatter()) ? (1 << 0) : 0);
        NvReorderThread(coherenceHint, 3 /*numCoherenceHintBits*/);

        if (isPassthrough)
        {
            const float3 passthroughTint = getMaterialBaseColor(surfMaterial, payload.hitInfo.uv, surfTexCtx).rgb;
            throughput *= passthroughTint;
            rcThroughput *= passthroughTint;
            if (hitWasWater)
            {
                setUnderwaterFromHit(payload, bool(payload.flags & PAYLOAD_FLAG_BACKFACE_HIT));
            }
            setRayOriginAndDirection(ray, payload.hitInfo.hitPos_WS, payload.hitInfo.hitNor_WS, ray.Direction, true /*faceforwardNormal*/);
            // bounceBsdfPdf, bounceWasSpecular, etc. are intentionally preserved from the last real BSDF sample
        }
        else // !isPassthrough
        {
            if (isReplay && replay.rcVertexIdx != 0 && vertexIdx + 1 == replay.rcVertexIdx)
            {
                return throughput * evaluateReconnection(replay, payload, surfMaterial, payload.hitInfo.uv, wo_WS, surfPos_WS,
                    surfNor_WS, surfTexCtx, canPassthrough, pathSeed, vertexIdx, pathDepth);
            }

            if (isReplay && vertexIdx >= replay.pathLength) // the target path ended here without matching
            {
                return 0.f;
            }

            // Russian roulette only shapes the initial samples; replay must never kill a stored path
            if (!isReplay && pathDepth >= 2)
            {
                const float survivalProbability = max(saturate(luminance(throughput)), 0.1f);
                RandomNumberGenerator rouletteRng = pathRng(pathSeed, vertexIdx, PATH_RNG_ROULETTE);
                if (rouletteRng.nextFloat() >= survivalProbability)
                {
                    break;
                }
                throughput /= survivalProbability;
                rrProduct *= survivalProbability;
            }

            RandomNumberGenerator bsdfRng = pathRng(pathSeed, vertexIdx, PATH_RNG_BSDF);
            const BsdfSample surfBsdfSample = sampleBsdf(surfMaterial, payload.hitInfo.uv, wo_WS, surfNor_WS, surfTexCtx, bsdfRng);

            // The reconnection vertex is the first x_j (j >= 2) whose pair with x_{j-1} passes the criteria.
            // Decided before this vertex's light samples, since those paths reconnect here too.
            if (useRestirPt && !isReplay && rc.vertexIdx == 0 && vertexIdx >= 2 &&
                isReconnectionVertex(bounceLobeRoughness, bounceBsdfPdf, prevSurfPos_WS, prevSurfNor_WS, surfPos_WS, surfNor_WS,
                    surfBsdfSample.pdf, surfBsdfSample.wasSpecular, surfMaterial.hasGlossy(), footprintThreshold))
            {
                rc.vertexIdx = vertexIdx;
                rc.hit = payload.hitInfo;
                rc.hitIsBackface = bool(payload.flags & PAYLOAD_FLAG_BACKFACE_HIT);
                rc.wi = surfBsdfSample.wi_WS;
            }

            if (doMis && surfMaterial.canScatter() && !isDeltaSurface)
            {
                const bool isUnderwater = payload.flags & PAYLOAD_FLAG_UNDERWATER;
                // Replay only re-samples a light when the target path ends with NEE from this vertex and
                // has no reconnection vertex (NEE paths otherwise always reconnect to their light vertex)
                const bool replayWantsNee = isReplay && replay.rcVertexIdx == 0 && vertexIdx + 1 == replay.pathLength;

                // ------------------------------
                // sample area lights
                // ------------------------------

                if (!isReplay || (replayWantsNee && replay.pathTechnique == PATH_TECHNIQUE_NEE_AREA))
                {
                    RandomNumberGenerator neeRng = pathRng(pathSeed, vertexIdx, PATH_RNG_NEE_AREA);
                    const RandomNumberGenerator shadowRng = pathRng(pathSeed, vertexIdx, PATH_RNG_SHADOW_AREA);
                    DirectLightingSample lightSample;
                    if (useRtsl)
                    {
                        lightSample = sampleDirectLightingRtsl(
                            surfPos_WS, surfNor_WS, payload.rayCone, canPassthrough, isUnderwater,
                            surfMaterial.acceptsBacksideLight(), neeRng, shadowRng);
                    }
                    else
                    {
                        lightSample = sampleDirectLightingUniform(
                            surfPos_WS, surfNor_WS, payload.rayCone, canPassthrough, isUnderwater, neeRng, shadowRng);
                    }

                    if (lightSample.didHitLight)
                    {
                        // no need to consider dome light pdf because dome light sampling can't hit area lights

                        const BsdfEval bsdfEval =
                            evaluateBsdf(surfMaterial, payload.hitInfo.uv, wo_WS, lightSample.wi_WS, surfNor_WS, surfTexCtx);

                        // light pdf in balance heuristic numerator cancels out with divide by pdf
                        const float3 lightFactor = bsdfEval.value * absCosTheta(lightSample.wi_WS, surfNor_WS) *
                                                   lightSample.Le * lightSample.transmittance / (lightSample.pdf + bsdfEval.pdf);
                        const float3 F = throughput * lightFactor;

                        if (isReplay)
                        {
                            return F;
                        }

                        PathCandidate candidate = makePathCandidate(F, rrProduct, vertexIdx + 1, PATH_TECHNIQUE_NEE_AREA);
                        if (rc.vertexIdx != 0)
                        {
                            setCandidateRcFromState(candidate, rc, lightSample.wi_WS, lightSample.Le * lightSample.transmittance,
                                rcThroughput * lightFactor, lightSample.pdf);
                        }
                        else
                        {
                            // Forced light reconnection (Lin et al. 2026, Section 6.2.3): replay never re-samples lights
                            setCandidateRcAtLightVertex(candidate, lightSample.lightHit, false, 0.f, lightSample.Le);
                            candidate.rcLightPdf = lightSample.pdf;
                        }
                        addPathCandidate(reservoir, candidate, useRestirPt, pathColor);
                    }
                }

                // ------------------------------
                // sample dome light
                // ------------------------------

                if (sceneParams.voxelMode == 1 && (!isReplay || (replayWantsNee && replay.pathTechnique == PATH_TECHNIQUE_NEE_DOME)))
                {
                    RandomNumberGenerator domeRng = pathRng(pathSeed, vertexIdx, PATH_RNG_NEE_DOME);
                    const DomeLightSample domeLightSample = sampleDomeLight(surfPos_WS, surfNor_WS, payload.rayCone,
                        canPassthrough, isUnderwater, surfMaterial.acceptsBacksideLight(), domeRng,
                        pathRng(pathSeed, vertexIdx, PATH_RNG_SHADOW_DOME));
                    if (domeLightSample.didReachDomeLight)
                    {
                        // no need to consider area light pdf because area light sampling can't hit dome light

                        const BsdfEval bsdfEval = evaluateBsdf(
                            surfMaterial, payload.hitInfo.uv, wo_WS, domeLightSample.wi_WS, surfNor_WS, surfTexCtx);

                        // dome light pdf in balance heuristic numerator cancels out with divide by pdf
                        const float3 lightFactor = bsdfEval.value * absCosTheta(domeLightSample.wi_WS, surfNor_WS) *
                                                   domeLightSample.Le * domeLightSample.transmittance / (domeLightSample.pdf + bsdfEval.pdf);
                        const float3 F = throughput * lightFactor;

                        if (isReplay)
                        {
                            return F;
                        }

                        PathCandidate candidate = makePathCandidate(F, rrProduct, vertexIdx + 1, PATH_TECHNIQUE_NEE_DOME);
                        if (rc.vertexIdx != 0)
                        {
                            setCandidateRcFromState(candidate, rc, domeLightSample.wi_WS, domeLightSample.Le * domeLightSample.transmittance,
                                rcThroughput * lightFactor, domeLightSample.pdf);
                        }
                        else
                        {
                            setCandidateRcAtLightVertex(candidate, rc.hit, false, domeLightSample.wi_WS, domeLightSample.Le);
                        }
                        addPathCandidate(reservoir, candidate, useRestirPt, pathColor);
                    }
                }
            }

            if (!isDeltaSurface)
            {
                hasEncounteredNonDeltaSurface = true;
            }

            float3 scatterFactor = surfBsdfSample.bsdfValue / surfBsdfSample.pdf;
            if (!surfBsdfSample.wasSpecular)
            {
                scatterFactor *= absCosTheta(surfBsdfSample.wi_WS, surfNor_WS);
            }
            throughput *= scatterFactor;
            rcThroughput = (rc.vertexIdx == vertexIdx) ? 1.f : rcThroughput * scatterFactor;

            if (hitWasWater && dot(surfBsdfSample.wi_WS, surfNor_WS) < 0.f) // apply only for rays that will transmit through the water
            {
                setUnderwaterFromHit(payload, bool(payload.flags & PAYLOAD_FLAG_BACKFACE_HIT));
            }

            if (pathDepth == 0 && !isReplay)
            {
                ptDiffuseAlbedo = throughput;
            }

            if (all(throughput == 0.f)) // dead BSDF sample; nothing further can contribute
            {
                break;
            }

            setRayOriginAndDirection(ray, surfPos_WS, surfNor_WS, surfBsdfSample.wi_WS, true /*faceforwardNormal*/);

            bounceBsdfPdf = surfBsdfSample.pdf;
            bounceWasSpecular = surfBsdfSample.wasSpecular;
            bounceAcceptedBacksideLight = surfMaterial.acceptsBacksideLight();
            bounceLobeRoughness = surfBsdfSample.lobeRoughness;
            ++vertexIdx;
        } // !isPassthrough

        ray.TMin = 0.f;
        ray.TMax = RAY_DEFAULT_TMAX;

        payload.flags &= PAYLOAD_FLAG_UNDERWATER; // reset all payload flags except PAYLOAD_FLAG_UNDERWATER
        payload.waterEntryT = RAY_DEFAULT_TMAX;
        payload.waterExitT = RAY_DEFAULT_TMAX;
        payload.rng = pathRng(pathSeed, pathDepth, PATH_RNG_RAY);
        TraceRay(raytracingAcs, RAY_FLAG_NONE, 0xFF, HITGROUP_PRIMARY, 0, 0, ray, payload);

        if (bool(payload.flags & PAYLOAD_FLAG_DID_HIT) && payload.materialIdx != MATERIAL_IDX_INVALID)
        {
            surfMaterial = getMaterialFromPayload(payload);

            const float hitDistance = distance(ray.Origin, payload.hitInfo.hitPos_WS);
            payload.rayCone.width = getRayConeWidthAtDistance(payload.rayCone, hitDistance);

            if (surfMaterial.hasDiffuse())
            {
                payload.rayCone.angle += 0.5f;
            }
        }

        const float3 segmentAbsorption = computeSegmentAbsorption(payload, ray.Origin, ray.Direction);

        // Bounces at pathDepth > 1 get only transmittance, no in-scattering.
        const uint numFogSteps = (!isReplay && pathDepth <= 1) ? max(renderParams.fogMarchSteps / 2, 1u) : 0u;
        RandomNumberGenerator bounceFogRng = pathRng(pathSeed, pathDepth + 1, PATH_RNG_FOG);
        const float fogTransmittance = applySegmentFog(payload, bounceFogRng, ray.Origin, ray.Direction, numFogSteps, throughput, pathColor);

        throughput *= segmentAbsorption;
        rcThroughput *= segmentAbsorption * fogTransmittance;

        if (isOrphanWaterBackfaceHit(payload))
        {
            break;
        }

        const bool didMiss = !bool(payload.flags & PAYLOAD_FLAG_DID_HIT);
        const float3 missDomeLightColor = didMiss ? getDomeLightColor(ray.Direction) : float3(0.f, 0.f, 0.f);

        if (pathDepth == 0 && !isReplay)
        {
            // at this point, ptDiffuseAlbedo = first bounce path weight

            if (bounceWasSpecular)
            {
                if (!bool(renderParams.doPathSplitting))
                {
                    ptDiffuseAlbedo = 0.f;
                }
                else
                {
                    float3 secondHitDiffuseAlbedo = 0.f;
                    if (bool(payload.flags & PAYLOAD_FLAG_DID_HIT) && payload.materialIdx != MATERIAL_IDX_INVALID)
                    {
                        const PerTriangleData secondHitPerTriData =
                            perTriDatas[instanceDatas[payload.hitInfo.instanceId].perTriDatasBufferOffset + payload.hitInfo.triangleIdx];
                        const TexSampleCtx secondHitTexCtx = makeTintedTexSampleCtx(
                            secondHitPerTriData, payload.rayCone.width, payload.hitInfo.hitPos_WS.xz);
                        if (surfMaterial.hasDiffuse())
                        {
                            secondHitDiffuseAlbedo += getMaterialBaseColor(surfMaterial, payload.hitInfo.uv, secondHitTexCtx).rgb;
                        }
                        if (surfMaterial.hasEmission())
                        {
                            secondHitDiffuseAlbedo +=
                                applyReinhard(getMaterialEmissiveColor(surfMaterial, payload.hitInfo.uv, secondHitTexCtx));
                        }
                    }
                    const bool secondHitHasDiffuseAlbedo = any(secondHitDiffuseAlbedo > 0.f);

                    if (secondHitHasDiffuseAlbedo)
                    {
                        ptDiffuseAlbedo *= secondHitDiffuseAlbedo * segmentAbsorption * fogTransmittance;
                    }
                    else if (didMiss && sceneParams.voxelMode == 1)
                    {
                        // Specular reflection of the sky. Excludes fog and absorption to match
                        // how the primary miss builds its albedo.
                        ptDiffuseAlbedo *= applyReinhard(missDomeLightColor);
                    }
                    else
                    {
                        ptDiffuseAlbedo = 0.f;
                    }
                }
            }

            // if !bounceWasSpecular, ptDiffAlbedo remains unchanged
        }

        if (didMiss)
        {
            float domePdf = 0.f;
            float misWeight = 1.f;
            if (doMis)
            {
                domePdf = domeLightPdf(ray.Direction, surfNor_WS); // 0 if !voxelMode
                misWeight = balanceHeuristic(bounceBsdfPdf, domePdf);
            }
            const float3 F = throughput * missDomeLightColor * misWeight;

            if (isReplay)
            {
                const bool matches = replay.pathTechnique == PATH_TECHNIQUE_BSDF_DOME && vertexIdx == replay.pathLength && replay.rcVertexIdx == 0;
                return matches ? F : 0.f;
            }

            PathCandidate candidate = makePathCandidate(F, rrProduct, vertexIdx, PATH_TECHNIQUE_BSDF_DOME);
            if (rc.vertexIdx != 0)
            {
                setCandidateRcFromState(candidate, rc, ray.Direction, rcThroughput * missDomeLightColor,
                    rcThroughput * missDomeLightColor * misWeight, domePdf);
            }
            else if (useRestirPt && isDomeReconnectionVertex(bounceLobeRoughness))
            {
                setCandidateRcAtLightVertex(candidate, rc.hit, false, ray.Direction, missDomeLightColor);
            }
            addPathCandidate(reservoir, candidate, useRestirPt, pathColor);
            break;
        }
        else if (payload.materialIdx == MATERIAL_IDX_INVALID)
        {
            break;
        }

        if (!isReplay && bool(renderParams.doPathSplitting) && pathDepth == 0 && bounceWasSpecular) // TODO: support multiple specular bounces?
        {
            if (pathSplitIdx == 0) // transmission
            {
                RWTexture2D<float4> normalsAndRoughnessTarget = ResourceDescriptorHeap[heapIndices.uav.normalsAndRoughnessTargetIdx];
                normalsAndRoughnessTarget[pixelIdx].xyz = payload.hitInfo.hitNor_WS;
            }
            else // reflection
            {
                RWTexture2D<float> specularHitDistanceTarget = ResourceDescriptorHeap[heapIndices.uav.specularHitDistanceTargetIdx];
                specularHitDistanceTarget[pixelIdx] = distance(surfPos_WS, payload.hitInfo.hitPos_WS);
            }
        }
    }

    ptDiffuseAlbedo = saturate(ptDiffuseAlbedo + ptEmissiveAlbedo);
    return 0.f;
}

[shader("raygeneration")]
void RayGeneration()
{
    const uint2 pixelIdx = getPixelIdx();
    const uint pathSplitIdx = getPathSplitIdx();

    const uint linearPixelIdx = pixelIdx.y * renderParams.renderSize.x + pixelIdx.x;
    const uint slotIdx = linearPixelIdx * (bool(renderParams.doPathSplitting) ? 2 : 1) + pathSplitIdx;

    const GbufferData gbufferData = gbufferIn[linearPixelIdx];
    Payload payload = initPayloadFromGbuffer(gbufferData);

    const uint pathSeed = initRng(constantParams.rngSeed, 987654103, slotIdx, renderParams.frameNumber).seed;
    // Separate stream from the path's RNG so resampling draws never perturb the path itself
    PathTreeReservoir reservoir = initPathTreeReservoir(
        initRng(constantParams.rngSeed, 192837465, slotIdx, renderParams.frameNumber), pathSeed, pathSplitIdx);

    float3 pathColor = 0.f;
    float3 outPtDiffuseAlbedo = 0.f;
    pathTraceRay(payload, reservoir, noReplay(), pixelIdx, pathSplitIdx, pathSeed, pathColor, outPtDiffuseAlbedo);

    if ((SamplingMode)renderParams.samplingMode == SamplingMode::RESTIR_PT)
    {
        reservoirsOut[slotIdx] = reservoir.finalize();
        const PathReservoir stored = reservoirsOut[slotIdx];

        const RestirDebugMode debugMode = (RestirDebugMode)renderParams.restirDebugMode;
        if (debugMode == RestirDebugMode::OFF)
        {
            pathColor += stored.F * stored.W;
        }
        else
        {
            float3 replayedF = 0.f;
            if (stored.W > 0.f)
            {
                Payload replayPayload = initPayloadFromGbuffer(gbufferData);
                float3 unusedColor, unusedAlbedo;
                replayedF = pathTraceRay(replayPayload, reservoir, makeReplayTarget(stored), pixelIdx, pathSplitIdx, stored.seed,
                    unusedColor, unusedAlbedo);
            }

            if (debugMode == RestirDebugMode::SELF_REPLAY)
            {
                pathColor += replayedF * stored.W;
            }
            else // SELF_REPLAY_ERROR
            {
                pathColor = (stored.W > 0.f) ? 100.f * abs(replayedF - stored.F) / max(luminance(stored.F), 1e-6f) : 0.f;
                outPtDiffuseAlbedo = 0.f;
            }
        }
    }

    if ((AntialiasingMode)renderParams.antialiasingMode == AntialiasingMode::ACCUMULATE && renderParams.accumulatedFrameNumber > 0)
    {
        pathTracingRawBufferOut[slotIdx].xyz += pathColor;
    }
    else
    {
        pathTracingRawBufferOut[slotIdx].xyz = pathColor;
    }

    ptDiffuseAlbedoRawBufferOut[slotIdx] = float4(outPtDiffuseAlbedo, 0.f);
}
