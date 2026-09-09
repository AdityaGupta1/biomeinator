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
#include "restir/pairing.hlsli"
#include "restir/pairwise_mis.hlsli"
#include "restir/path_reservoir.hlsli"
#include "restir/reconnection.hlsli"
#include "restir/temporal.hlsli"
#include "util/color.hlsli"
#include "util/math.hlsli"

StructuredBuffer<GbufferData> gbufferIn : REGISTER_T(PT, GBUFFER_IN);

#define RESTIR_ATTRIBUTION_TEST 0 // TEMP
static uint debugZeroReason = 0; // TEMP
static uint debugZeroKind = 0; // TEMP

RWStructuredBuffer<float4> pathTracingRawBufferOut : REGISTER_U(PT, PATH_TRACING_RAW_BUFFER_OUT);
RWStructuredBuffer<float4> ptDiffuseAlbedoRawBufferOut : REGISTER_U(PT, PT_DIFFUSE_ALBEDO_RAW_BUFFER_OUT);
RWStructuredBuffer<PathReservoir> reservoirsOut : REGISTER_U(PT, RESERVOIRS_OUT);
RWStructuredBuffer<PathReservoir> reservoirsMergedOut : REGISTER_U(PT, RESERVOIRS_MERGED_OUT);
RWStructuredBuffer<ShiftedPath> shiftedOut : REGISTER_U(PT, SHIFTED_OUT);
#if RESTIR_SHIFT_STATS
RWStructuredBuffer<uint> restirStatsOut : REGISTER_U(PT, RESTIR_STATS_OUT);
#endif
StructuredBuffer<uint> pairingTextures : REGISTER_T(PT, PAIRING_TEXTURES_IN);
StructuredBuffer<GbufferData> gbufferPrevIn : REGISTER_T(PT, GBUFFER_PREV_IN);
StructuredBuffer<PathReservoir> reservoirsHistoryIn : REGISTER_T(PT, RESERVOIRS_HISTORY_IN);
StructuredBuffer<float> duplicationMapIn : REGISTER_T(PT, DUPLICATION_MAP_IN);

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

Payload initPayloadFromGbuffer(const GbufferData gbufferData, const float3 cameraPos_WS)
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
        ? payload.rayCone.angle * distance(cameraPos_WS, payload.hitInfo.hitPos_WS)
        : 0.f;
    return payload;
}

// The path selected by a reservoir, to be rebuilt from its seed instead of sampling a new path tree.
// Holds only what replay reads, so F, W and M are not live through the replay.
struct ReplayTarget
{
    bool active;
    uint pathLength;
    uint rcVertexIdx;
    uint pathTechnique;
    bool rcPrevLobeDiffuse;
    uint rcInstance;
    uint rcTriangleIdx;
    uint rcBarycentrics;
    uint rcWi;
    float3 rcRadiance;
    float rcLightPdf;
    float rcJacobianTerms;
};

ReplayTarget makeReplayTarget(const PathReservoir path)
{
    ReplayTarget replay;
    replay.active = true;
    replay.pathLength = getPathLength(path.flags);
    replay.rcVertexIdx = getRcVertexIdx(path.flags);
    replay.pathTechnique = getPathTechnique(path.flags);
    replay.rcPrevLobeDiffuse = bool(path.flags & PATH_FLAGS_RC_PREV_LOBE_DIFFUSE);
    replay.rcInstance = path.rcInstance;
    replay.rcTriangleIdx = path.rcTriangleIdx;
    replay.rcBarycentrics = path.rcBarycentrics;
    replay.rcWi = path.rcWi;
    replay.rcRadiance = path.rcRadiance;
    replay.rcLightPdf = path.rcLightPdf;
    replay.rcJacobianTerms = path.rcJacobianTerms;
    return replay;
}

ReplayTarget noReplay()
{
    ReplayTarget replay;
    replay.active = false;
    replay.pathLength = 0;
    replay.rcVertexIdx = 0;
    replay.pathTechnique = 0;
    replay.rcPrevLobeDiffuse = false;
    replay.rcInstance = 0;
    replay.rcTriangleIdx = 0;
    replay.rcBarycentrics = 0;
    replay.rcWi = 0;
    replay.rcRadiance = 0.f;
    replay.rcLightPdf = 0.f;
    replay.rcJacobianTerms = 0.f;
    return replay;
}

// The reconnection vertex chosen so far while tracing a path tree
// Kept packed as the reservoir stores it: this state and the selected reservoir stay live across
// every TraceRay of the path, where registers are what the raygen is short of
struct RcState
{
    uint vertexIdx; // 0 = none yet
    uint instance;
    uint triangleIdx;
    uint barycentrics;
    bool prevLobeDiffuse; // lobe sampled at the vertex before
    uint wi; // octahedral-encoded direction sampled at the rc vertex
    float prevPdfTimesGeom; // pdf of the direction that reached the rc vertex, times its geometry term
    float pdf; // pdf of wi
};

// Loop-carried facts about the last real bounce of pathTraceRay
#define BOUNCE_FLAG_WAS_SPECULAR (1 << 0)
#define BOUNCE_FLAG_ACCEPTED_BACKSIDE_LIGHT (1 << 1)
#define BOUNCE_FLAG_SAMPLED_DIFFUSE (1 << 2)
#define BOUNCE_FLAG_ENCOUNTERED_NON_DELTA (1 << 3) // the path has passed a non-delta surface (including the current one)
#define BOUNCE_FLAG_AREA_NEE (1 << 4) // area-light NEE was allowed at the vertex the last bounce left, see areaNeeAllowed

// Debug toggle: debugBool1 restricts area-light NEE to the first non-delta vertex (dome NEE stays on at
// every vertex), to see whether later-bounce area NEE earns its cost. Where NEE is off, BSDF-sampled
// light hits take the whole path MIS weight, so the estimate stays unbiased. Only valid before the
// vertex's own ENCOUNTERED_NON_DELTA update.
bool areaNeeAllowed(const uint bounceFlags)
{
    return !(bool(debugParams.debugBool1) && bool(bounceFlags & BOUNCE_FLAG_ENCOUNTERED_NON_DELTA));
}

