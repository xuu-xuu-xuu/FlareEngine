#include "PbrBindings.hlsli"
#include "GeometryCoverage.hlsli"
#ifdef SE_MATERIAL_SURFACE
#include "MaterialCoverage.hlsli"
#endif
struct VertexInput {float3 position:POSITION;float3 normal:NORMAL;float2 uv:TEXCOORD0;float4 tangent:TANGENT;};
struct PixelInput
{
    float4 position:SV_POSITION;
    float3 worldPosition:TEXCOORD1;
    float3 normal:NORMAL;
    float2 uv:TEXCOORD0;
    float4 tangent:TANGENT;
};
// 只供双阶段 artifact 的接口反射；真实预绘 PSO 始终使用实际主 Pass 的 VS 字节码。
PixelInput VSMain(VertexInput input)
{
    PixelInput output;
    output.position=mul(clipFromLocal,float4(input.position,1));
    output.worldPosition=mul(worldFromLocal,float4(input.position,1)).xyz;
    output.normal=mul(normalFromLocal,float4(input.normal,0)).xyz;
    output.uv=input.uv;output.tangent=input.tangent;return output;
}
float2 PSMain(PixelInput input,bool frontFace:SV_IsFrontFace):SV_TARGET
{
#ifdef SE_DEPTH_NORMAL_BASIC
    if(!isfinite(input.tangent.w))discard;
    const float4 normalSample=normalMap.Sample(materialSampler,input.uv);
    if(!isfinite(normalSample.x))discard;
#ifndef SE_MATERIAL_SURFACE
    bool logicalFront=frontFace;
#else
    bool logicalFront=frontFace==(parameters.w>0);
#endif
#else
    ApplyLodCoverage(options.w,input.position.xy);
    bool logicalFront=frontFace==(parameters.w>0);
#endif
#ifdef SE_MATERIAL_SURFACE
    MaterialCoverage(input.uv,logicalFront);
#endif
    if(!logicalFront&&options.z<.5)discard;
    float3 n=UnitVector(input.normal,float3(0,0,1));if(!logicalFront)n=-n;
    n/=abs(n.x)+abs(n.y)+abs(n.z);
    float2 encoded=n.xy;
    if(n.z<0)encoded=(1-abs(encoded.yx))*float2(encoded.x<0?-1:1,encoded.y<0?-1:1);
    return encoded;
}
