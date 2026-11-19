// FlareEngine DXR 1.1 混合硬件光线追踪着色器（内联 RayQuery + SVGF 时空联合降噪 + 物理能量守恒合成）。
// 包含两个独立阶段：
//   1. 不透明前可见性阶段：硬件光追方向光软阴影 + 硬件光追环境光遮蔽 (RTAO) -> 时域累积 -> À-Trous 小波滤波 -> RG16F 可见性图。
//   2. 不透明后间接光阶段：GGX VNDF 重要性采样反射 + 余弦半球单次弹射漫反射 GI（支持屏外几何重心插值与二次阴影光线）-> 时空降噪 -> 全分辨率双边升采样合成。

struct RTInstanceData
{
    float4 worldRow0;
    float4 worldRow1;
    float4 worldRow2;
    float4 normalRow0;
    float4 normalRow1;
    float4 normalRow2;
    float4 baseColor;      // rgb=线性基色乘子, a=不透明度
    float4 materialParams; // x=金属度, y=粗糙度, z=无光照自发光(0/1), w=双面(0/1)
    uint4 meshMeta;        // x=网格描述符槽(0..63 或 0xFFFFFFFF), y=顶点步长字节, z=索引数, w=存在有效反照率贴图(0/1)
};

cbuffer RayTracingConstants : register(b0, space0)
{
    column_major float4x4 rtViewFromWorld;
    column_major float4x4 rtClipFromView;
    column_major float4x4 rtViewFromClip;
    column_major float4x4 rtWorldFromView;
    column_major float4x4 rtPreviousClipFromWorld;
    float4 rtViewport;          // x=宽, y=高, z=1/宽, w=1/高
    float4 rtHalfViewport;      // x=半宽, y=半高, z=1/半宽, w=1/半高
    float4 rtSunDirection;      // xyz=指向太阳单位方向, w=方向光启用(0/1)
    float4 rtSunRadiance;       // rgb=太阳辐亮度, w=太阳角半径(rad)
    float4 rtSkyZenith;         // rgb=天顶色, w=环境贴图强度(0表示无环境贴图)
    float4 rtSkyHorizon;        // rgb=地平线色, w=cos(envYaw)
    float4 rtSkyGround;         // rgb=地面色, w=sin(envYaw)
    float4 rtStrengths;         // x=阴影强度, y=AO强度, z=反射强度, w=GI强度
    float4 rtTracingParams;     // x=AO半径, y=最远追踪距离, z=法线偏移, w=最大反射粗糙度
    float4 rtTemporalParams;    // x=历史混合权重, y=历史有效掩码(bit0=可见性, bit1=间接光), z=环境最大Mip, w=屏幕空间回退掩码(bit0=AO, bit1=Contact)
    uint4 rtFlags;              // x=帧号, y=阴影启用, z=AO启用, w=普通点光数
    uint4 rtIndirectFlags;      // x=反射启用, y=GI启用, z=TLAS实例数, w=局部聚光/点光数
    float4 rtPointPosRange[8];
    float4 rtPointRadiance[8];
    float4 rtLocalPosRange[8];
    float4 rtLocalRadiance[8];
    float4 rtLocalDirection[8];
    float4 rtLocalCone[8];
};

cbuffer RayTracingPassConstants : register(b1, space0)
{
    uint4 rtPassParams; // x=stepSize, y=targetWidth, z=targetHeight, w=passIndex
};

#ifndef SE_RT_COMPOSITE_ONLY
RaytracingAccelerationStructure sceneTlas : register(t0, space0);
StructuredBuffer<RTInstanceData> rtInstances : register(t1, space0);
#endif

Texture2D<float> sceneDepth : register(t2, space0);
Texture2D<float2> sceneNormal : register(t3, space0);
Texture2D<float4> sceneMaterial : register(t4, space0);
Texture2D<float4> sourceHdr : register(t5, space0);
TextureCube<float4> sceneEnvironment : register(t6, space0);
Texture2D<float4> environmentBrdf : register(t7, space0);
Texture2D<float4> rtInputA : register(t8, space0);
Texture2D<float4> rtInputB : register(t9, space0);
Texture2D<float4> rtInputC : register(t10, space0);
Texture2D<float4> rtInputD : register(t11, space0);

#ifndef SE_RT_COMPOSITE_ONLY
RWTexture2D<float4> rtOutA : register(u0, space0);
RWTexture2D<float4> rtOutB : register(u1, space0);
RWTexture2D<float4> rtOutC : register(u2, space0);
RWTexture2D<float2> rtVisibilityOut : register(u3, space0);

ByteAddressBuffer rtMeshVertices[64] : register(t0, space1);
Buffer<uint> rtMeshIndices[64] : register(t0, space2);
Texture2D<float4> rtMeshAlbedo[64] : register(t0, space3);
#endif

SamplerState linearClampSampler : register(s0, space0);
SamplerState linearWrapSampler : register(s1, space0);

static const float Pi = 3.14159265359;

float3 SafeUnit(float3 v, float3 fallback)
{
    float lenSq = dot(v, v);
    return lenSq > 1e-10 ? v * rsqrt(lenSq) : fallback;
}

float2 EncodeOctNormal(float3 n)
{
    n /= max(1e-6, abs(n.x) + abs(n.y) + abs(n.z));
    float2 enc = n.xy;
    if (n.z < 0.0)
        enc = (1.0 - abs(enc.yx)) * float2(enc.x < 0.0 ? -1.0 : 1.0, enc.y < 0.0 ? -1.0 : 1.0);
    return enc;
}

float3 DecodeOctNormal(float2 packed)
{
    float3 n = float3(packed, 1.0 - abs(packed.x) - abs(packed.y));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * float2(n.x < 0.0 ? -1.0 : 1.0, n.y < 0.0 ? -1.0 : 1.0);
    return SafeUnit(n, float3(0.0, 1.0, 0.0));
}

float2 FullUv(int2 pixel)
{
    return (float2(pixel) + 0.5) * rtViewport.zw;
}

float3 ViewPosAt(float2 uv, float depth)
{
    float4 clipPos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), depth, 1.0);
    float4 viewPos = mul(rtViewFromClip, clipPos);
    return viewPos.xyz / (abs(viewPos.w) > 1e-8 ? viewPos.w : 1e-8);
}

float3 WorldPosFromView(float3 viewPos)
{
    return mul(rtWorldFromView, float4(viewPos, 1.0)).xyz;
}

