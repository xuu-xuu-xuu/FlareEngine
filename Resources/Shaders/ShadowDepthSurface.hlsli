cbuffer ShadowConstants:register(b0){column_major float4x4 clipFromWorld;};
Texture2D<float4> baseColorMap:register(t0);
SamplerState materialSampler:register(s0);
#include "MaterialCoverage.hlsli"
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
struct SurfaceDepthPixel {float4 position:SV_POSITION;float2 uv:TEXCOORD0;};
SurfaceDepthPixel VSMain(SurfaceDepthVertex input)
{
    float3 position=input.position.xyz;
#ifdef SE_SKINNED
    position=SkinPosition(WeightedSkinMatrix(0,input.joints,input.weights),position);
#endif
    SurfaceDepthPixel output;output.position=mul(clipFromWorld,float4(position,1));output.uv=input.uv;return output;
}
void PSMain(SurfaceDepthPixel input,bool frontFace:SV_IsFrontFace)
{MaterialCoverage(input.uv,frontFace==(surfaceCoverage.w>0));}
