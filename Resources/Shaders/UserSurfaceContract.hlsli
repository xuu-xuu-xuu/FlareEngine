#ifndef FLARE_ENGINE_USER_SURFACE_CONTRACT
#define FLARE_ENGINE_USER_SURFACE_CONTRACT

struct FlareVertexInput
{
    float3 localPos;
    float3 localNormal;
    float2 uv;
    float4 time; // x=time, y=sin(time), z=cos(time), w=deltaTime
};

struct FlareSurfaceInput
{
    float2 uv;
    float3 worldPos;
    float3 localPos;
    float3 objectPos;
    float3 cameraPos;
    float3 worldNormal;
    float3 worldTangent;
    float3 worldBitangent;
    float3 viewDir;
    float3 lightDir;
    float3 lightColor;
    float4 vertexColor;
    float2 screenUv;
    float2 screenPos;
    float2 pixelPos;
    float  facing; // +1.0 正面, -1.0 背面
    float4 time;   // x=time, y=sin(time), z=cos(time), w=deltaTime
};

float FlareBayer4x4(float2 pixelPos)
{
    uint2 p = uint2(max(pixelPos, 0.0));
    const float bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    return (bayer[(p.y & 3u) * 4u + (p.x & 3u)] + 0.5) / 16.0;
}

struct FlareSurfaceOutput
{
    float3 baseColor;
    float  metallic;
    float  roughness;
    float3 normalTS;
    float3 emissive;
    float  opacity;
    float  alphaCutoff;
    float  occlusion;
    float  specular;
    float  translucency;
};

float4 SampleBaseColor(float2 uv)
{
    return baseColorMap.Sample(materialSampler, uv);
}

float3 SampleNormalTS(float2 uv)
{
#ifdef SE_SHADOW_PASS
    return float3(0.0, 0.0, 1.0);
#elif defined(SE_NORMAL_BC5)
    float2 xy = normalMap.Sample(materialSampler, uv).rg * 2.0 - 1.0;
    return float3(xy, sqrt(saturate(1.0 - dot(xy, xy))));
#else
    return normalMap.Sample(materialSampler, uv).xyz * 2.0 - 1.0;
#endif
}

float2 SampleMetallicRoughness(float2 uv)
{
#ifdef SE_SHADOW_PASS
    return float2(0.0, 0.5);
#else
    return metallicRoughnessMap.Sample(materialSampler, uv).bg;
#endif
}

float SampleOcclusion(float2 uv)
{
#ifdef SE_SHADOW_PASS
    return 1.0;
#else
    return metallicRoughnessMap.Sample(materialSampler, uv).r;
#endif
}

// ==================== Unity 风格简明着色器接口与内置变量 ====================
#define _Time           float4(flareTime.x * 0.05, flareTime.x, flareTime.x * 2.0, flareTime.x * 3.0)
#define _SinTime        float4(sin(flareTime.x * 0.125), sin(flareTime.x * 0.25), sin(flareTime.x * 0.5), flareTime.y)
#define _CosTime        float4(cos(flareTime.x * 0.125), cos(flareTime.x * 0.25), cos(flareTime.x * 0.5), flareTime.z)
#define unity_DeltaTime float4(flareTime.w, 1.0 / max(flareTime.w, 1e-5), flareTime.w, flareTime.w)

struct FlareVertexData
{
    float3 vertex;   // 模型局部顶点坐标
    float3 normal;   // 模型局部顶点法线
    float2 uv;       // 主 UV 坐标
    float2 texcoord; // 主 UV 别名（兼容 Unity 写法）
};
typedef FlareVertexData appdata_full;

struct FlareSurfInput
{
    float2 uv;             // 贴图 UV 坐标
    float2 uv_MainTex;     // 贴图 UV 别名（兼容 Unity Surface Shader 习惯）
    float2 uv_BumpMap;     // 法线贴图 UV 别名
    float3 worldPos;       // 世界空间坐标
    float3 localPos;       // 模型局部坐标
    float3 objectPos;      // 物体枢轴原点世界坐标
    float3 cameraPos;      // 相机世界坐标
    float3 worldNormal;    // 世界空间法线
    float3 worldTangent;   // 世界空间切线
    float3 worldBitangent; // 世界空间副切线
    float3 viewDir;        // 指向相机的单位视线向量
    float3 lightDir;       // 指向主光源的单位方向向量
    float3 lightColor;     // 主光源颜色
    float2 screenUV;       // 屏幕归一化坐标 [0, 1]
    float2 screenPos;      // 屏幕归一化坐标别名
    float2 pixelPos;       // 屏幕物理像素坐标
    float4 color;          // 顶点色
    float  facing;         // 正面 +1.0，背面 -1.0
    float4 time;           // (time, sin(time), cos(time), deltaTime)
};

