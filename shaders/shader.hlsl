struct Payload
{
    float3 color;
    bool allowReflection;
    bool missed;
};

uint hash(uint s)
{
    s ^= 2747636419u;
    s *= 2654435769u;
    s ^= s >> 16;
    s *= 2654435769u;
    s ^= s >> 16;
    s *= 2654435769u;
    return s;
}

struct RandomSampler
{
    uint seed;
    
    uint nextUint()
    {
        seed = hash(seed);
        return seed;
    }
    
    float nextFloat()
    {
        return (nextUint() & 0x00FFFFFF) / 16777216.0;
    }
};

RandomSampler initRandomSampler(uint seed)
{
    RandomSampler randomSampler;
    randomSampler.seed = hash(seed);
    return randomSampler;
}

RandomSampler initRandomSampler2(uint2 seed)
{
    return initRandomSampler(seed.x ^ hash(seed.y));
}

RaytracingAccelerationStructure scene : register(t0);

RWTexture2D<float4> uav : register(u0);

static const float fovY = radians(35);

static const float3 cameraPos_WS = float3(0, 1.5, -7);
static const float3 cameraRight_WS = float3(1, 0, 0);
static const float3 cameraUp_WS = float3(0, 1, 0);
static const float3 cameraForward_WS = float3(0, 0, 1);

static const float3 lightPos_WS = float3(0, 200, 0);
static const float3 skyTop = float3(0.24, 0.44, 0.72);
static const float3 skyBottom = float3(0.75, 0.86, 0.93);

static const uint numSamplesPerPixel = 32;

float3 calculateRayTarget(const float2 idx, const float2 size)
{
    const float2 uv = idx / size;
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    
    const float aspect = size.x / size.y;
    const float yScale = tan(fovY * 0.5);
    const float xScale = yScale * aspect;
    
    const float3 target = cameraPos_WS
        + cameraRight_WS * ndc.x * xScale
        + cameraUp_WS * ndc.y * yScale
        + cameraForward_WS * 1.0;
    return target;
}

[shader("raygeneration")]
void RayGeneration()
{
    const uint2 idx = DispatchRaysIndex().xy;
    const float2 size = DispatchRaysDimensions().xy;
    
    RandomSampler rng = initRandomSampler2(idx);

    float3 accumulatedColor = float3(0, 0, 0);
    for (uint i = 0; i < numSamplesPerPixel; ++i)
    {
        const float2 jitter = float2(rng.nextFloat(), rng.nextFloat());
        const float3 targetPos_WS = calculateRayTarget(idx + jitter, size);

        RayDesc ray;
        ray.Origin = cameraPos_WS;
        ray.Direction = targetPos_WS - cameraPos_WS;
        ray.TMin = 0.001;
        ray.TMax = 1000;

        Payload payload;
        payload.allowReflection = true;
        payload.missed = false;
        
        TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);
        accumulatedColor += payload.color;
    }

    uav[idx] = float4(accumulatedColor / numSamplesPerPixel, 1);
}

[shader("miss")]
void Miss(inout Payload payload)
{
    float slope = normalize(WorldRayDirection()).y;
    float t = saturate(slope * 5 + 0.5);
    payload.color = lerp(skyBottom, skyTop, t);

    payload.missed = true;
}

void HitCube(inout Payload payload, float2 uv);
void HitMirror(inout Payload payload, float2 uv);
void HitFloor(inout Payload payload, float2 uv);

[shader("closesthit")]
void ClosestHit(inout Payload payload,
                BuiltInTriangleIntersectionAttributes attribs)
{
    float2 uv = attribs.barycentrics;

    switch (InstanceID())
    {
        case 0:
            HitCube(payload, uv);
            break;
        case 1:
            HitMirror(payload, uv);
            break;
        case 2:
            HitFloor(payload, uv);
            break;
        default:
            payload.color = float3(1, 0, 1);
            break;
    }
}

void HitCube(inout Payload payload, float2 uv)
{
    uint tri = PrimitiveIndex();

    tri /= 2;
    float3 normal = (tri.xxx % 3 == uint3(0, 1, 2)) * (tri < 3 ? -1 : 1);

    float3 worldNormal = normalize(mul(normal, (float3x3) ObjectToWorld4x3()));

    float3 color = abs(normal) / 3 + 0.5;
    if (uv.x < 0.03 || uv.y < 0.03)
    {
        color = 0.25.rrr;
    }

    color *= saturate(dot(worldNormal, normalize(lightPos_WS))) + 0.33;
    payload.color = color;
}

void HitMirror(inout Payload payload, float2 uv)
{
    if (!payload.allowReflection)
    {
        return;
    }

    float3 pos = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
    float3 normal = normalize(mul(float3(0, 1, 0), (float3x3) ObjectToWorld4x3()));
    float3 reflected = reflect(normalize(WorldRayDirection()), normal);

    RayDesc mirrorRay;
    mirrorRay.Origin = pos;
    mirrorRay.Direction = reflected;
    mirrorRay.TMin = 0.001;
    mirrorRay.TMax = 1000;

    payload.allowReflection = false;
    TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, mirrorRay, payload);
}

void HitFloor(inout Payload payload, float2 uv)
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