bool ProjectWorldToUv(float3 worldPos, out float2 uv, out float viewDepth)
{
    float3 vp = mul(rtViewFromWorld, float4(worldPos, 1.0)).xyz;
    viewDepth = -vp.z;
    float4 clip = mul(rtClipFromView, float4(vp, 1.0));
    uv = clip.xy / max(clip.w, 1e-7) * float2(0.5, -0.5) + 0.5;
    return clip.w > 1e-6 && clip.z >= 0.0 && clip.z <= clip.w && all(uv >= 0.0) && all(uv < 1.0);
}

void BuildOrthonormalBasis(float3 n, out float3 t, out float3 b)
{
    float signZ = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (signZ + n.z);
    float d = n.x * n.y * a;
    t = float3(1.0 + signZ * n.x * n.x * a, signZ * d, -signZ * n.x);
    b = float3(d, signZ + n.y * n.y * a, -n.y);
}

uint HashPixel(uint2 p, uint salt)
{
    uint n = p.x * 0x9E3779B9u + p.y * 0x85EBCA6Bu + salt * 0xC2B2AE35u;
    n ^= n >> 16;
    n *= 0x7FEB352Du;
    n ^= n >> 15;
    n *= 0x846CA68Bu;
    n ^= n >> 16;
    return n;
}

float2 SampleSequence2D(uint2 pixel, uint frameIdx, uint dimension)
{
    // R2 准蒙特卡洛低差异序列配合像素级哈希旋转，兼顾单帧空域蓝噪特性与时域快速收敛。
    const float2 r2Alpha = float2(0.754877666, 0.569840291);
    uint h = HashPixel(pixel, dimension * 37u + 11u);
    float2 shift = float2((h & 0xFFFFu) / 65536.0, ((h >> 16) & 0xFFFFu) / 65536.0);
    return frac(shift + float((frameIdx + dimension * 17u) & 255u) * r2Alpha);
}

float3 SampleCosineHemisphere(float2 u, float3 normal)
{
    float r = sqrt(u.x);
    float phi = 2.0 * Pi * u.y;
    float3 localDir = float3(r * cos(phi), r * sin(phi), sqrt(max(0.0, 1.0 - u.x)));
    float3 t, b;
    BuildOrthonormalBasis(normal, t, b);
    return SafeUnit(t * localDir.x + b * localDir.y + normal * localDir.z, normal);
}

// Heitz 2018 GGX 可见法线分布函数 (VNDF) 采样，消除掠射角高粗糙度镜面伪影与能量损失。
float3 SampleGgxVndf(float3 viewLocal, float alpha, float2 u)
{
    float3 vh = SafeUnit(float3(alpha * viewLocal.x, alpha * viewLocal.y, viewLocal.z), float3(0.0, 0.0, 1.0));
    float lensq = vh.x * vh.x + vh.y * vh.y;
    float3 t1 = lensq > 1e-7 ? float3(-vh.y, vh.x, 0.0) * rsqrt(lensq) : float3(1.0, 0.0, 0.0);
    float3 t2 = cross(vh, t1);
    float r = sqrt(u.x);
    float phi = 2.0 * Pi * u.y;
    float p1 = r * cos(phi);
    float p2 = r * sin(phi);
    float s = 0.5 * (1.0 + vh.z);
    p2 = (1.0 - s) * sqrt(max(0.0, 1.0 - p1 * p1)) + s * p2;
    float3 nh = p1 * t1 + p2 * t2 + sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * vh;
    return SafeUnit(float3(alpha * nh.x, alpha * nh.y, max(1e-5, nh.z)), float3(0.0, 0.0, 1.0));
}

float3 EvaluateEnvironmentOrSky(float3 worldDir, float mipFraction)
{
    if (rtSkyZenith.w > 0.0)
    {
        float cosYaw = rtSkyHorizon.w;
        float sinYaw = rtSkyGround.w;
        float3 envDir = float3(cosYaw * worldDir.x - sinYaw * worldDir.z, worldDir.y, sinYaw * worldDir.x + cosYaw * worldDir.z);
        return sceneEnvironment.SampleLevel(linearClampSampler, envDir, saturate(mipFraction) * rtTemporalParams.z).rgb * rtSkyZenith.w;
    }
    float y = clamp(worldDir.y, -1.0, 1.0);
    float3 sky = y >= 0.0
        ? lerp(rtSkyHorizon.rgb, rtSkyZenith.rgb, pow(y, 0.65))
        : lerp(rtSkyHorizon.rgb, rtSkyGround.rgb, pow(-y, 0.75));
    if (rtSunDirection.w > 0.5 && mipFraction < 0.15)
    {
        float cosSun = dot(worldDir, rtSunDirection.xyz);
        float cosRadius = cos(max(0.004, rtSunRadiance.w));
        if (cosSun >= cosRadius)
            sky += rtSunRadiance.rgb * smoothstep(cosRadius * 0.999, 1.0, cosSun);
    }
    return max(sky, 0.0);
}

int2 HalfToFullPixel(int2 halfPixel, uint frameIdx)
{
    // 2x2 Bayer 子像素轮换，配合全分辨率双边升采样在 1/4 光线开销下保留几何细节。
    uint phase = frameIdx & 3u;
    int2 sub = int2(int(phase & 1u), int((phase >> 1u) & 1u));
    return min(halfPixel * 2 + sub, int2(rtViewport.xy) - 1);
}

