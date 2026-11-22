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
    float3 Albedo;      // 固有基础颜色 (RGB)
    float3 Normal;      // 切线空间法线 (默认 float3(0, 0, 1))
    float3 Emission;    // 自发光颜色 (支持大于 1 的 HDR 辉光)
    float  Metallic;    // 金属度 [0, 1]
    float  Smoothness;  // 光滑度 [0, 1] (Unity 习惯：1=镜面光滑, 0=粗糙)
    float  Roughness;   // 粗糙度 [0, 1] (若直接修改 Roughness 同样自动生效)
    float  Alpha;       // 不透明度 [0, 1]
    float  AlphaCutoff; // Cutout 镂空裁切阈值 (默认 0.5)
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
#endif
    return baseColorMap.SampleLevel(materialSampler, uvLod.xy, uvLod.w);
}
float3 UnpackNormal(float4 packedNormal)
{
    return UnitVector(packedNormal.xyz * 2.0 - 1.0, float3(0.0, 0.0, 1.0));
}

#include "FlareUserSurfaceBody.hlsli"

#endif