// Routes a complete path's contribution to the reservoir when ReSTIR PT is on, otherwise
// straight into pathColor. Terms that are not resampled paths (primary emission, primary miss,
// fog in-scatter) always go straight into pathColor.
void addPathCandidate(inout PathTreeReservoir reservoir,
                      const PathCandidate candidate,
                      const bool useRestirPt,
                      const uint pathSeed,
                      const uint pathSplitIdx,
                      inout float3 pathColor)
{
    if (useRestirPt)
    {
        if (reservoir.addCandidate(candidate))
        {
            reservoirsOut[reservoir.slotIdx] = packCandidate(candidate, pathSeed, pathSplitIdx);
        }
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
    candidate.rcPrevLobeDiffuse = false;
    candidate.rcJacobianTerms = 0.f;
    candidate.rcInstance = 0;
    candidate.rcTriangleIdx = 0;
    candidate.rcBarycentrics = 0;
    candidate.rcWi = 0;
    candidate.rcRadiance = 0.f;
    candidate.rcLightPdf = 0.f;
    return candidate;
}

// Reconnection data for a candidate whose rc vertex x_j was found before its light vertex x_k. When
// j == k - 1 the stored direction is the final segment's and the stored radiance excludes the path
// MIS weight, which replay recomputes from its own bsdf pdf against rcLightPdf. A final NEE segment
// contributes no pdf to the Jacobian terms since light sampling ignores the incoming direction.
void setCandidateRcFromState(inout PathCandidate candidate, const RcState rc, const float3 finalSegmentDir,
    const float3 radianceIfRcPrecedesLight, const float3 radianceOtherwise, const float lightPdf, const bool finalSegmentIsNee)
{
    const bool rcPrecedesLight = (rc.vertexIdx + 1 == candidate.pathLength);
    candidate.rcVertexIdx = rc.vertexIdx;
    candidate.rcInstance = rc.instance;
    candidate.rcTriangleIdx = rc.triangleIdx;
    candidate.rcBarycentrics = rc.barycentrics;
    candidate.rcPrevLobeDiffuse = rc.prevLobeDiffuse;
    candidate.rcWi = rcPrecedesLight ? octEncode(finalSegmentDir) : rc.wi;
    candidate.rcRadiance = rcPrecedesLight ? radianceIfRcPrecedesLight : radianceOtherwise;
    candidate.rcLightPdf = lightPdf;
    candidate.rcJacobianTerms = rc.prevPdfTimesGeom * ((rcPrecedesLight && finalSegmentIsNee) ? 1.f : rc.pdf);
}

// Reconnection data for a candidate that reconnects straight to its light vertex. `jacobianTerms`
// is the technique's pdf of the light vertex times its geometry term.
void setCandidateRcAtLightVertex(inout PathCandidate candidate, const HitInfo lightHit, const uint lightInstanceGeneration,
    const float3 emission, const float jacobianTerms, const bool prevLobeDiffuse)
{
    candidate.rcVertexIdx = candidate.pathLength;
    candidate.rcInstance = packRcInstance(lightInstanceGeneration, lightHit.instanceId);
    candidate.rcTriangleIdx = lightHit.triangleIdx;
    candidate.rcBarycentrics = packBarycentrics(lightHit.barycentrics);
    candidate.rcPrevLobeDiffuse = prevLobeDiffuse;
    candidate.rcWi = 0;
    candidate.rcRadiance = emission;
    candidate.rcJacobianTerms = jacobianTerms;
}

// Same for the dome, where the direction identifies the vertex and there is no hit
void setCandidateRcAtDome(inout PathCandidate candidate, const float3 domeDir, const float3 emission, const float jacobianTerms,
    const bool prevLobeDiffuse)
{
    candidate.rcVertexIdx = candidate.pathLength;
    candidate.rcInstance = 0;
    candidate.rcTriangleIdx = 0;
    candidate.rcBarycentrics = 0;
    candidate.rcPrevLobeDiffuse = prevLobeDiffuse;
    candidate.rcWi = octEncode(domeDir);
    candidate.rcRadiance = emission;
    candidate.rcJacobianTerms = jacobianTerms;
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
        (canPassthrough ? PAYLOAD_FLAG_REFRACTION_PASSTHROUGH : 0) |
        (startUnderwater ? PAYLOAD_FLAG_UNDERWATER : 0);
    payload.pathWeight = float3(1.f, 1.f, 1.f);
    payload.rng = rng;
    payload.waterEntryT = startUnderwater ? 0.f : RAY_DEFAULT_TMAX;
    payload.waterExitT = RAY_DEFAULT_TMAX;
    payload.rayCone = rayCone;
    // TraceRay rather than inline: the reuse passes are SER-sorted by reconnection type and
    // register-bound, and an inline query here measured +10..16% on cave_lights/evil_room
    if (isSegmentOccluded(ray, payload, false))
    {
        return false;
    }

    const float fogTransmittance =
        (applyFog && isFogActive(startUnderwater)) ? computeFogTransmittance(ray.Origin, ray.Direction, segmentDist) : 1.f;
    transmittance = payload.pathWeight * computePassthroughAbsorption(payload, segmentDist) * fogTransmittance;
    return true;
}

// Completes a replayed path at y_{j-1} by connecting to the stored reconnection vertex x_j. Returns
// the product of every factor from y_{j-1}'s scatter onward, to be multiplied by the throughput at
// y_{j-1}, plus the shift's Jacobian (Lin et al. 2026 Eq. 2) and the shifted path's own Jacobian
// terms. Returns zero when the shift is undefined: the offset path would have reconnected earlier,
// or would not reconnect here, so the mapping has no inverse (GRIS Section 7.4). The connecting
// segment is traced the way the original technique traced it: an NEE light vertex gets a shadow ray
// (own rng stream, no fog), anything else the BSDF segment's stream and fog.
float3 evaluateReconnection(const ReplayTarget replay,
                            const RayCone rayCone,
                            const bool isUnderwater,
                            const Material surfMaterial,
                            const float2 uv,
                            const float3 wo_WS,
                            const float3 surfPos_WS,
                            const float3 surfNor_WS,
                            const TexSampleCtx surfTexCtx,
                            const bool canPassthrough,
                            const uint pathSeed,
                            const uint vertexIdx,
                            const uint pathDepth,
                            const float bounceLobeRoughness,
                            const float bounceBsdfPdf,
                            const float3 prevSurfPos_WS,
                            const float3 prevSurfNor_WS,
                            const float footprintThreshold,
                            const bool useRtsl,
                            const bool areaNee,
                            out float jacobian,
                            out float jacobianTerms)
{
    jacobian = 0.f;
    jacobianTerms = 0.f;

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

    // The rc vertex is rebuilt on the current mesh so it follows deforming geometry; a recycled
    // instance id means the stored vertex no longer exists
    const float3 rcWi_WS = octDecode(replay.rcWi);
    const float2 rcBary2 = unpackBarycentrics(replay.rcBarycentrics);
    HitInfo rcHit;
    float3 rcGeoNor_WS;
    bool rcIsBackface = false;
    if (!rcIsDome && !rebuildHit(rcInstanceId(replay.rcInstance), rcInstanceGeneration(replay.rcInstance), replay.rcTriangleIdx,
            rcBary2, surfPos_WS, rcHit, rcGeoNor_WS, rcIsBackface))
    {
        debugZeroReason = 1; return 0.f;
    }
    // A light point sampled by NEE carries its triangle's geometric normal, not the shading normal
    if (rcIsNeeLightVertex)
    {
        rcHit.hitNor_WS = rcGeoNor_WS;
    }
    const float3 wi_WS = rcIsDome ? rcWi_WS : normalize(rcHit.hitPos_WS - surfPos_WS);

    const BsdfEval prevEval = evaluateBsdf(surfMaterial, uv, wo_WS, wi_WS, surfNor_WS, surfTexCtx);
    if (!any(prevEval.value > 0.f))
    {
        debugZeroReason = 2; return 0.f;
    }
    const float3 prevFactor = prevEval.value * absCosTheta(wi_WS, surfNor_WS);

    // The pair ending at this vertex must not qualify, or the offset path would have reconnected earlier.
    // Initial sampling judges the pair by the lobe it samples here, so the offset path's judgement is
    // reproduced by drawing that same sample.
    if (vertexIdx >= 2)
    {
        RandomNumberGenerator bsdfRng = pathRng(pathSeed, vertexIdx, PATH_RNG_BSDF);
        const BsdfSample continuationSample = sampleBsdf(surfMaterial, uv, wo_WS, surfNor_WS, surfTexCtx, bsdfRng);
        if (isReconnectionVertex(bounceLobeRoughness, bounceBsdfPdf, prevSurfPos_WS, prevSurfNor_WS, surfPos_WS, surfNor_WS,
                continuationSample.pdf, continuationSample.wasSpecular, surfMaterial.hasGlossy(), footprintThreshold))
        {
            debugZeroReason = 3 + 16 * min(vertexIdx, 15); return 0.f;
        }
    }

    // The base path's lobe at x_{j-1} must exist here, and its roughness is this surface's
    if (replay.rcPrevLobeDiffuse ? !surfMaterial.hasDiffuse() : !surfMaterial.hasGlossy())
    {
        debugZeroReason = 4; return 0.f;
    }
    const float prevLobeRoughness = replay.rcPrevLobeDiffuse ? 1.f : surfMaterial.roughness;
    // The only material read after the ray, so the material itself is not live across it
    const bool surfAcceptsBacksideLight = surfMaterial.acceptsBacksideLight();

    float3 transmittance;
    if (!traceReconnectionRay(surfPos_WS, surfNor_WS, wi_WS, rcIsDome, rcIsNeeLightVertex, rcHit.hitPos_WS, rcHit.hitNor_WS,
            rayCone, canPassthrough, isUnderwater, !rcIsNeeLightVertex, rayRng, transmittance))
    {
        debugZeroReason = 5; return 0.f;
    }

    if (rcIsLightVertex)
    {
        float lightPdf = 0.f;
        if (rcIsDome)
        {
            lightPdf = rcIsNeeLightVertex ? neeDomeLightPdf() : domeLightPdf(wi_WS, surfNor_WS);
        }
        else if (!areaNee)
        {
            if (rcIsNeeLightVertex)
            {
                debugZeroReason = 21; return 0.f; // no such path is generated here
            }
        }
        else if (useRtsl)
        {
            lightPdf = lightPdfRtsl(rcHit, surfPos_WS, surfNor_WS, wi_WS, surfAcceptsBacksideLight);
        }
        else
        {
            lightPdf = lightPdfUniform(rcHit, surfPos_WS, wi_WS);
        }

        // NEE paths always reconnect to their light; BSDF-sampled light vertices only where the criteria hold
        if (!rcIsNeeLightVertex)
        {
            const bool qualifies = rcIsDome
                ? isDomeReconnectionVertex(prevLobeRoughness)
                : isReconnectionVertex(prevLobeRoughness, prevEval.pdf, surfPos_WS, surfNor_WS, rcHit.hitPos_WS,
                      rcHit.hitNor_WS, 0.f, false, false, footprintThreshold);
            if (!qualifies)
            {
                debugZeroReason = 6; return 0.f;
            }
        }

        const float techniquePdf = rcIsNeeLightVertex ? lightPdf : prevEval.pdf;
        jacobianTerms = techniquePdf * (rcIsDome ? 1.f : reconnectionGeometryTerm(surfPos_WS, rcHit.hitPos_WS, rcHit.hitNor_WS));
        jacobian = (replay.rcJacobianTerms > 0.f) ? jacobianTerms / replay.rcJacobianTerms : 0.f;

        // With their path MIS weights applied, NEE and BSDF sampling of the light vertex both reduce to
        // f * cos * Le / (p_light + p_bsdf)
        return prevFactor * transmittance * replay.rcRadiance / (lightPdf + prevEval.pdf);
    }

    if (prevEval.pdf <= 0.f)
    {
        debugZeroReason = 7; return 0.f;
    }

    const InstanceData rcInstanceData = instanceDatas[rcHit.instanceId];
    const PerTriangleData rcPerTriData = perTriDatas[rcInstanceData.perTriDatasBufferOffset + rcHit.triangleIdx];
    const float rcConeWidth = getRayConeWidthAtDistance(rayCone, distance(surfPos_WS, rcHit.hitPos_WS));
    Material rcMaterial = getHitMaterialAt(rcInstanceData.materialIdx, rcPerTriData.flags, rcHit.uv,
        makeUntintedTexSampleCtx(computeMipLevel(rcConeWidth), rcPerTriData.texArraySliceIdx), rcIsBackface);
    const TexSampleCtx rcTexCtx = makeTintedTexSampleCtx(rcPerTriData, rcConeWidth, rcHit.hitPos_WS);
    rcMaterial.baseColor = getMaterialBaseColor(rcMaterial, rcHit.uv, rcTexCtx).rgb;
    rcMaterial.baseColorTextureId = TEXTURE_ID_INVALID;

    const BsdfEval rcEval = evaluateBsdf(rcMaterial, rcHit.uv, -wi_WS, rcWi_WS, rcHit.hitNor_WS, rcTexCtx);
    const float3 rcFactor = rcEval.value * absCosTheta(rcWi_WS, rcHit.hitNor_WS);

    // The offset path must reconnect here too
    if (!isReconnectionVertex(prevLobeRoughness, prevEval.pdf, surfPos_WS, surfNor_WS, rcHit.hitPos_WS, rcHit.hitNor_WS,
            rcEval.pdf, false, rcMaterial.hasGlossy(), footprintThreshold))
    {
        debugZeroReason = 8; return 0.f;
    }

    // When x_j precedes the light vertex, the final segment's path MIS weight is recomputed here
    const bool rcPrecedesLight = (replay.rcVertexIdx + 1 == replay.pathLength);
    const float rcDenominator = rcPrecedesLight ? (replay.rcLightPdf + rcEval.pdf) : rcEval.pdf;
    if (rcDenominator <= 0.f)
    {
        debugZeroReason = 9; return 0.f;
    }

    const bool finalSegmentIsNee = rcPrecedesLight &&
        (replay.pathTechnique == PATH_TECHNIQUE_NEE_AREA || replay.pathTechnique == PATH_TECHNIQUE_NEE_DOME);
    jacobianTerms = prevEval.pdf * reconnectionGeometryTerm(surfPos_WS, rcHit.hitPos_WS, rcHit.hitNor_WS) *
                    (finalSegmentIsNee ? 1.f : rcEval.pdf);
    jacobian = (replay.rcJacobianTerms > 0.f) ? jacobianTerms / replay.rcJacobianTerms : 0.f;

    return prevFactor / prevEval.pdf * transmittance * rcFactor / rcDenominator * replay.rcRadiance;
}

// Traces one path tree from the primary hit in the gbuffer. In initial sampling mode every complete
// path is fed to the reservoir (ReSTIR PT) or summed into pathColor (other modes), and the terms
// that are not resampled paths (primary emission and miss, fog in-scatter) go straight to pathColor.
// In replay mode only the target path is rebuilt from its seed and its integrand is returned along
// with the shift's Jacobian and the rebuilt path's own Jacobian terms; nothing is accumulated and no
// roulette is applied. Replaying at a pixel other than the path's own is the hybrid shift, and the
// result is zero wherever the shift is undefined.
float3 pathTraceRay(inout Payload payload,
                    inout PathTreeReservoir reservoir,
                    const ReplayTarget replay,
                    const uint2 pixelIdx,
                    const float3 cameraPos_WS,
                    const uint pathSplitIdx,
                    const uint pathSeed,
                    out float3 pathColor,
                    out float3 ptDiffuseAlbedo,
                    out float replayJacobian,
                    out float replayJacobianTerms)
{
    pathColor = 0.f;
    ptDiffuseAlbedo = 0.f;
    replayJacobian = 1.f; // random replay has unit Jacobian
    replayJacobianTerms = 0.f;

    const SamplingMode samplingMode = (SamplingMode)renderParams.samplingMode;
    const bool useRestirPt = (samplingMode == SamplingMode::RESTIR_PT);
    // Debug toggle: debugBool0 swaps RTSL for uniform light picking under ReSTIR, to see what the tree still
    // buys once resampling does the importance sampling
    const bool useRtsl = (samplingMode == SamplingMode::RTSL || useRestirPt) && !(useRestirPt && bool(debugParams.debugBool0));
    const bool doMis = (samplingMode == SamplingMode::MIS || useRtsl || useRestirPt);
    const bool isReplay = replay.active;
    // Inline shadow rays win in initial sampling but not in the replay passes (see isSegmentOccluded);
    // constant per raygen entry, so each pass compiles only one of the two paths
    const bool neeRayQuery = !isReplay;

    RayDesc ray;
    // Same direction as the gbuffer ray, used for calculating wo_WS the first time. Replay derives it
    // from the hit so a path can be rebuilt in the previous frame's camera too.
    ray.Direction = isReplay && bool(payload.flags & PAYLOAD_FLAG_DID_HIT)
        ? normalize(payload.hitInfo.hitPos_WS - cameraPos_WS)
        : getPrimaryRayDirection(pixelIdx);

    float3 throughput = 1.f; // includes the roulette division
    float rrProduct = 1.f;   // roulette survival product, so the stored integrand can exclude it
    float3 rcThroughput = 1.f; // factors applied after the reconnection vertex's scatter (no roulette)

    const float3 primarySegmentAbsorption = computeSegmentAbsorption(payload, cameraPos_WS, ray.Direction);

    // The primary segment is identical for both path splits and collect sums them, so
    // in-scattered radiance is added only by split 0 (same as emission and the dome light miss).
    RandomNumberGenerator primaryFogRng = pathRng(pathSeed, 0, PATH_RNG_FOG);
    applySegmentFog(payload, primaryFogRng, cameraPos_WS, ray.Direction,
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

    // data of last "real" bounce (i.e. not passthrough); the bools share one register
    uint bounceFlags = 0;
    float bounceBsdfPdf = 0.f;
    float bounceLobeRoughness = 0.f;
    float3 surfPos_WS, surfNor_WS;
    // the real bounce before that, for the reconnection test between the two
    float3 prevSurfPos_WS = cameraPos_WS;
    float3 prevSurfNor_WS = 0.f;

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

    Material surfMaterial = getHitMaterial(payload, payload.rayCone.width);

    // Vertex indices follow the papers: x1 is the primary hit, passthrough hits are not vertices
    uint vertexIdx = 1;
    const float footprintThreshold =
        reconnectionFootprintThreshold(cameraPos_WS, payload.hitInfo.hitPos_WS, payload.hitInfo.hitNor_WS);
    RcState rc;
    rc.vertexIdx = 0;
    rc.instance = 0;
    rc.triangleIdx = 0;
    rc.barycentrics = 0;
    rc.prevLobeDiffuse = false;
    rc.wi = 0;
    rc.prevPdfTimesGeom = 0.f;
    rc.pdf = 0.f;

    const uint effectiveMaxPathDepth = renderParams.maxPathDepth;
    for (uint pathDepth = 0; pathDepth < effectiveMaxPathDepth; ++pathDepth)
    {
        const InstanceData instanceData = instanceDatas[payload.hitInfo.instanceId];
        const PerTriangleData perTriData = perTriDatas[instanceData.perTriDatasBufferOffset + payload.hitInfo.triangleIdx];
        const bool hitWasWater = bool(perTriData.flags & TRIANGLE_FLAG_IS_WATER);
        const TexSampleCtx surfTexCtx =
            makeTintedTexSampleCtx(perTriData, payload.rayCone.width, payload.hitInfo.hitPos_WS);

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
        const bool surfAcceptsBacksideLight = surfMaterial.acceptsBacksideLight();

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
                if (doMis && !bool(bounceFlags & BOUNCE_FLAG_WAS_SPECULAR) && bool(bounceFlags & BOUNCE_FLAG_AREA_NEE))
                {
                    lightPdf = useRtsl
                        ? lightPdfRtsl(payload.hitInfo, surfPos_WS, surfNor_WS, ray.Direction,
                              bool(bounceFlags & BOUNCE_FLAG_ACCEPTED_BACKSIDE_LIGHT))
                        : lightPdfUniform(payload.hitInfo, surfPos_WS, ray.Direction);
                    misWeight = balanceHeuristic(bounceBsdfPdf, lightPdf);
                }
                const float3 F = throughput * Le * misWeight;

                const bool lightVertexQualifies = useRestirPt &&
                    isReconnectionVertex(bounceLobeRoughness, bounceBsdfPdf, surfPos_WS, surfNor_WS,
                        payload.hitInfo.hitPos_WS, payload.hitInfo.hitNor_WS, 0.f, false, false, footprintThreshold);
                if (isReplay)
                {
                    if (replay.pathTechnique == PATH_TECHNIQUE_BSDF_EMISSION && vertexIdx == replay.pathLength && replay.rcVertexIdx == 0)
                    {
                        // A path stored without reconnection is undefined where the offset path would reconnect
                        return lightVertexQualifies ? 0.f : F;
                    }
                }
                else
                {
                    PathCandidate candidate = makePathCandidate(F, rrProduct, vertexIdx, PATH_TECHNIQUE_BSDF_EMISSION);
                    if (rc.vertexIdx != 0)
                    {
                        setCandidateRcFromState(candidate, rc, ray.Direction, rcThroughput * Le, rcThroughput * Le * misWeight, lightPdf, false);
                    }
                    else if (lightVertexQualifies)
                    {
                        const float jacobianTerms =
                            bounceBsdfPdf * reconnectionGeometryTerm(surfPos_WS, payload.hitInfo.hitPos_WS, payload.hitInfo.hitNor_WS);
                        setCandidateRcAtLightVertex(candidate, payload.hitInfo, instanceData.generation, Le,
                            jacobianTerms, bool(bounceFlags & BOUNCE_FLAG_SAMPLED_DIFFUSE));
                    }
                    addPathCandidate(reservoir, candidate, useRestirPt, pathSeed, pathSplitIdx, pathColor);
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
        const bool canPassthrough = bool(renderParams.refractionIndirectPassthrough) &&
            (!isDeltaSurface || bool(bounceFlags & BOUNCE_FLAG_ENCOUNTERED_NON_DELTA));
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

        // Replay threads sort by the work their shift does next, which the stored path decides, not
        // the surface: whether random replay continues past this vertex, and whether the rc vertex
        // is a light vertex (pdf walk, shadow ray) or the dome (nothing to rebuild)
        uint coherenceHint;
        if (isReplay)
        {
            const bool rcIsLightVertex = replay.rcVertexIdx == replay.pathLength;
            coherenceHint =
                (vertexIdx + 1 != replay.rcVertexIdx ? (1 << 2) : 0) |
                (rcIsLightVertex ? (1 << 1) : 0) |
                ((rcIsLightVertex && isDomeTechnique(replay.pathTechnique)) ? (1 << 0) : 0);
        }
        else
        {
            coherenceHint =
                (pathDepth == 0 ? (1 << 2) : 0) |
                (isPassthrough ? (1 << 1) : 0) |
                ((!isDeltaSurface && surfMaterial.canScatter()) ? (1 << 0) : 0);
        }
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
            // bounceBsdfPdf, bounceFlags, etc. are intentionally preserved from the last real BSDF sample
        }
        else // !isPassthrough
        {
            if (isReplay && replay.rcVertexIdx != 0 && vertexIdx + 1 == replay.rcVertexIdx)
            {
                return throughput * evaluateReconnection(replay, payload.rayCone, bool(payload.flags & PAYLOAD_FLAG_UNDERWATER),
                    surfMaterial, payload.hitInfo.uv, wo_WS, surfPos_WS, surfNor_WS, surfTexCtx, canPassthrough, pathSeed, vertexIdx,
                    pathDepth, bounceLobeRoughness, bounceBsdfPdf, prevSurfPos_WS, prevSurfNor_WS, footprintThreshold,
                    useRtsl, areaNeeAllowed(bounceFlags), replayJacobian, replayJacobianTerms);
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
            // Decided before this vertex's light samples, since those paths reconnect here too. In replay,
            // a pair qualifying before the stored rc vertex means the offset path would have reconnected
            // earlier, so the shift is undefined.
            if (useRestirPt && vertexIdx >= 2 && (isReplay ? true : rc.vertexIdx == 0) &&
                isReconnectionVertex(bounceLobeRoughness, bounceBsdfPdf, prevSurfPos_WS, prevSurfNor_WS, surfPos_WS, surfNor_WS,
                    surfBsdfSample.pdf, surfBsdfSample.wasSpecular, surfMaterial.hasGlossy(), footprintThreshold))
            {
                if (isReplay)
                {
                    return 0.f;
                }
                rc.vertexIdx = vertexIdx;
                rc.instance = packRcInstance(instanceData.generation, payload.hitInfo.instanceId);
                rc.triangleIdx = payload.hitInfo.triangleIdx;
                rc.barycentrics = packBarycentrics(payload.hitInfo.barycentrics);
                rc.prevLobeDiffuse = bool(bounceFlags & BOUNCE_FLAG_SAMPLED_DIFFUSE);
                rc.wi = octEncode(surfBsdfSample.wi_WS);
                rc.prevPdfTimesGeom = bounceBsdfPdf * reconnectionGeometryTerm(prevSurfPos_WS, surfPos_WS, surfNor_WS);
                rc.pdf = surfBsdfSample.pdf;
            }

            if (doMis && surfMaterial.canScatter() && !isDeltaSurface)
            {
                const bool isUnderwater = payload.flags & PAYLOAD_FLAG_UNDERWATER;
                // Replay only re-samples a light when the target path ends with NEE from this vertex and
                // has no reconnection vertex (NEE paths otherwise always reconnect to their light vertex)
                const bool replayWantsNee = isReplay && replay.rcVertexIdx == 0 && vertexIdx + 1 == replay.pathLength;
                const bool doAreaNee = areaNeeAllowed(bounceFlags) &&
                    (!isReplay || (replayWantsNee && replay.pathTechnique == PATH_TECHNIQUE_NEE_AREA));
                const bool doDomeNee = sceneParams.voxelMode == 1 &&
                    (!isReplay || (replayWantsNee && replay.pathTechnique == PATH_TECHNIQUE_NEE_DOME));

                // Each light sample's BSDF is evaluated before its shadow ray: samples the BSDF rejects
                // trace nothing, and the BSDF value rather than the material is what the ray keeps live.

                // ------------------------------
                // sample area lights
                // ------------------------------

                if (doAreaNee)
                {
                    RandomNumberGenerator neeRng = pathRng(pathSeed, vertexIdx, PATH_RNG_NEE_AREA);
                    AreaLightSample areaSample;
                    if (useRtsl)
                    {
                        areaSample = sampleAreaLightRtsl(surfPos_WS, surfNor_WS, surfAcceptsBacksideLight, neeRng);
                    }
                    else
                    {
                        areaSample = sampleAreaLightUniform(surfPos_WS, neeRng);
                    }
                    BsdfEval areaBsdfEval;
                    if (areaSample.valid)
                    {
                        areaBsdfEval = evaluateBsdf(surfMaterial, payload.hitInfo.uv, wo_WS, areaSample.wi_WS, surfNor_WS, surfTexCtx);
                        areaSample.valid = any(areaBsdfEval.value > 0.f);
                    }

                    DirectLightingSample lightSample;
                    if (areaSample.valid && traceToLight(surfPos_WS, surfNor_WS, areaSample, payload.rayCone, canPassthrough,
                            isUnderwater, pathRng(pathSeed, vertexIdx, PATH_RNG_SHADOW_AREA), neeRayQuery, lightSample))
                    {
                        // no need to consider dome light pdf because dome light sampling can't hit area lights

                        // light pdf in balance heuristic numerator cancels out with divide by pdf
                        const float3 lightFactor = areaBsdfEval.value * absCosTheta(lightSample.wi_WS, surfNor_WS) *
                                                   lightSample.Le * lightSample.transmittance / (lightSample.pdf + areaBsdfEval.pdf);
                        const float3 F = throughput * lightFactor;

                        if (isReplay)
                        {
                            return F;
                        }

                        PathCandidate candidate = makePathCandidate(F, rrProduct, vertexIdx + 1, PATH_TECHNIQUE_NEE_AREA);
                        if (rc.vertexIdx != 0)
                        {
                            setCandidateRcFromState(candidate, rc, lightSample.wi_WS, lightSample.Le * lightSample.transmittance,
                                rcThroughput * lightFactor, lightSample.pdf, true);
                        }
                        else
                        {
                            // Forced light reconnection (Lin et al. 2026, Section 6.2.3): replay never re-samples lights
                            const float jacobianTerms = lightSample.pdf *
                                reconnectionGeometryTerm(surfPos_WS, lightSample.lightHit.hitPos_WS, lightSample.lightHit.hitNor_WS);
                            setCandidateRcAtLightVertex(candidate, lightSample.lightHit,
                                instanceDatas[lightSample.lightHit.instanceId].generation, lightSample.Le, jacobianTerms,
                                surfBsdfSample.sampledDiffuse);
                            candidate.rcLightPdf = lightSample.pdf;
                        }
                        addPathCandidate(reservoir, candidate, useRestirPt, pathSeed, pathSplitIdx, pathColor);
                    }
                }

                // ------------------------------
                // sample dome light
                // ------------------------------

                if (doDomeNee)
                {
                    RandomNumberGenerator domeRng = pathRng(pathSeed, vertexIdx, PATH_RNG_NEE_DOME);
                    float3 domeWi_WS;
                    float domePdf;
                    bool domeSampleValid = sampleDomeLightDir(surfNor_WS, surfAcceptsBacksideLight, domeRng, domeWi_WS, domePdf);
                    BsdfEval domeBsdfEval;
                    if (domeSampleValid)
                    {
                        domeBsdfEval = evaluateBsdf(surfMaterial, payload.hitInfo.uv, wo_WS, domeWi_WS, surfNor_WS, surfTexCtx);
                        domeSampleValid = any(domeBsdfEval.value > 0.f);
                    }

                    float3 domeLe, domeTransmittance;
                    if (domeSampleValid && traceToDomeLight(surfPos_WS, surfNor_WS, domeWi_WS, payload.rayCone, canPassthrough,
                            isUnderwater, pathRng(pathSeed, vertexIdx, PATH_RNG_SHADOW_DOME), neeRayQuery, domeLe, domeTransmittance))
                    {
                        // no need to consider area light pdf because area light sampling can't hit dome light

                        // dome light pdf in balance heuristic numerator cancels out with divide by pdf
                        const float3 lightFactor = domeBsdfEval.value * absCosTheta(domeWi_WS, surfNor_WS) *
                                                   domeLe * domeTransmittance / (domePdf + domeBsdfEval.pdf);
                        const float3 F = throughput * lightFactor;

                        if (isReplay)
                        {
                            return F;
                        }

                        PathCandidate candidate = makePathCandidate(F, rrProduct, vertexIdx + 1, PATH_TECHNIQUE_NEE_DOME);
                        if (rc.vertexIdx != 0)
                        {
                            setCandidateRcFromState(candidate, rc, domeWi_WS, domeLe * domeTransmittance,
                                rcThroughput * lightFactor, domePdf, true);
                        }
                        else
                        {
                            setCandidateRcAtDome(candidate, domeWi_WS, domeLe, domePdf, surfBsdfSample.sampledDiffuse);
                        }
                        addPathCandidate(reservoir, candidate, useRestirPt, pathSeed, pathSplitIdx, pathColor);
                    }
                }
            }

            const bool areaNeeHere = areaNeeAllowed(bounceFlags);
            if (!isDeltaSurface)
            {
                bounceFlags |= BOUNCE_FLAG_ENCOUNTERED_NON_DELTA;
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
            bounceLobeRoughness = surfBsdfSample.lobeRoughness;
            bounceFlags = (bounceFlags & BOUNCE_FLAG_ENCOUNTERED_NON_DELTA) |
                (surfBsdfSample.wasSpecular ? BOUNCE_FLAG_WAS_SPECULAR : 0) |
                (surfAcceptsBacksideLight ? BOUNCE_FLAG_ACCEPTED_BACKSIDE_LIGHT : 0) |
                (surfBsdfSample.sampledDiffuse ? BOUNCE_FLAG_SAMPLED_DIFFUSE : 0) |
                (areaNeeHere ? BOUNCE_FLAG_AREA_NEE : 0);
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
            const float hitDistance = distance(ray.Origin, payload.hitInfo.hitPos_WS);
            payload.rayCone.width = getRayConeWidthAtDistance(payload.rayCone, hitDistance);
            surfMaterial = getHitMaterial(payload, payload.rayCone.width);

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

            if (bool(bounceFlags & BOUNCE_FLAG_WAS_SPECULAR))
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
                            secondHitPerTriData, payload.rayCone.width, payload.hitInfo.hitPos_WS);
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

            // if the bounce was not specular, ptDiffAlbedo remains unchanged
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

            const bool domeQualifies = useRestirPt && isDomeReconnectionVertex(bounceLobeRoughness);
            if (isReplay)
            {
                const bool matches = replay.pathTechnique == PATH_TECHNIQUE_BSDF_DOME && vertexIdx == replay.pathLength && replay.rcVertexIdx == 0;
                return (matches && !domeQualifies) ? F : 0.f;
            }

            PathCandidate candidate = makePathCandidate(F, rrProduct, vertexIdx, PATH_TECHNIQUE_BSDF_DOME);
            if (rc.vertexIdx != 0)
            {
                setCandidateRcFromState(candidate, rc, ray.Direction, rcThroughput * missDomeLightColor,
                    rcThroughput * missDomeLightColor * misWeight, domePdf, false);
            }
            else if (domeQualifies)
            {
                setCandidateRcAtDome(candidate, ray.Direction, missDomeLightColor, bounceBsdfPdf,
                    bool(bounceFlags & BOUNCE_FLAG_SAMPLED_DIFFUSE));
            }
            addPathCandidate(reservoir, candidate, useRestirPt, pathSeed, pathSplitIdx, pathColor);
            break;
        }
        else if (payload.materialIdx == MATERIAL_IDX_INVALID)
        {
            break;
        }

        if (!isReplay && bool(renderParams.doPathSplitting) && pathDepth == 0 && bool(bounceFlags & BOUNCE_FLAG_WAS_SPECULAR)) // TODO: support multiple specular bounces?
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

// Shifts a stored path to `pixelIdx` by replaying it from that pixel's primary hit, as seen from
// `cameraPos_WS` (the previous frame's camera when shifting into the previous frame's domain)
ShiftedPath shiftPathToPixel(const PathReservoir path, const GbufferData gbufferData, const uint2 pixelIdx, const float3 cameraPos_WS)
{
    ShiftedPath shifted;
    shifted.F = 0.f;
    shifted.jacobian = 0.f;
    shifted.rcJacobianTerms = 0.f;
    shifted.pad0 = 0;
    shifted.pad1 = 0;
    shifted.pad2 = 0;
    if (path.W <= 0.f)
    {
        return shifted;
    }

    Payload payload = initPayloadFromGbuffer(gbufferData, cameraPos_WS);
    PathTreeReservoir unusedReservoir = initPathTreeReservoir(initRng(0), 0); // replay never adds candidates
    const uint pathSplitIdx = bool(path.flags & PATH_FLAGS_SPLIT_IDX) ? 1 : 0;
    float3 unusedColor, unusedAlbedo;
    shifted.F = pathTraceRay(payload, unusedReservoir, makeReplayTarget(path), pixelIdx, cameraPos_WS, pathSplitIdx, path.seed,
        unusedColor, unusedAlbedo, shifted.jacobian, shifted.rcJacobianTerms);

    // Extreme Jacobians come from near-specular pdf ratios and are singularities for resampling
    // (GRIS 5.4); rejecting them symmetrically keeps the shift invertible, so this only shrinks its
    // domain and stays unbiased. Non-finite results are treated as undefined shifts too.
    const bool jacobianExtreme = shifted.jacobian < 1e-4f || shifted.jacobian > 1e4f;
    const bool resultNonFinite = any(isnan(shifted.F)) || any(isinf(shifted.F)) || isnan(shifted.jacobian) || isinf(shifted.jacobian);
    if (jacobianExtreme || resultNonFinite)
    {
        debugZeroReason = (debugZeroReason == 0) ? 20 : debugZeroReason;
        debugZeroKind = 1 + getPathTechnique(path.flags) + (getRcVertexIdx(path.flags) == getPathLength(path.flags) ? 4 : 0) + (getRcVertexIdx(path.flags) == 0 ? 8 : 0)
            + ((path.rcJacobianTerms == 0.f) ? 16 : 0);
        shifted.F = 0.f;
        shifted.jacobian = 0.f;
        shifted.rcJacobianTerms = 0.f;
    }
    return shifted;
}

// Initial sampling: one path tree per pixel slot. In ReSTIR PT mode the reservoir is stored and,
// outside the self-replay debug modes, shaded later by the resample pass.
void initialSamplingRayGen()
{
    const uint2 pixelIdx = getPixelIdx();
    const uint pathSplitIdx = getPathSplitIdx();

    const uint linearPixelIdx = pixelIdx.y * renderParams.renderSize.x + pixelIdx.x;
    const uint slotIdx = linearPixelIdx * (bool(renderParams.doPathSplitting) ? 2 : 1) + pathSplitIdx;

    const GbufferData gbufferData = gbufferIn[linearPixelIdx];
    Payload payload = initPayloadFromGbuffer(gbufferData, cameraParams.pos_WS);

    const uint pathSeed = initRng(constantParams.rngSeed, 987654103, slotIdx, renderParams.frameNumber).seed;
    // Separate stream from the path's RNG so resampling draws never perturb the path itself
    PathTreeReservoir reservoir =
        initPathTreeReservoir(initRng(constantParams.rngSeed, 192837465, slotIdx, renderParams.frameNumber), slotIdx);

    float3 pathColor = 0.f;
    float3 outPtDiffuseAlbedo = 0.f;
    float unusedJacobian, unusedJacobianTerms;
    pathTraceRay(payload, reservoir, noReplay(), pixelIdx, cameraParams.pos_WS, pathSplitIdx, pathSeed, pathColor, outPtDiffuseAlbedo,
        unusedJacobian, unusedJacobianTerms);

    if ((SamplingMode)renderParams.samplingMode == SamplingMode::RESTIR_PT)
    {
        // One path tree is one unit of confidence, empty or not
        if (reservoir.hasSelected())
        {
            reservoirsOut[slotIdx].W = reservoir.finalW();
            reservoirsOut[slotIdx].flags = (reservoirsOut[slotIdx].flags & PATH_FLAGS_CONFIDENCE_MASK) |
                (uint(PATH_FLAGS_CONFIDENCE_SCALE) << PATH_FLAGS_CONFIDENCE_SHIFT);
        }
        else
        {
            PathReservoir empty = makeEmptyPathReservoir();
            setReservoirM(empty, 1.f);
            reservoirsOut[slotIdx] = empty;
        }
        const PathReservoir stored = reservoirsOut[slotIdx];

        const RestirDebugMode debugMode = (RestirDebugMode)renderParams.restirDebugMode;
        if (debugMode == RestirDebugMode::SELF_REPLAY || debugMode == RestirDebugMode::SELF_REPLAY_ERROR)
        {
            const ShiftedPath replayed = shiftPathToPixel(stored, gbufferData, pixelIdx, cameraParams.pos_WS);
            if (debugMode == RestirDebugMode::SELF_REPLAY)
            {
                pathColor += replayed.F * stored.W;
            }
            else
            {
#if RESTIR_ATTRIBUTION_TEST
                // TEMP attribution: red = replay returned zero (reason codes), green = relative error > 10%, blue = > 0.1%
                const float relErr = luminance(abs(replayed.F - stored.F)) / max(luminance(stored.F), 1e-6f);
                pathColor = 0.f;
                if (stored.W > 0.f)
                {
                    if (!any(replayed.F > 0.f))
                    {
                        pathColor = float3(255.f, debugZeroReason, debugZeroKind) / 255.f;
                    }
                    else if (relErr > 0.1f)
                    {
                        const uint kind = 1 + getPathTechnique(stored.flags) + (getRcVertexIdx(stored.flags) == getPathLength(stored.flags) ? 4 : 0) +
                            (getRcVertexIdx(stored.flags) == 0 ? 8 : 0) + 16 * min(getRcVertexIdx(stored.flags), 7);
                        pathColor = float3(0.f, kind, clamp(relErr * 100.f, 1.f, 255.f)) / 255.f;
                    }
                    else if (relErr > 0.001f)
                    {
                        pathColor = float3(0.f, 0.f, 1.f);
                    }
                }
#else
                pathColor = (stored.W > 0.f) ? 100.f * abs(replayed.F - stored.F) / max(luminance(stored.F), 1e-6f) : 0.f;
#endif
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

// The two split slots of a pixel sample disjoint parts of path space, so merging them is exact
// resampling with unit MIS weights. Deterministic per pixel and frame so every thread that needs a
// pixel's merged reservoir computes the same one.
PathReservoir mergeSlotReservoirs(const uint2 pixelIdx)
{
    const uint linearPixelIdx = pixelIdx.y * renderParams.renderSize.x + pixelIdx.x;
    if (!bool(renderParams.doPathSplitting))
    {
        return reservoirsOut[linearPixelIdx];
    }

    const PathReservoir slot0 = reservoirsOut[linearPixelIdx * 2];
    const PathReservoir slot1 = reservoirsOut[linearPixelIdx * 2 + 1];
    const float weight0 = slot0.W * luminance(slot0.F);
    const float weight1 = slot1.W * luminance(slot1.F);
    const float weightSum = weight0 + weight1;

    RandomNumberGenerator rng = initRng(constantParams.rngSeed, 555111, linearPixelIdx, renderParams.frameNumber);
    PathReservoir merged = slot1;
    if (rng.nextFloat() * weightSum < weight0)
    {
        merged = slot0;
    }
    const float pHat = luminance(merged.F);
    merged.W = (weightSum > 0.f && pHat > 0.f) ? weightSum / pHat : 0.f;
    setReservoirM(merged, 1.f);
    return merged;
}

// Temporal reuse: the pixel's merged initial reservoir is the canonical sample, its reprojected
// history reservoir (confidence capped) the one neighbor. The history path is shifted into this
// frame's pixel, and this pixel's path into the previous frame's pixel for the canonical MIS term.
// The result is the input to spatial reuse.
// Shift outcome counters for the perf report, see RESTIR_STATS_* in common_structs.h. `path` is
// the reservoir that was shifted (or empty when the pair was skipped). See RESTIR_SHIFT_STATS.
void recordShiftStats(const uint passBase, const bool noPartner, const bool skipped, const PathReservoir path, const ShiftedPath shifted)
{
#if RESTIR_SHIFT_STATS
    if (!bool(restirParams.shiftStatsEnabled))
    {
        return;
    }
    InterlockedAdd(restirStatsOut[passBase + RESTIR_STATS_PAIRS], 1);
    if (noPartner)
    {
        InterlockedAdd(restirStatsOut[passBase + RESTIR_STATS_NO_PARTNER], 1);
        return;
    }
    if (skipped)
    {
        InterlockedAdd(restirStatsOut[passBase + RESTIR_STATS_SKIPPED], 1);
        return;
    }
    // Vertices random replay traces before reconnecting: none when the rc vertex is x2, the whole
    // path (minus the primary hit) when there is no rc vertex
    const uint rcVertexIdx = getRcVertexIdx(path.flags);
    const uint replayVertices = rcVertexIdx == 0 ? getPathLength(path.flags) - 1 : rcVertexIdx - 2;
    const uint bucket = min(replayVertices, RESTIR_STATS_REPLAY_BUCKETS - 1);
    InterlockedAdd(restirStatsOut[passBase + RESTIR_STATS_BUCKETS_BASE + 2 * bucket], 1);
    if (any(shifted.F > 0.f))
    {
        InterlockedAdd(restirStatsOut[passBase + RESTIR_STATS_BUCKETS_BASE + 2 * bucket + 1], 1);
    }
#endif
}

// Stream compaction for random replay (Enhanced, Section 6.2.2): the spatial shift pass does the
// shifts that reconnect straight from the primary hit and appends every pair that needs random
// replay to a list; the spatial replay pass then runs over just that list, so the tail of a few
// replaying pairs per warp stops holding every warp of the shift pass. The list lives in the initial
// reservoir buffer, which nothing reads after the temporal pass (an extra root descriptor on the
// path tracing root signature measurably slows the whole raygen): the counter is reservoir 0's
// flags, entries are the seed fields of the reservoirs after it, and a pair that does not fit is
// shifted in place.
uint replayListCapacity()
{
    const uint pixelCount = renderParams.renderSize.x * renderParams.renderSize.y;
    return pixelCount * (bool(renderParams.doPathSplitting) ? 2 : 1) - 1;
}

void resetReplayList()
{
    reservoirsOut[0].flags = 0;
}

bool appendToReplayList(const uint linearPixelIdx, const uint textureIdx)
{
    uint entryIdx;
    InterlockedAdd(reservoirsOut[0].flags, 1, entryIdx);
    if (entryIdx >= replayListCapacity())
    {
        return false;
    }
    reservoirsOut[1 + entryIdx].seed = linearPixelIdx | (textureIdx << 30);
    return true;
}

// The partner's reservoir under pairing texture `textureIdx`, or an empty one when the partner is
// off screen
bool loadSpatialPartner(const uint textureIdx, const uint2 pixelIdx, out PathReservoir partner)
{
    const bool pairWithSelf = (RestirDebugMode)renderParams.restirDebugMode == RestirDebugMode::SPATIAL_SELF;
    uint2 partnerIdx = pixelIdx;
    const bool hasPartner = pairWithSelf || getPairedPixel(pairingTextures, textureIdx, pixelIdx, partnerIdx);
    partner = makeEmptyPathReservoir();
    if (hasPartner)
    {
        partner = reservoirsMergedOut[partnerIdx.y * renderParams.renderSize.x + partnerIdx.x];
    }
    return hasPartner;
}

void temporalRayGen()
{
    const uint2 pixelIdx = DispatchRaysIndex().xy;
    const uint linearPixelIdx = pixelIdx.y * renderParams.renderSize.x + pixelIdx.x;

    const PathReservoir canonical = mergeSlotReservoirs(pixelIdx);
    const GbufferData gbufferData = gbufferIn[linearPixelIdx];
    // After every thread's own slots are read the initial reservoirs are scratch, see replayListCapacity
    if (all(pixelIdx == 0))
    {
        resetReplayList();
    }

    uint2 prevPixelIdx;
    const bool hasHistory = bool(restirParams.temporalHistoryValid) && restirParams.temporalConfidenceCap > 0.f &&
        bool(gbufferData.payloadFlags & PAYLOAD_FLAG_DID_HIT) && reprojectToPrevPixel(gbufferData.hitInfo, prevPixelIdx);
    if (!hasHistory)
    {
        reservoirsMergedOut[linearPixelIdx] = canonical;
        return;
    }
    // The history pixel is jittered by up to one pixel each way, the DLSS-RR guide's "randomize the
    // temporal reuse step" (Section 3.5): the reused sample then differs between neighbors and
    // frames instead of tracking one pixel's chain. The surface check below still applies to the
    // jittered pixel, and both MIS shifts use it consistently.
    {
        RandomNumberGenerator jitterRng = initRng(constantParams.rngSeed, 271828, linearPixelIdx, renderParams.frameNumber);
        const int2 offset = int2(floor(jitterRng.nextFloat2() * 3.f)) - 1;
        prevPixelIdx = uint2(clamp(int2(prevPixelIdx) + offset, int2(0, 0), int2(renderParams.renderSize) - 1));
    }

    // The previous frame's primary hit is rebuilt on the current mesh (so it follows deforming
    // geometry) and must be the same surface. Everything stored last frame lives in last frame's
    // render space; replay traces the current scene.
    const float3 prevToCurrent = prevFrameToCurrentOffset();
    const uint prevLinearPixelIdx = prevPixelIdx.y * renderParams.renderSize.x + prevPixelIdx.x;
    GbufferData prevGbufferData = gbufferPrevIn[prevLinearPixelIdx];
    const float3 prevCameraPos_WS = cameraParams.prevPos_WS + prevToCurrent;
    HitInfo prevHit;
    float3 prevHitGeoNor_WS;
    bool prevHitIsBackface;
    const bool prevHitValid = bool(prevGbufferData.payloadFlags & PAYLOAD_FLAG_DID_HIT) && prevGbufferData.materialIdx != MATERIAL_IDX_INVALID &&
        rebuildHit(prevGbufferData.hitInfo.instanceId, prevGbufferData.instanceGeneration, prevGbufferData.hitInfo.triangleIdx,
            prevGbufferData.hitInfo.barycentrics, prevCameraPos_WS, prevHit, prevHitGeoNor_WS, prevHitIsBackface);
    if (!prevHitValid || !isSameSurface(prevHit, gbufferData.hitInfo))
    {
        reservoirsMergedOut[linearPixelIdx] = canonical;
        return;
    }
    prevGbufferData.hitInfo = prevHit;
    prevGbufferData.payloadFlags =
        (prevGbufferData.payloadFlags & ~PAYLOAD_FLAG_BACKFACE_HIT) | (prevHitIsBackface ? PAYLOAD_FLAG_BACKFACE_HIT : 0);

    PathReservoir history = reservoirsHistoryIn[prevLinearPixelIdx];
    // Decorrelation (Enhanced, Section 5): where last frame's reservoirs around the history pixel
    // mostly held copies of one sample, trust the history less so the copy stops spreading
    float confidenceCap = restirParams.temporalConfidenceCap;
    if (bool(restirParams.decorrelationEnabled))
    {
        const float duplication = duplicationMapIn[prevLinearPixelIdx];
        confidenceCap = lerp(confidenceCap, restirParams.decorrelationMinCap, pow(duplication, restirParams.decorrelationExponent));
    }
    setReservoirM(history, min(reservoirM(history), confidenceCap));

    const ShiftedPath historyAtCanonical = shiftPathToPixel(history, gbufferData, pixelIdx, cameraParams.pos_WS);
    const ShiftedPath canonicalAtHistory = shiftPathToPixel(canonical, prevGbufferData, prevPixelIdx, prevCameraPos_WS);
    recordShiftStats(RESTIR_STATS_TEMPORAL_BASE, false, history.W <= 0.f, history, historyAtCanonical);
    recordShiftStats(RESTIR_STATS_TEMPORAL_BASE, false, canonical.W <= 0.f, canonical, canonicalAtHistory);

    const float canonicalM = reservoirM(canonical);
    const float historyM = reservoirM(history);
    const float neighborConfidence = historyM;
    const float totalConfidence = canonicalM + neighborConfidence;
    const float canonicalPHat = luminance(canonical.F);

    const float canonicalMis = canonicalM / totalConfidence +
        pairwiseMisCanonicalTerm(canonicalM, historyM, neighborConfidence, totalConfidence, canonicalPHat,
            luminance(canonicalAtHistory.F) * canonicalAtHistory.jacobian);

    PathReservoir selected = canonical;
    float selectedPHat = canonicalPHat;
    float weightSum = canonicalMis * canonicalPHat * canonical.W;

    const float historyPHat = luminance(historyAtCanonical.F);
    if (historyPHat > 0.f && historyAtCanonical.jacobian > 0.f && history.W > 0.f)
    {
        const float pHatFromHistory = luminance(history.F) / historyAtCanonical.jacobian;
        const float mis = pairwiseMisNeighbor(canonicalM, historyM, neighborConfidence, totalConfidence, historyPHat, pHatFromHistory);
        const float weight = mis * historyPHat * history.W * historyAtCanonical.jacobian;
        if (weight > 0.f)
        {
            weightSum += weight;
            RandomNumberGenerator rng = initRng(constantParams.rngSeed, 424242, linearPixelIdx, renderParams.frameNumber);
            if (rng.nextFloat() * weightSum < weight)
            {
                selected = history;
                selected.F = historyAtCanonical.F;
                selected.rcJacobianTerms = historyAtCanonical.rcJacobianTerms;
                selectedPHat = historyPHat;
            }
        }
    }

    selected.W = (weightSum > 0.f && selectedPHat > 0.f) ? weightSum / selectedPHat : 0.f;
    setReservoirM(selected, totalConfidence);
    selected.debugFlags = (historyPHat > 0.f && historyAtCanonical.jacobian > 0.f) ? RESERVOIR_DEBUG_TEMPORAL_SHIFT_SUCCEEDED : 0;
    reservoirsMergedOut[linearPixelIdx] = selected;
}

// Paired spatial reuse, first pass: each pixel shifts every partner's path to itself. The pairing is
// symmetric, so this one evaluation per pair serves both the partner's candidate at this pixel and
// this pixel's MIS term at the partner.
void spatialShiftRayGen()
{
    const uint2 pixelIdx = DispatchRaysIndex().xy;
    const uint linearPixelIdx = pixelIdx.y * renderParams.renderSize.x + pixelIdx.x;
    const uint pixelCount = renderParams.renderSize.x * renderParams.renderSize.y;

    const GbufferData gbufferData = gbufferIn[linearPixelIdx];
    for (uint textureIdx = 0; textureIdx < restirParams.spatialNeighborCount; ++textureIdx)
    {
        PathReservoir partner;
        const bool hasPartner = loadSpatialPartner(textureIdx, pixelIdx, partner);
        const bool needsReplay = partner.W > 0.f && getRcVertexIdx(partner.flags) != 2;
        if (needsReplay && appendToReplayList(linearPixelIdx, textureIdx))
        {
            continue;
        }
        const ShiftedPath shifted = shiftPathToPixel(partner, gbufferData, pixelIdx, cameraParams.pos_WS);
        shiftedOut[textureIdx * pixelCount + linearPixelIdx] = shifted;
        recordShiftStats(RESTIR_STATS_SPATIAL_BASE, !hasPartner, partner.W <= 0.f, partner, shifted);
    }
}

void spatialReplayRayGen()
{
    const uint pixelCount = renderParams.renderSize.x * renderParams.renderSize.y;
    const uint entryCount = min(reservoirsOut[0].flags, replayListCapacity());
    const uint threadCount = DispatchRaysDimensions().x;
    for (uint entryIdx = DispatchRaysIndex().x; entryIdx < entryCount; entryIdx += threadCount)
    {
        const uint entry = reservoirsOut[1 + entryIdx].seed;
        const uint linearPixelIdx = entry & 0x3FFFFFFF;
        const uint textureIdx = entry >> 30;
        const uint2 pixelIdx = uint2(linearPixelIdx % renderParams.renderSize.x, linearPixelIdx / renderParams.renderSize.x);

        PathReservoir partner;
        loadSpatialPartner(textureIdx, pixelIdx, partner);
        const ShiftedPath shifted = shiftPathToPixel(partner, gbufferIn[linearPixelIdx], pixelIdx, cameraParams.pos_WS);
        shiftedOut[textureIdx * pixelCount + linearPixelIdx] = shifted;
        recordShiftStats(RESTIR_STATS_SPATIAL_BASE, false, false, partner, shifted);
    }
}

// One entry point per PtPass (selected by raygen shader record) rather than one switching on a
// root constant: each raygen is compiled and register-allocated on its own, so replay-only code
// and live state do not tax initial sampling
[shader("raygeneration")]
void RayGeneration_InitialSampling()
{
    initialSamplingRayGen();
}

[shader("raygeneration")]
void RayGeneration_Temporal()
{
    temporalRayGen();
}

[shader("raygeneration")]
void RayGeneration_SpatialShift()
{
    spatialShiftRayGen();
}

[shader("raygeneration")]
void RayGeneration_SpatialReplay()
{
    spatialReplayRayGen();
}