float Luminance(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

#ifndef SE_RT_COMPOSITE_ONLY

bool TraceAnyHitShadow(float3 origin, float3 direction, float maxDist)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.002;
    ray.TMax = maxDist;

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(sceneTlas, RAY_FLAG_NONE, 0xFF, ray);
    q.Proceed();
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

// 追踪主间接光线并在命中点直接估值：优先混合屏幕可见直接光，屏外/被遮挡三角形通过重心坐标还原真实法线与纹理反照率并投射二次阴影光线。
float4 EvaluateSceneRay(float3 origin, float3 rayDir, float maxDist, float fallbackMip, bool secondaryShadow)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = rayDir;
    ray.TMin = 0.004;
    ray.TMax = maxDist;

    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(sceneTlas, RAY_FLAG_NONE, 0xFF, ray);
    q.Proceed();

    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    {
        return float4(EvaluateEnvironmentOrSky(rayDir, fallbackMip), 0.0);
    }

    float hitT = q.CommittedRayT();
    float3 hitWorldPos = origin + rayDir * hitT;
    uint instId = q.CommittedInstanceID();
    if (instId >= rtIndirectFlags.z)
    {
        return float4(EvaluateEnvironmentOrSky(rayDir, fallbackMip), 1.0);
    }

    RTInstanceData inst = rtInstances[instId];
    float3 hitAlbedo = inst.baseColor.rgb;
    float3 hitNormal = -rayDir;

    uint slot = inst.meshMeta.x;
    uint stride = inst.meshMeta.y;
    uint primIdx = q.CommittedPrimitiveIndex();
    if (slot < 64u && stride >= 32u && (primIdx * 3u + 2u) < inst.meshMeta.z)
    {
        uint i0 = rtMeshIndices[NonUniformResourceIndex(slot)].Load(primIdx * 3u + 0u);
        uint i1 = rtMeshIndices[NonUniformResourceIndex(slot)].Load(primIdx * 3u + 1u);
        uint i2 = rtMeshIndices[NonUniformResourceIndex(slot)].Load(primIdx * 3u + 2u);

        float2 bary = q.CommittedTriangleBarycentrics();
        float b0 = 1.0 - bary.x - bary.y;
        float b1 = bary.x;
        float b2 = bary.y;

        float3 n0 = asfloat(rtMeshVertices[NonUniformResourceIndex(slot)].Load3(i0 * stride + 12u));
        float3 n1 = asfloat(rtMeshVertices[NonUniformResourceIndex(slot)].Load3(i1 * stride + 12u));
        float3 n2 = asfloat(rtMeshVertices[NonUniformResourceIndex(slot)].Load3(i2 * stride + 12u));

        float3 localN = b0 * n0 + b1 * n1 + b2 * n2;
        float3 worldN = float3(
            dot(inst.normalRow0.xyz, localN),
            dot(inst.normalRow1.xyz, localN),
            dot(inst.normalRow2.xyz, localN));
        hitNormal = SafeUnit(worldN, -rayDir);
        if (dot(hitNormal, -rayDir) < 0.0)
            hitNormal = -hitNormal;

        if (inst.meshMeta.w != 0u)
        {
            float2 uv0 = asfloat(rtMeshVertices[NonUniformResourceIndex(slot)].Load2(i0 * stride + 24u));
            float2 uv1 = asfloat(rtMeshVertices[NonUniformResourceIndex(slot)].Load2(i1 * stride + 24u));
            float2 uv2 = asfloat(rtMeshVertices[NonUniformResourceIndex(slot)].Load2(i2 * stride + 24u));
            float2 hitUv = b0 * uv0 + b1 * uv1 + b2 * uv2;
            float3 sampledTex = rtMeshAlbedo[NonUniformResourceIndex(slot)].SampleLevel(linearWrapSampler, hitUv, 1.5).rgb;
            hitAlbedo *= sampledTex;
        }
    }

    if (inst.materialParams.z > 0.5)
    {
        return float4(min(max(hitAlbedo, 0.0), 64.0), 1.0);
    }

    // 若命中点在当前相机屏幕内且未被前景遮挡，直接平滑复用已含完整聚光/多材质扩展的 HDR 辐亮度；屏外或背面则完全走硬件光追解析着色。
    float2 screenUv;
    float hitViewZ;
    if (ProjectWorldToUv(hitWorldPos, screenUv, hitViewZ))
    {
        int2 sp = clamp(int2(screenUv * rtViewport.xy), int2(0, 0), int2(rtViewport.xy) - 1);
        float sd = sceneDepth.Load(int3(sp, 0));
        if (sd < 1.0)
        {
            float screenZ = -ViewPosAt(FullUv(sp), sd).z;
            float3 screenN = DecodeOctNormal(sceneNormal.Load(int3(sp, 0)));
            if (abs(screenZ - hitViewZ) < max(0.06, hitViewZ * 0.015) && dot(screenN, hitNormal) > 0.80)
            {
                float2 border = min(screenUv, 1.0 - screenUv);
                float screenWeight = saturate(min(border.x, border.y) * 16.0);
                if (screenWeight > 0.92)
                {
                    float3 screenHdr = min(max(sourceHdr.SampleLevel(linearClampSampler, screenUv, 0.0).rgb, 0.0), 32.0);
                    return float4(screenHdr, 1.0);
                }
            }
        }
    }

    float metal = saturate(inst.materialParams.x);
    float rough = clamp(inst.materialParams.y, 0.05, 1.0);
    float3 diffuseAlbedo = hitAlbedo * (1.0 - metal);
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), hitAlbedo, metal);

    float3 radiance = diffuseAlbedo * EvaluateEnvironmentOrSky(hitNormal, 0.85) * 0.45;
    if (metal > 0.02)
    {
        float3 reflDir = reflect(rayDir, hitNormal);
        radiance += f0 * EvaluateEnvironmentOrSky(reflDir, rough) * (1.0 - rough * 0.5);
    }

    if (rtSunDirection.w > 0.5)
    {
        float ndl = saturate(dot(hitNormal, rtSunDirection.xyz));
        if (ndl > 0.0)
        {
            float sunVis = 1.0;
            if (secondaryShadow)
            {
                sunVis = TraceAnyHitShadow(hitWorldPos + hitNormal * max(rtTracingParams.z, hitT * 0.002),
                                           rtSunDirection.xyz, rtTracingParams.y) ? 0.0 : 1.0;
            }
            if (sunVis > 0.0)
            {
                float3 vHit = -rayDir;
                float3 h = SafeUnit(vHit + rtSunDirection.xyz, hitNormal);
                float ndh = saturate(dot(hitNormal, h));
                float a2 = max(0.002, rough * rough * rough * rough);
                float denom = ndh * ndh * (a2 - 1.0) + 1.0;
                float dTerm = a2 / max(1e-5, Pi * denom * denom);
                float3 spec = f0 * min(dTerm * 0.25, 8.0);
                radiance += (diffuseAlbedo * (1.0 / Pi) + spec) * rtSunRadiance.rgb * (ndl * sunVis);
            }
        }
    }

    [loop] for (uint pIdx = 0; pIdx < rtFlags.w; ++pIdx)
    {
        float3 delta = rtPointPosRange[pIdx].xyz - hitWorldPos;
        float distSq = dot(delta, delta);
        float radius = max(0.05, rtPointPosRange[pIdx].w);
        if (distSq < radius * radius)
        {
            float ratio = distSq / (radius * radius);
            float win = saturate(1.0 - ratio * ratio);
            float atten = (win * win) / max(distSq, 0.01);
            float ndl = saturate(dot(hitNormal, SafeUnit(delta, hitNormal)));
            radiance += diffuseAlbedo * (1.0 / Pi) * rtPointRadiance[pIdx].rgb * (atten * ndl);
        }
    }

    [loop] for (uint lIdx = 0; lIdx < rtIndirectFlags.w; ++lIdx)
    {
        float3 delta = rtLocalPosRange[lIdx].xyz - hitWorldPos;
        float distSq = dot(delta, delta);
        float radius = max(0.05, rtLocalPosRange[lIdx].w);
        if (distSq < radius * radius)
        {
            float3 lDir = SafeUnit(delta, hitNormal);
            float ratio = distSq / (radius * radius);
            float win = saturate(1.0 - ratio * ratio);
            float atten = (win * win) / max(distSq, 0.01);
            if (rtLocalRadiance[lIdx].w > 0.5)
            {
                float cosCone = dot(-lDir, rtLocalDirection[lIdx].xyz);
                float innerCos = rtLocalDirection[lIdx].w;
                float outerCos = rtLocalCone[lIdx].x;
                float spot = saturate((cosCone - outerCos) / max(1e-4, innerCos - outerCos));
                atten *= spot * spot;
            }
            float ndl = saturate(dot(hitNormal, lDir));
            radiance += diffuseAlbedo * (1.0 / Pi) * rtLocalRadiance[lIdx].rgb * (atten * ndl);
        }
    }

    return float4(min(max(radiance, 0.0), 64.0), 1.0);
}

