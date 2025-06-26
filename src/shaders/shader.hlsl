#include "rng.hlsli"
#include "common_structs.hlsli"
#include "global_params.hlsli"

struct Payload
{
    float3 color;
    bool allowReflection;
    bool missed;

    RandomSampler rng;
};

struct HitInfo
{
    float3 normal_OS;
    float2 uv;
};

RaytracingAccelerationStructure scene : register(t0);

StructuredBuffer<Vertex> verts : register(t1);
ByteAddressBuffer idxs : register(t2);

StructuredBuffer<InstanceData> instanceDatas : register(t3);
StructuredBuffer<Material> materials : register(t4);

RWTexture2D<float4> renderTarget : register(u0);

static const float3 lightPos_WS = float3(0, 200, 0);
static const float3 skyTopColor = float3(0.24, 0.44, 0.72);
static const float3 skyBottomColor = float3(0.75, 0.86, 0.93);

static const uint numSamplesPerPixel = 1;

float3 calculateRayTarget(const float2 idx, const float2 size)
{
    const float2 uv = idx / size;
    const float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);

    const float aspect = size.x / size.y;
    const float yScale = cameraParams.tanHalfFovY;
    const float xScale = yScale * aspect;

    const float3 target = cameraParams.pos_WS
        + cameraParams.right_WS * ndc.x * xScale
        + cameraParams.up_WS * ndc.y * yScale
        + cameraParams.forward_WS * 1.0;
    return target;
}

[shader("raygeneration")]
void RayGeneration()
{
    const uint2 idx = DispatchRaysIndex().xy;
    const float2 size = DispatchRaysDimensions().xy;

    float3 accumulatedColor = float3(0, 0, 0);
    for (uint i = 0; i < numSamplesPerPixel; ++i)
    {
        Payload payload;
        payload.color = float3(1, 1, 1);
        payload.allowReflection = true;
        payload.missed = false;
        payload.rng = initRandomSampler3(uint3(idx, sceneParams.frameNumber));

        const float2 jitter = float2(payload.rng.nextFloat(), payload.rng.nextFloat());
        const float3 targetPos_WS = calculateRayTarget(idx + jitter, size);

        RayDesc ray;
        ray.Origin = cameraParams.pos_WS;
        ray.Direction = targetPos_WS - cameraParams.pos_WS;
        ray.TMin = 0.001;
        ray.TMax = 1000;

        TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);

        accumulatedColor += payload.color;
    }

    renderTarget[idx] = float4(accumulatedColor / numSamplesPerPixel, 1);
}

[shader("miss")]
void Miss(inout Payload payload)
{
    float slope = normalize(WorldRayDirection()).y;
    float t = saturate(slope * 5 + 0.5);
    payload.color = lerp(skyBottomColor, skyTopColor, t);

    payload.missed = true;
}

void HitCube(inout Payload payload, HitInfo hitInfo);
void HitMirror(inout Payload payload, HitInfo hitInfo);
void HitFloor(inout Payload payload, HitInfo hitInfo);

[shader("closesthit")]
void ClosestHit(inout Payload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    const InstanceData instanceData = instanceDatas[InstanceID()];

    uint i0, i1, i2;
    if (instanceData.hasIdxs)
    {
        const uint idxBufferByteOffset = instanceData.idxBufferByteOffset + PrimitiveIndex() * 3 * 4;
        i0 = idxs.Load(idxBufferByteOffset + 0);
        i1 = idxs.Load(idxBufferByteOffset + 4);
        i2 = idxs.Load(idxBufferByteOffset + 8);
    }
    else
    {
        i0 = PrimitiveIndex() * 3;
        i1 = i0 + 1;
        i2 = i0 + 2;
    }

    Vertex v0 = verts[instanceData.vertBufferOffset + i0];
    Vertex v1 = verts[instanceData.vertBufferOffset + i1];
    Vertex v2 = verts[instanceData.vertBufferOffset + i2];

    const float2 bary2 = attribs.barycentrics;
    const float3 bary = float3(1 - bary2.x - bary2.y, bary2.xy);

    HitInfo hitInfo;
    hitInfo.normal_OS = v0.nor * bary.x + v1.nor * bary.y + v2.nor * bary.z;
    hitInfo.uv = v0.uv * bary.x + v1.uv * bary.y + v2.uv * bary.z;

    if (instanceData.materialId == -1)
    {
        payload.color = 0;
        return;
    }

    const Material material = materials[instanceData.materialId];

    const float diffuseChance = material.diffWeight / (material.diffWeight + material.specWeight);
    if (payload.rng.nextFloat() < diffuseChance)
    {
        payload.color *= material.diffCol / diffuseChance;
    }
    else
    {
        if (!payload.allowReflection)
        {
            payload.color = 0;
            return;
        }

        payload.color *= material.specCol / (1 - diffuseChance);

        float3 hitPos_WS = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
        float3 normal_WS = normalize(mul(hitInfo.normal_OS, (float3x3) ObjectToWorld4x3()));
        float3 reflectedDir_WS = reflect(normalize(WorldRayDirection()), normal_WS);

        RayDesc mirrorRay;
        mirrorRay.Origin = hitPos_WS;
        mirrorRay.Direction = reflectedDir_WS;
        mirrorRay.TMin = 0.001;
        mirrorRay.TMax = 1000;

        payload.allowReflection = false;

        TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, mirrorRay, payload);
    }

//    switch (InstanceID())
//    {
//    case 0:
//        HitMirror(payload, hitInfo);
//        break;
//    case 1:
//        HitFloor(payload, hitInfo);
//        break;
//    default:
//        HitCube(payload, hitInfo);
//        break;
//    }
}

void HitCube(inout Payload payload, HitInfo hitInfo)
{
    const float3 normal_WS = normalize(mul(hitInfo.normal_OS, (float3x3) ObjectToWorld4x3()));

    float3 color = hitInfo.normal_OS;

    if (any(color < 0.f))
    {
        color += 1.f;
    }

    if (any(abs(hitInfo.uv - 0.5) > 0.47))
    {
        color = 0.25.rrr;
    }

    color *= saturate(dot(normal_WS, normalize(lightPos_WS))) + 0.33;
    payload.color = color;
}

void HitMirror(inout Payload payload, HitInfo hitInfo)
{
    if (!payload.allowReflection)
    {
        return;
    }

    float3 hitPos_WS = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
    float3 normal_WS = normalize(mul(hitInfo.normal_OS, (float3x3) ObjectToWorld4x3()));
    float3 reflectedDir_WS = reflect(normalize(WorldRayDirection()), normal_WS);

    RayDesc mirrorRay;
    mirrorRay.Origin = hitPos_WS;
    mirrorRay.Direction = reflectedDir_WS;
    mirrorRay.TMin = 0.001;
    mirrorRay.TMax = 1000;

    payload.allowReflection = false;

    TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, mirrorRay, payload);
}

void HitFloor(inout Payload payload, HitInfo hitInfo)
{
    float3 hitPos_WS = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();

    bool2 pattern = frac(hitPos_WS.xz) > 0.5;
    payload.color = (pattern.x ^ pattern.y ? 0.6 : 0.4).rrr;

    RayDesc shadowRay;
    shadowRay.Origin = hitPos_WS;
    shadowRay.Direction = lightPos_WS - hitPos_WS;
    shadowRay.TMin = 0.001;
    shadowRay.TMax = 1;

    Payload shadow;
    shadow.allowReflection = false;
    shadow.missed = false;

    TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, shadowRay, shadow);

    if (!shadow.missed)
    {
        payload.color /= 2;
    }
}