struct FlareSurfOutput
{
    float3 Albedo;       // 固有基础颜色 (RGB)
    float3 Normal;       // 切线空间法线 (默认 float3(0, 0, 1))
    float3 Emission;     // 自发光颜色 (支持大于 1 的 HDR 辉光)
    float  Metallic;     // 金属度 [0, 1]
    float  Smoothness;   // 光滑度 [0, 1] (Unity 习惯：1=镜面光滑, 0=粗糙)
    float  Roughness;    // 粗糙度 [0, 1] (若直接修改 Roughness 同样自动生效)
    float  Occlusion;    // 烘焙微环境遮蔽 [0, 1] (默认 1.0，兼容 Unity SurfaceOutputStandard)
    float  Specular;     // 电介质镜面反射率 [0, 1] (UE 习惯：默认 0.5 对应 F0=0.04；水体设 0.25 对应 F0=0.02)
    float  Translucency; // 叶片/薄介质背光透射强度 [0, 1] (Shading Foliage 默认 0.45)
    float  Alpha;        // 不透明度 [0, 1]
    float  AlphaCutoff;  // Cutout 镂空裁切阈值 (默认 0.5)
};

static const uint _MainTex          = 0u;
static const uint _BumpMap          = 1u;
static const uint _MetallicGlossMap = 2u;

float4 SampleMainTex(float2 uv) { return SampleBaseColor(uv); }
float4 FlareSampleTex2D(uint slot, float2 uv)
{
    if (slot == 1u) return float4(SampleNormalTS(uv) * 0.5 + 0.5, 1.0);
    if (slot == 2u)
    {
        float2 mr = SampleMetallicRoughness(uv);
        return float4(mr.x, mr.y, 0.0, 1.0 - mr.y);
    }
#if defined(SE_SHADING_EXTENSIONS) && !defined(SE_SHADOW_PASS)
    if (slot == 3u) return userTex0Map.Sample(materialSampler, uv);
    if (slot == 4u) return userTex1Map.Sample(materialSampler, uv);
    if (slot == 5u) return userTex2Map.Sample(materialSampler, uv);
    if (slot == 6u) return userTex3Map.Sample(materialSampler, uv);
#endif
    return SampleBaseColor(uv);
}
float4 FlareSampleTex2DLod(uint slot, float4 uvLod)
{
#ifndef SE_SHADOW_PASS
    if (slot == 1u) return normalMap.SampleLevel(materialSampler, uvLod.xy, uvLod.w);
    if (slot == 2u)
    {
        float2 mr = metallicRoughnessMap.SampleLevel(materialSampler, uvLod.xy, uvLod.w).bg;
        return float4(mr.x, mr.y, 0.0, 1.0 - mr.y);
    }
#ifdef SE_SHADING_EXTENSIONS
    if (slot == 3u) return userTex0Map.SampleLevel(materialSampler, uvLod.xy, uvLod.w);
    if (slot == 4u) return userTex1Map.SampleLevel(materialSampler, uvLod.xy, uvLod.w);
    if (slot == 5u) return userTex2Map.SampleLevel(materialSampler, uvLod.xy, uvLod.w);
    if (slot == 6u) return userTex3Map.SampleLevel(materialSampler, uvLod.xy, uvLod.w);
#endif
#endif
    return baseColorMap.SampleLevel(materialSampler, uvLod.xy, uvLod.w);
}
float4 FlareSampleTex2DGrad(uint slot, float2 uv, float2 ddxUv, float2 ddyUv)
{
#ifndef SE_SHADOW_PASS
    if (slot == 1u)
    {
        float4 ns = normalMap.SampleGrad(materialSampler, uv, ddxUv, ddyUv);
#ifdef SE_NORMAL_BC5
        float2 xy = ns.rg * 2.0 - 1.0;
        return float4(xy * 0.5 + 0.5, sqrt(saturate(1.0 - dot(xy, xy))) * 0.5 + 0.5, 1.0);
#else
        return ns;
#endif
    }
    if (slot == 2u)
    {
        float2 mr = metallicRoughnessMap.SampleGrad(materialSampler, uv, ddxUv, ddyUv).bg;
        return float4(mr.x, mr.y, 0.0, 1.0 - mr.y);
    }
#ifdef SE_SHADING_EXTENSIONS
    if (slot == 3u) return userTex0Map.SampleGrad(materialSampler, uv, ddxUv, ddyUv);
    if (slot == 4u) return userTex1Map.SampleGrad(materialSampler, uv, ddxUv, ddyUv);
    if (slot == 5u) return userTex2Map.SampleGrad(materialSampler, uv, ddxUv, ddyUv);
    if (slot == 6u) return userTex3Map.SampleGrad(materialSampler, uv, ddxUv, ddyUv);
#endif
#endif
    return baseColorMap.SampleGrad(materialSampler, uv, ddxUv, ddyUv);
}
float3 UnpackNormal(float4 packedNormal)
{
    float2 xy = packedNormal.rg * 2.0 - 1.0;
    float3 n = packedNormal.b > 0.004 ? (packedNormal.xyz * 2.0 - 1.0) : float3(xy, sqrt(saturate(1.0 - dot(xy, xy))));
    return UnitVector(n, float3(0.0, 0.0, 1.0));
}