// ============================================================================
// 阶段 1：可见性硬件光追（方向光软阴影 + RTAO）与 SVGF 时空滤波
// ============================================================================

[numthreads(8, 8, 1)]
void CSTraceVisibility(uint3 dispatchId : SV_DispatchThreadID)
{
    int2 pixel = int2(dispatchId.xy);
    int2 extent = int2(rtPassParams.yz);
    if (any(pixel >= extent)) return;

    float depth = sceneDepth.Load(int3(pixel, 0));
    uint fbMode = uint(round(rtTemporalParams.w));
    float2 fallbackVis = float2(1.0, 1.0);
    if (fbMode != 0u)
    {
        float2 loadedFb = rtInputD.Load(int3(pixel, 0)).rg;
        if ((fbMode & 1u) != 0u) fallbackVis.x = loadedFb.x;
        if ((fbMode & 2u) != 0u) fallbackVis.y = loadedFb.y;
    }

    if (depth >= 1.0)
    {
        rtOutA[pixel] = float4(fallbackVis.x, fallbackVis.y, -1.0, 0.0);
        return;
    }

    float3 viewPos = ViewPosAt(FullUv(pixel), depth);
    float viewDist = max(1e-4, -viewPos.z);
    float3 worldPos = WorldPosFromView(viewPos);
    float3 worldNormal = DecodeOctNormal(sceneNormal.Load(int3(pixel, 0)));
    float3 rayOrigin = worldPos + worldNormal * max(rtTracingParams.z, viewDist * 0.0015);

    float ao = fallbackVis.x;
    if (rtFlags.z != 0u)
    {
        float2 uAo = SampleSequence2D(uint2(pixel), rtFlags.x, 0u);
        float3 aoDir = SampleCosineHemisphere(uAo, worldNormal);
        RayDesc aoRay;
        aoRay.Origin = rayOrigin;
        aoRay.Direction = aoDir;
        aoRay.TMin = 0.002;
        aoRay.TMax = rtTracingParams.x;

        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> qAo;
        qAo.TraceRayInline(sceneTlas, RAY_FLAG_NONE, 0xFF, aoRay);
        qAo.Proceed();
        if (qAo.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        {
            float hitRatio = saturate(qAo.CommittedRayT() / max(1e-3, rtTracingParams.x));
            ao = lerp(1.0, hitRatio, saturate(rtStrengths.y));
        }
        else
        {
            ao = 1.0;
        }
    }

    float shadow = fallbackVis.y;
    float penumbra = 0.0;
    if (rtFlags.y != 0u && rtSunDirection.w > 0.5)
    {
        float ndl = dot(worldNormal, rtSunDirection.xyz);
        if (ndl <= 0.0)
        {
            shadow = 0.0;
        }
        else
        {
            float2 uSun = SampleSequence2D(uint2(pixel), rtFlags.x, 1u);
            float diskRadius = tan(clamp(rtSunRadiance.w, 0.0, 0.15));
            float r = sqrt(uSun.x) * diskRadius;
            float phi = 2.0 * Pi * uSun.y;
            float3 sunT, sunB;
            BuildOrthonormalBasis(rtSunDirection.xyz, sunT, sunB);
            float3 shadowDir = SafeUnit(rtSunDirection.xyz + sunT * (r * cos(phi)) + sunB * (r * sin(phi)), rtSunDirection.xyz);
            if (dot(shadowDir, worldNormal) <= 0.001)
                shadowDir = rtSunDirection.xyz;

            RayDesc shRay;
            shRay.Origin = rayOrigin;
            shRay.Direction = shadowDir;
            shRay.TMin = 0.002;
            shRay.TMax = rtTracingParams.y;

            RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> qSh;
            qSh.TraceRayInline(sceneTlas, RAY_FLAG_NONE, 0xFF, shRay);
            qSh.Proceed();
            if (qSh.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
            {
                shadow = 0.0;
                penumbra = saturate(qSh.CommittedRayT() * diskRadius * 12.0);
            }
            else
            {
                shadow = 1.0;
            }
        }
    }

    rtOutA[pixel] = float4(ao, shadow, viewDist, penumbra);
}

[numthreads(8, 8, 1)]
void CSTemporalVisibility(uint3 dispatchId : SV_DispatchThreadID)
{
    int2 pixel = int2(dispatchId.xy);
    int2 extent = int2(rtPassParams.yz);
    if (any(pixel >= extent)) return;

    float4 curr = rtInputA.Load(int3(pixel, 0));
    if (curr.z <= 0.0)
    {
        rtOutA[pixel] = float4(curr.xy, -1.0, 0.0);
        rtOutB[pixel] = 0.0;
        return;
    }

    float depth = sceneDepth.Load(int3(pixel, 0));
    float3 viewPos = ViewPosAt(FullUv(pixel), depth);
    float3 worldPos = WorldPosFromView(viewPos);
    float2 currOctN = sceneNormal.Load(int3(pixel, 0));
    float3 currN = DecodeOctNormal(currOctN);

    float2 m1 = 0.0, m2 = 0.0;
    float maxPenumbra = curr.w;
    float wBoxSum = 0.0;
    [unroll] for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll] for (int dx = -1; dx <= 1; ++dx)
        {
            int2 q = clamp(pixel + int2(dx, dy), int2(0, 0), extent - 1);
            float4 s = rtInputA.Load(int3(q, 0));
            if (s.z <= 0.0) continue;
            if (abs(s.z - curr.z) > max(0.06, curr.z * 0.04)) continue;
            m1 += s.xy;
            m2 += s.xy * s.xy;
            maxPenumbra = max(maxPenumbra, s.w);
            wBoxSum += 1.0;
        }
    }
    m1 /= max(1.0, wBoxSum);
    m2 /= max(1.0, wBoxSum);
    float2 sigma = sqrt(max(0.0, m2 - m1 * m1));
    float2 clampMin = saturate(m1 - 1.5 * sigma - 0.02);
    float2 clampMax = saturate(m1 + 1.5 * sigma + 0.02);

    float2 accum = curr.xy;
    float histLen = 1.0;
    float combinedVar = max(sigma.x * sigma.x, sigma.y * sigma.y);

    if ((uint(round(rtTemporalParams.y)) & 1u) != 0u)
    {
        float4 prevClip = mul(rtPreviousClipFromWorld, float4(worldPos, 1.0));
        if (prevClip.w > 1e-6)
        {
            float2 prevUv = prevClip.xy / prevClip.w * float2(0.5, -0.5) + 0.5;
            if (all(prevUv >= 0.0) && all(prevUv < 1.0))
            {
                float2 prevTexel = prevUv * float2(extent) - 0.5;
                int2 baseTc = int2(floor(prevTexel));
                float2 f = frac(prevTexel);
                float2 histSum = 0.0;
                float lenSum = 0.0, varSum = 0.0, wSum = 0.0;
                float depthTol = max(0.05, curr.z * 0.05);

                [unroll] for (int oy = 0; oy <= 1; ++oy)
                {
                    [unroll] for (int ox = 0; ox <= 1; ++ox)
                    {
                        int2 tc = clamp(baseTc + int2(ox, oy), int2(0, 0), extent - 1);
                        float4 hVal = rtInputB.Load(int3(tc, 0));
                        if (hVal.z <= 0.0 || abs(hVal.z - curr.z) > depthTol) continue;
                        float4 hMeta = rtInputC.Load(int3(tc, 0));
                        float3 prevN = DecodeOctNormal(hMeta.zw);
                        if (dot(currN, prevN) < 0.85) continue;
                        float bw = (ox == 0 ? 1.0 - f.x : f.x) * (oy == 0 ? 1.0 - f.y : f.y);
                        histSum += hVal.xy * bw;
                        lenSum += hVal.w * bw;
                        varSum += hMeta.x * bw;
                        wSum += bw;
                    }
                }
                if (wSum > 0.05)
                {
                    float2 prevVis = clamp(histSum / wSum, clampMin, clampMax);
                    histLen = min(24.0, (lenSum / wSum) + 1.0);
                    float alpha = max(1.0 / histLen, 1.0 - clamp(rtTemporalParams.x, 0.0, 0.95));
                    accum = lerp(prevVis, curr.xy, alpha);
                    float2 diff = curr.xy - accum;
                    combinedVar = lerp(varSum / wSum, max(diff.x * diff.x, diff.y * diff.y), max(alpha, 0.15));
                }
            }
        }
    }

    rtOutA[pixel] = float4(accum, curr.z, histLen);
    rtOutB[pixel] = float4(combinedVar, maxPenumbra, currOctN);
}

