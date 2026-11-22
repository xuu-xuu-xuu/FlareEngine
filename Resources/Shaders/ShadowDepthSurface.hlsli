cbuffer ShadowConstants:register(b0){column_major float4x4 clipFromWorld;};
Texture2D<float4> baseColorMap:register(t0);
SamplerState materialSampler:register(s0);
#include "MaterialCoverage.hlsli"
#ifdef SE_USER_SURFACE
#define SE_SHADOW_PASS 1
float3 UnitVector(float3 value,float3 fallback)
{
    float squared=dot(value,value);
    return squared>1e-12&&squared<=3.402823466e38?value*rsqrt(squared):fallback;
}
#include "UserSurfaceContract.hlsli"
#endif
#ifdef SE_SKINNED
#include "Skinning.hlsli"
#endif
struct SurfaceDepthVertex
{
#ifdef SE_VERTEX_COMPRESSED
    float4 position:POSITION;
#else
    float3 position:POSITION;
#endif
    float2 uv:TEXCOORD0;
#ifdef SE_SKINNED
    uint4 joints:JOINTS0;float4 weights:WEIGHTS0;
#endif
};
struct SurfaceDepthPixel
{
    float4 position:SV_POSITION;
    float2 uv:TEXCOORD0;
#ifdef SE_USER_SURFACE
    float3 localPos:TEXCOORD1;
#endif
};
SurfaceDepthPixel VSMain(SurfaceDepthVertex input)
{
    float3 position=input.position.xyz;
#ifdef SE_SKINNED
    position=SkinPosition(WeightedSkinMatrix(0,input.joints,input.weights),position);
#endif
#if defined(SE_USER_SURFACE) && defined(FLARE_HAS_VERTEX)
    FlareVertexInput userV;
    userV.localPos=position;
    userV.localNormal=float3(0.0,1.0,0.0);
    userV.uv=input.uv;
    userV.time=flareTime;
    FlareVertex(userV);
    position=userV.localPos;
#endif
    SurfaceDepthPixel output;
    output.position=mul(clipFromWorld,float4(position,1));
    output.uv=input.uv;
#ifdef SE_USER_SURFACE
    output.localPos=position;
#endif
    return output;
}
void PSMain(SurfaceDepthPixel input,bool frontFace:SV_IsFrontFace)
{
    bool logicalFront=frontFace==(surfaceCoverage.w>0);
#ifdef SE_USER_SURFACE
    if(!logicalFront&&surfaceCoverage.z<.5)discard;
    if(logicalFront&&surfaceCoverage.z>1.5)discard;
    float4 baseSample=baseColorMap.Sample(materialSampler,input.uv);
    if(surfaceCoverage.y>.5&&surfaceCoverage.y<1.5)
    {
        FlareSurfaceInput userIn=(FlareSurfaceInput)0;
        userIn.uv=input.uv;
        userIn.worldPos=input.localPos;
        userIn.localPos=input.localPos;
        userIn.cameraPos=float3(0.0,0.0,5.0);
        userIn.worldNormal=float3(0.0,1.0,0.0);
        userIn.worldTangent=float3(1.0,0.0,0.0);
        userIn.worldBitangent=float3(0.0,0.0,1.0);
        userIn.viewDir=float3(0.0,0.0,1.0);
        userIn.lightDir=float3(0.35,0.85,0.38);
        userIn.lightColor=float3(1.0,1.0,1.0);
        userIn.vertexColor=float4(1.0,1.0,1.0,1.0);
        userIn.screenUv=frac(input.position.xy/1024.0);
        userIn.screenPos=userIn.screenUv;
        userIn.pixelPos=input.position.xy;
        userIn.facing=logicalFront?1.0:-1.0;
        userIn.time=flareTime;
        FlareSurfaceOutput userS=(FlareSurfaceOutput)0;
        userS.baseColor=baseSample.rgb*surfaceBaseColorFactor.rgb;
        userS.metallic=0.0;
        userS.roughness=0.5;
        userS.normalTS=float3(0.0,0.0,1.0);
        userS.emissive=float3(0.0,0.0,0.0);
        userS.opacity=baseSample.a*surfaceBaseColorFactor.a;
        userS.alphaCutoff=surfaceCoverage.x;
        FlareSurface(userIn,userS);
        clip(userS.opacity-userS.alphaCutoff);
    }
#else
    MaterialCoverage(input.uv,logicalFront);
#endif
}