// ==================== 场景深度与不透明背景颜色采样（Unity _CameraDepthTexture / UE SceneColor 对齐） ====================
float FlareSampleSceneDepth(float2 screenUV)
{
#if defined(SE_SHADING_EXTENSIONS) && !defined(SE_SHADOW_PASS) && !defined(SE_DEPTH_NORMALS)
    uint w = 1, h = 1;
    sceneDepthMap.GetDimensions(w, h);
    int2 p = clamp((int2)(saturate(screenUV) * float2(w, h)), int2(0, 0), int2(w - 1, h - 1));
    return sceneDepthMap.Load(int3(p, 0));
#else
    return 1.0;
#endif
}
float FlareLinearEyeDepth(float rawDepth)
{
#if defined(SE_SHADING_EXTENSIONS) && !defined(SE_SHADOW_PASS)
    float p22 = globalSh[0].w, p23 = globalSh[1].w, p32 = globalSh[2].w, p33 = globalSh[3].w;
    float denom = p22 - rawDepth * p32;
    return (globalSh[8].w > 0.5 && abs(denom) > 1e-7) ? max(0.0, (p23 - rawDepth * p33) / denom) : (rawDepth * 100.0);
#else
    return rawDepth * 100.0;
#endif
}
float FlareSampleLinearSceneDepth(float2 screenUV)
{
    return FlareLinearEyeDepth(FlareSampleSceneDepth(screenUV));
}
float FlareSurfaceEyeDepth(FlareSurfInput IN)
{
#if defined(SE_SHADING_EXTENSIONS) && !defined(SE_SHADOW_PASS)
    float4 viewPlane = float4(-globalSh[4].w, -globalSh[5].w, -globalSh[6].w, -globalSh[7].w);
    return (globalSh[8].w > 0.5) ? max(0.0, dot(viewPlane, float4(IN.worldPos, 1.0))) : distance(IN.cameraPos, IN.worldPos);
#else
    return distance(IN.cameraPos, IN.worldPos);
#endif
}
float FlareWaterDepthAt(FlareSurfInput IN, float2 screenUV)
{
    return max(0.0, FlareSampleLinearSceneDepth(screenUV) - FlareSurfaceEyeDepth(IN));
}
float FlareWaterDepth(FlareSurfInput IN)
{
    return FlareWaterDepthAt(IN, IN.screenUV);
}
float3 FlareSampleSceneColor(float2 screenUV)
{
#if defined(SE_SHADING_EXTENSIONS) && !defined(SE_SHADOW_PASS) && !defined(SE_DEPTH_NORMALS)
    return max(ShadingLinear(sceneColorMap, saturate(screenUV)).rgb, 0.0);
#else
    return float3(0.0, 0.0, 0.0);
#endif
}
// 透明表面屏幕空间反射步进（对齐 UE5 SingleLayerWater / Unity HDRP Transparent SSR，950m 远山覆盖 + 顶边平滑衰减）
float FlareTraceScreenReflection(FlareSurfInput IN, float3 worldReflectDir, out float3 reflColor)
{
    reflColor = float3(0.0, 0.0, 0.0);
    float confidence = 0.0;
#if defined(SE_SHADING_EXTENSIONS) && defined(SE_CLUSTERED) && !defined(SE_SHADOW_PASS) && !defined(SE_DEPTH_NORMALS)
    if (globalSh[8].w > 0.5)
    {
        float3 viewPos = mul(clusterView, float4(IN.worldPos, 1.0)).xyz;
        float3 viewDir = UnitVector(mul((float3x3)clusterView, worldReflectDir), float3(0.0, 1.0, 0.0));
        float waterFloorEye = FlareSampleLinearSceneDepth(IN.screenUV);
        float maxDist   = 960.0;
        float prevDelta = -1.0;
        float3 prevRay  = viewPos;
        float2 prevUV   = IN.screenUV;
        float2 exitUV   = IN.screenUV;
        bool exitedScreen = false;
        bool hasValidUV   = false;
        bool hitSurface   = false;
        [loop]
        for (uint s = 1u; s <= 24u; ++s)
        {
            float u = float(s) / 24.0;
            float dist = 2.5 * u + maxDist * (u * u);
            float3 rayView = viewPos + viewDir * dist;
            if (rayView.z >= -0.08) break;
            float4 clipP = mul(clusterProjection, float4(rayView, 1.0));
            if (clipP.w <= 1e-5) break;
            float2 uv = clipP.xy / clipP.w * float2(0.5, -0.5) + 0.5;
            if (any(uv < 0.004) || any(uv > 0.996))
            {
                float2 dUV = uv - prevUV;
                float tx = (dUV.x < -1e-5) ? ((0.004 - prevUV.x) / dUV.x) : ((dUV.x > 1e-5) ? ((0.996 - prevUV.x) / dUV.x) : 1.0);
                float ty = (dUV.y < -1e-5) ? ((0.004 - prevUV.y) / dUV.y) : ((dUV.y > 1e-5) ? ((0.996 - prevUV.y) / dUV.y) : 1.0);
                float tClip = saturate(min(tx, ty));
                exitUV = clamp(prevUV + dUV * tClip, 0.004, 0.996);
                exitedScreen = true;
                break;
            }
            prevUV = uv;
            exitUV = uv;
            hasValidUV = true;

            float rawD = FlareSampleSceneDepth(uv);
            if (rawD >= 0.9998)
            {
                prevDelta = -1.0;
                prevRay = rayView;
                continue;
            }
            float sceneEye  = FlareLinearEyeDepth(rawD);
            if (s <= 2u && abs(sceneEye - waterFloorEye) < 1.6)
            {
                prevRay = rayView;
                continue;
            }
            float rayEye    = -rayView.z;
            float delta     = rayEye - sceneEye;
            float thickness = max(3.0, rayEye * 0.28);
            if (delta >= 0.0 && delta < thickness)
            {
                float fracHit  = saturate(-prevDelta / max(delta - prevDelta, 1e-4));
                float3 midView = lerp(prevRay, rayView, fracHit);
                float4 midClip = mul(clusterProjection, float4(midView, 1.0));
                float2 hitUV   = (midClip.w > 1e-5) ? saturate(midClip.xy / midClip.w * float2(0.5, -0.5) + 0.5) : uv;
                reflColor      = FlareSampleSceneColor(hitUV);
                float edgeX    = smoothstep(0.004, 0.09, hitUV.x) * smoothstep(0.996, 0.91, hitUV.x);
                float edgeY    = smoothstep(0.015, 0.22, hitUV.y) * smoothstep(0.996, 0.90, hitUV.y);
                confidence     = edgeX * edgeY;
                hitSurface     = true;
                break;
            }
            prevDelta = delta;
            prevRay   = rayView;
        }
        if (!hitSurface && (hasValidUV || exitedScreen) && FlareSampleSceneDepth(exitUV) >= 0.9995)
        {
            reflColor   = FlareSampleSceneColor(exitUV);
            float edgeX = smoothstep(0.004, 0.09, exitUV.x) * smoothstep(0.996, 0.91, exitUV.x);
            float edgeY = smoothstep(0.015, 0.22, exitUV.y) * smoothstep(0.996, 0.90, exitUV.y);
            confidence  = edgeX * edgeY * (exitedScreen ? 0.35 : 0.90);
        }
    }
#endif
    return confidence;
}

#include "FlareUserSurfaceBody.hlsli"

#endif