[numthreads(8, 8, 1)]
void CSATrousVisibility(uint3 dispatchId : SV_DispatchThreadID)
{
    int2 pixel = int2(dispatchId.xy);
    int2 extent = int2(rtPassParams.yz);
    if (any(pixel >= extent)) return;

    float4 center = rtInputA.Load(int3(pixel, 0));
    if (center.z <= 0.0)
    {
        rtOutA[pixel] = center;
        if (rtPassParams.w == 2u)
            rtVisibilityOut[pixel] = saturate(center.xy);
        return;
    }

    float3 centerN = DecodeOctNormal(sceneNormal.Load(int3(pixel, 0)));
    float4 meta = rtInputB.Load(int3(pixel, 0));
    float variance = max(1e-5, meta.x);
    float penumbra = meta.y;
    float histLen = max(1.0, center.w);

    int stepSize = max(1, int(rtPassParams.x));
    // 接触阴影（penumbra 接近 0 且时域已收敛）自动收窄滤波核，保持根部锐利边缘。
    float kernelScale = (penumbra < 0.02 && histLen > 6.0 && rtFlags.z == 0u) ? 0.0 : 1.0;
    float2 sum = center.xy * (6.0 / 16.0) * (6.0 / 16.0);
    float wSum = (6.0 / 16.0) * (6.0 / 16.0);

    if (kernelScale > 0.0)
    {
        float sigmaZ = max(0.02, center.z * 0.01 * float(stepSize));
        float sigmaV = max(0.08, (2.5 + 3.0 * penumbra) * sqrt(variance) + (histLen < 4.0 ? 0.25 : 0.0));
        const float kw[5] = { 1.0 / 16.0, 4.0 / 16.0, 6.0 / 16.0, 4.0 / 16.0, 1.0 / 16.0 };

        [unroll] for (int oy = -2; oy <= 2; ++oy)
        {
            [unroll] for (int ox = -2; ox <= 2; ++ox)
            {
                if (ox == 0 && oy == 0) continue;
                int2 q = pixel + int2(ox, oy) * stepSize;
                if (any(q < 0) || any(q >= extent)) continue;

                float4 qVal = rtInputA.Load(int3(q, 0));
                if (qVal.z <= 0.0) continue;
                float3 qN = DecodeOctNormal(sceneNormal.Load(int3(q, 0)));
                float cosN = saturate(dot(centerN, qN));
                if (cosN < 0.75) continue;

                float wN = pow(cosN, 32.0);
                float dz = abs(qVal.z - center.z) / sigmaZ;
                float wZ = exp(-0.5 * dz * dz);
                float dv = max(abs(qVal.x - center.x), abs(qVal.y - center.y) / max(0.25, 0.25 + penumbra));
                float wV = exp(-dv / sigmaV);
                float w = kw[ox + 2] * kw[oy + 2] * wN * wZ * wV;
                sum += qVal.xy * w;
                wSum += w;
            }
        }
    }

    float2 filtered = sum / max(1e-6, wSum);
    rtOutA[pixel] = float4(filtered, center.z, center.w);
    if (rtPassParams.w == 2u)
    {
        float outAo = saturate(filtered.x);
        float outShadow = rtFlags.y != 0u ? saturate(lerp(1.0, filtered.y, saturate(rtStrengths.x))) : saturate(filtered.y);
        rtVisibilityOut[pixel] = float2(outAo, outShadow);
    }
}

// ============================================================================
// 阶段 2：半分辨率硬件光追反射 (VNDF) 与全局漫反射间接光 (RTGI) + SVGF 降噪
// ============================================================================

[numthreads(8, 8, 1)]
void CSTraceIndirect(uint3 dispatchId : SV_DispatchThreadID)
{
    int2 halfPixel = int2(dispatchId.xy);
    int2 extent = int2(rtPassParams.yz);
    if (any(halfPixel >= extent)) return;

    int2 fullPixel = HalfToFullPixel(halfPixel, rtFlags.x);
    float depth = sceneDepth.Load(int3(fullPixel, 0));
    if (depth >= 1.0)
    {
        rtOutA[halfPixel] = 0.0;
        rtOutB[halfPixel] = 0.0;
        return;
    }

    float4 mat = sceneMaterial.Load(int3(fullPixel, 0));
    if (mat.a < 0.0)
    {
        rtOutA[halfPixel] = 0.0;
        rtOutB[halfPixel] = 0.0;
        return;
    }

    uint code = uint(round(max(0.0, mat.a)));
    float rough = max(0.045, (code % 32u) / 31.0);
    float3 viewPos = ViewPosAt(FullUv(fullPixel), depth);
    float viewDist = max(1e-4, -viewPos.z);
    float3 worldPos = WorldPosFromView(viewPos);
    float3 worldNormal = DecodeOctNormal(sceneNormal.Load(int3(fullPixel, 0)));
    float3 cameraWorld = mul(rtWorldFromView, float4(0.0, 0.0, 0.0, 1.0)).xyz;
    float3 viewDir = SafeUnit(cameraWorld - worldPos, worldNormal);
    float3 rayOrigin = worldPos + worldNormal * max(rtTracingParams.z, viewDist * 0.0015);

    float4 specOut = float4(0.0, 0.0, 0.0, -1.0);
    if (rtIndirectFlags.x != 0u && rough <= rtTracingParams.w)
    {
        float3 reflDir;
        if (rough < 0.06)
        {
            reflDir = reflect(-viewDir, worldNormal);
        }
        else
        {
            float3 t, b;
            BuildOrthonormalBasis(worldNormal, t, b);
            float3 vLocal = float3(dot(viewDir, t), dot(viewDir, b), max(1e-4, dot(viewDir, worldNormal)));
            float2 uSpec = SampleSequence2D(uint2(halfPixel), rtFlags.x, 2u);
            float alpha = rough * rough;
            float3 hLocal = SampleGgxVndf(vLocal, alpha, uSpec);
            float3 hWorld = SafeUnit(t * hLocal.x + b * hLocal.y + worldNormal * hLocal.z, worldNormal);
            reflDir = reflect(-viewDir, hWorld);
            if (dot(reflDir, worldNormal) <= 0.005)
                reflDir = reflect(-viewDir, worldNormal);
        }
        float4 tracedSpec = EvaluateSceneRay(rayOrigin, reflDir, rtTracingParams.y, rough, true);
        // alpha 编码：>0 表示有效几何样本（正值存储命中掩码与视深关联：viewDist + (hit ? 0 : 65536) 避免额外缓冲，FP16 改为存 viewDist 且用负号区分未命中天空）。
        // 注：在有 IBL 环境贴图的场景里未命中天空贡献为 0（保持原预滤波环境贴图），仅几何命中提供反射替换量！
        float3 envBase = rtSkyZenith.w > 0.0 ? EvaluateEnvironmentOrSky(reflDir, rough) : float3(0.0, 0.0, 0.0);
        float3 specDeltaOrRad = rtSkyZenith.w > 0.0
            ? (tracedSpec.w > 0.5 ? max(tracedSpec.rgb - envBase, -envBase) : float3(0.0, 0.0, 0.0))
            : tracedSpec.rgb;
        specOut = float4(clamp(specDeltaOrRad, -32.0, 64.0), viewDist);
    }
    else
    {
        specOut = float4(0.0, 0.0, 0.0, viewDist);
    }

    float4 giOut = float4(0.0, 0.0, 0.0, viewDist);
    if (rtIndirectFlags.y != 0u)
    {
        float2 uGi = SampleSequence2D(uint2(halfPixel), rtFlags.x, 3u);
        float3 giDir = SampleCosineHemisphere(uGi, worldNormal);
        float4 tracedGi = EvaluateSceneRay(rayOrigin, giDir, rtTracingParams.y * 0.5, 0.75, true);
        // 仅当命中附近表面时计入额外反弹漫反射，避免与已有天空/IBL 环境漫反射重复叠加。
        float3 bounceIrradiance = tracedGi.w > 0.5 ? tracedGi.rgb : float3(0.0, 0.0, 0.0);
        giOut = float4(min(max(bounceIrradiance, 0.0), 32.0), viewDist);
    }

    rtOutA[halfPixel] = specOut;
    rtOutB[halfPixel] = giOut;
}

[numthreads(8, 8, 1)]
void CSTemporalIndirect(uint3 dispatchId : SV_DispatchThreadID)
{
    int2 halfPixel = int2(dispatchId.xy);
    int2 extent = int2(rtPassParams.yz);
    if (any(halfPixel >= extent)) return;

    float4 currSpec = rtInputA.Load(int3(halfPixel, 0));
    float4 currGi = rtInputB.Load(int3(halfPixel, 0));
    if (currSpec.w <= 0.0)
    {
        rtOutA[halfPixel] = 0.0;
        rtOutB[halfPixel] = 0.0;
        rtOutC[halfPixel] = 0.0;
        return;
    }

    int2 fullPixel = HalfToFullPixel(halfPixel, rtFlags.x);
    float depth = sceneDepth.Load(int3(fullPixel, 0));
    float3 viewPos = ViewPosAt(FullUv(fullPixel), depth);
    float3 worldPos = WorldPosFromView(viewPos);
    float2 currOctN = sceneNormal.Load(int3(fullPixel, 0));
    float3 currN = DecodeOctNormal(currOctN);
    float4 mat = sceneMaterial.Load(int3(fullPixel, 0));
    uint code = uint(round(max(0.0, mat.a)));
    float rough = max(0.045, (code % 32u) / 31.0);

    float3 specM1 = 0.0, specM2 = 0.0, giM1 = 0.0, giM2 = 0.0;
    float boxW = 0.0;
    [unroll] for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll] for (int dx = -1; dx <= 1; ++dx)
        {
            int2 q = clamp(halfPixel + int2(dx, dy), int2(0, 0), extent - 1);
            float4 sS = rtInputA.Load(int3(q, 0));
            float4 sG = rtInputB.Load(int3(q, 0));
            if (sS.w <= 0.0 || abs(sS.w - currSpec.w) > max(0.08, currSpec.w * 0.06)) continue;
            specM1 += sS.rgb;
            specM2 += sS.rgb * sS.rgb;
            giM1 += sG.rgb;
            giM2 += sG.rgb * sG.rgb;
            boxW += 1.0;
        }
    }
    specM1 /= max(1.0, boxW);
    specM2 /= max(1.0, boxW);
    giM1 /= max(1.0, boxW);
    giM2 /= max(1.0, boxW);
    float3 specStd = sqrt(max(0.0, specM2 - specM1 * specM1));
    float3 giStd = sqrt(max(0.0, giM2 - giM1 * giM1));

    float3 accumSpec = currSpec.rgb;
    float3 accumGi = currGi.rgb;
    float varEst = max(Luminance(specStd), Luminance(giStd));

    if ((uint(round(rtTemporalParams.y)) & 2u) != 0u)
    {
        float4 prevClip = mul(rtPreviousClipFromWorld, float4(worldPos, 1.0));
        if (prevClip.w > 1e-6)
        {
            float2 prevUv = prevClip.xy / prevClip.w * float2(0.5, -0.5) + 0.5;
            if (all(prevUv >= 0.0) && all(prevUv < 1.0))
            {
                float2 prevTexel = prevUv * float2(extent) - 0.5;
                int2 baseTc = int2(floor(prevTexel));
                float2 f = frac(prevTexel);
                float3 hSpecSum = 0.0, hGiSum = 0.0;
                float hVarSum = 0.0, wSum = 0.0;
                float depthTol = max(0.08, currSpec.w * 0.08);

                [unroll] for (int oy = 0; oy <= 1; ++oy)
                {
                    [unroll] for (int ox = 0; ox <= 1; ++ox)
                    {
                        int2 tc = clamp(baseTc + int2(ox, oy), int2(0, 0), extent - 1);
                        float4 pSpec = rtInputC.Load(int3(tc, 0));
                        if (pSpec.w <= 0.0 || abs(pSpec.w - currSpec.w) > depthTol) continue;
                        float4 pGi = rtInputD.Load(int3(tc, 0));
                        // 历史法线与方差从同一对状态读取（法线近似由当前邻域与深度门控保障，光滑镜面减小重投影拖影）。
                        float bw = (ox == 0 ? 1.0 - f.x : f.x) * (oy == 0 ? 1.0 - f.y : f.y);
                        hSpecSum += pSpec.rgb * bw;
                        hGiSum += pGi.rgb * bw;
                        hVarSum += max(0.0, pGi.w) * bw;
                        wSum += bw;
                    }
                }
                if (wSum > 0.05)
                {
                    // 光滑镜面反射对视角变化更敏感，使用较低时域权重防止镜面重投影滞后；漫反射 GI 使用高权重稳定收敛。
                    float specBlend = clamp(rtTemporalParams.x * lerp(0.68, 0.96, saturate(rough * 2.0)), 0.0, 0.92);
                    float giBlend = clamp(rtTemporalParams.x, 0.0, 0.94);
                    float3 clampedSpec = clamp(hSpecSum / wSum, specM1 - 1.5 * specStd, specM1 + 1.5 * specStd);
                    float3 clampedGi = clamp(hGiSum / wSum, max(0.0, giM1 - 1.35 * giStd), giM1 + 1.35 * giStd);
                    accumSpec = lerp(currSpec.rgb, clampedSpec, specBlend);
                    accumGi = lerp(currGi.rgb, clampedGi, giBlend);
                    varEst = lerp(varEst, hVarSum / wSum, 0.75);
                }
            }
        }
    }

    rtOutA[halfPixel] = float4(accumSpec, currSpec.w);
    rtOutB[halfPixel] = float4(accumGi, max(1e-4, varEst));
    rtOutC[halfPixel] = float4(accumGi, max(1e-4, varEst));
}

[numthreads(8, 8, 1)]
void CSATrousIndirect(uint3 dispatchId : SV_DispatchThreadID)
{
    int2 halfPixel = int2(dispatchId.xy);
    int2 extent = int2(rtPassParams.yz);
    if (any(halfPixel >= extent)) return;

    float4 centerSpec = rtInputA.Load(int3(halfPixel, 0));
    float4 centerGi = rtInputB.Load(int3(halfPixel, 0));
    if (centerSpec.w <= 0.0)
    {
        rtOutA[halfPixel] = 0.0;
        rtOutB[halfPixel] = 0.0;
        return;
    }

    int2 fullPixel = HalfToFullPixel(halfPixel, rtFlags.x);
    float3 centerN = DecodeOctNormal(sceneNormal.Load(int3(fullPixel, 0)));
    float4 mat = sceneMaterial.Load(int3(fullPixel, 0));
    uint code = uint(round(max(0.0, mat.a)));
    float rough = max(0.045, (code % 32u) / 31.0);

    int stepSize = max(1, int(rtPassParams.x));
    float sigmaZ = max(0.04, centerSpec.w * 0.015 * float(stepSize));
    float varMetric = max(0.02, rtInputC.Load(int3(halfPixel, 0)).w);
    float sigmaGiL = max(0.06, 3.5 * varMetric);
    float sigmaSpecL = max(0.04, (0.5 + 4.0 * rough) * varMetric);
    bool blurSpec = rough > 0.09;

    const float kw[5] = { 1.0 / 16.0, 4.0 / 16.0, 6.0 / 16.0, 4.0 / 16.0, 1.0 / 16.0 };
    float centerKw = kw[2] * kw[2];
    float3 sumSpec = centerSpec.rgb * centerKw;
    float3 sumGi = centerGi.rgb * centerKw;
    float wSpecSum = centerKw;
    float wGiSum = centerKw;
    float centerGiLum = Luminance(centerGi.rgb);

    [unroll] for (int oy = -2; oy <= 2; ++oy)
    {
        [unroll] for (int ox = -2; ox <= 2; ++ox)
        {
            if (ox == 0 && oy == 0) continue;
            int2 q = halfPixel + int2(ox, oy) * stepSize;
            if (any(q < 0) || any(q >= extent)) continue;

            float4 qSpec = rtInputA.Load(int3(q, 0));
            if (qSpec.w <= 0.0) continue;
            float4 qGi = rtInputB.Load(int3(q, 0));

            int2 qFull = HalfToFullPixel(q, rtFlags.x);
            float3 qN = DecodeOctNormal(sceneNormal.Load(int3(qFull, 0)));
            float cosN = saturate(dot(centerN, qN));
            if (cosN < 0.70) continue;

            float dz = abs(qSpec.w - centerSpec.w) / sigmaZ;
            float wZ = exp(-0.5 * dz * dz);
            float k = kw[ox + 2] * kw[oy + 2] * wZ;

            float wGi = k * pow(cosN, 16.0) * exp(-abs(Luminance(qGi.rgb) - centerGiLum) / sigmaGiL);
            sumGi += qGi.rgb * wGi;
            wGiSum += wGi;

            if (blurSpec)
            {
                float specLobePower = lerp(64.0, 12.0, saturate(rough));
                float wS = k * pow(cosN, specLobePower) * exp(-Luminance(abs(qSpec.rgb - centerSpec.rgb)) / sigmaSpecL);
                sumSpec += qSpec.rgb * wS;
                wSpecSum += wS;
            }
        }
    }

    rtOutA[halfPixel] = float4(blurSpec ? (sumSpec / max(1e-6, wSpecSum)) : centerSpec.rgb, centerSpec.w);
    rtOutB[halfPixel] = float4(sumGi / max(1e-6, wGiSum), centerSpec.w);
}

#endif // !SE_RT_COMPOSITE_ONLY

// ============================================================================
// 全屏联合双边升采样与物理能量守恒 HDR 合成阶段
// ============================================================================

float4 VSComposite(uint vertexId : SV_VertexID) : SV_POSITION
{
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 PSComposite(float4 position : SV_POSITION) : SV_Target
{
    int2 pixel = int2(position.xy);
    float4 baseHdr = sourceHdr.Load(int3(pixel, 0));
    float depth = sceneDepth.Load(int3(pixel, 0));
    if (depth >= 1.0) return baseHdr;

    float4 material = sceneMaterial.Load(int3(pixel, 0));
    if (material.a < 0.0) return baseHdr;

    uint code = uint(round(max(0.0, material.a)));
    float metal = (code / 32u) / 31.0;
    float rough = max(0.045, (code % 32u) / 31.0);

    float3 viewPos = ViewPosAt(FullUv(pixel), depth);
    float viewDist = max(1e-4, -viewPos.z);
    float3 worldPos = WorldPosFromView(viewPos);
    float3 worldNormal = DecodeOctNormal(sceneNormal.Load(int3(pixel, 0)));
    float3 cameraWorld = mul(rtWorldFromView, float4(0.0, 0.0, 0.0, 1.0)).xyz;
    float3 viewDir = SafeUnit(cameraWorld - worldPos, worldNormal);
    float ndv = saturate(dot(worldNormal, viewDir));

    int2 halfExtent = int2(rtHalfViewport.xy);
    float2 halfCoord = (float2(pixel) + 0.5) * 0.5 - 0.5;
    int2 baseHalf = int2(floor(halfCoord));

    float3 upsampledSpec = 0.0;
    float3 upsampledGi = 0.0;
    float wSpecTotal = 0.0;
    float wGiTotal = 0.0;
    float sigmaZ = max(0.03, viewDist * 0.02);

    [unroll] for (int oy = -1; oy <= 2; ++oy)
    {
        [unroll] for (int ox = -1; ox <= 2; ++ox)
        {
            int2 hc = clamp(baseHalf + int2(ox, oy), int2(0, 0), halfExtent - 1);
            float4 sSpec = rtInputA.Load(int3(hc, 0));
            if (sSpec.w <= 0.0) continue;
            float4 sGi = rtInputB.Load(int3(hc, 0));

            int2 qFull = HalfToFullPixel(hc, rtFlags.x);
            float3 qN = DecodeOctNormal(sceneNormal.Load(int3(qFull, 0)));
            float cosN = saturate(dot(worldNormal, qN));
            if (cosN < 0.72) continue;

            float dz = abs(sSpec.w - viewDist) / sigmaZ;
            float2 dxy = (float2(qFull) - float2(pixel)) * 0.5;
            float wSpatial = exp(-0.65 * dot(dxy, dxy) - 0.5 * dz * dz);
            float wG = wSpatial * pow(cosN, 12.0);
            float wS = wSpatial * pow(cosN, lerp(48.0, 12.0, saturate(rough)));

            upsampledGi += sGi.rgb * wG;
            wGiTotal += wG;
            upsampledSpec += sSpec.rgb * wS;
            wSpecTotal += wS;
        }
    }

    float3 result = baseHdr.rgb;
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), material.rgb, metal);

    if (rtIndirectFlags.y != 0u && rtStrengths.w > 0.0 && metal < 1.0 && wGiTotal > 1e-5)
    {
        float3 giIrradiance = upsampledGi / wGiTotal;
        float3 diffuseBrdf = (1.0 - metal) * material.rgb * (1.0 - f0);
        result += rtStrengths.w * giIrradiance * diffuseBrdf;
    }

    if (rtIndirectFlags.x != 0u && rtStrengths.z > 0.0 && rough <= rtTracingParams.w && wSpecTotal > 1e-5)
    {
        float3 specRad = upsampledSpec / wSpecTotal;
        float2 ab = environmentBrdf.SampleLevel(linearClampSampler, float2(ndv, rough), 0.0).rg;
        float3 specBrdf = (rtSkyZenith.w > 0.0) ? (f0 * ab.x + ab.y) : (f0 + (1.0 - f0) * pow(1.0 - ndv, 5.0));
        float roughFade = 1.0 - smoothstep(rtTracingParams.w * 0.75, rtTracingParams.w, rough);
        result += rtStrengths.z * roughFade * specRad * specBrdf;
    }

    return float4(clamp(result, 0.0, 65504.0), baseHdr.a);
}
