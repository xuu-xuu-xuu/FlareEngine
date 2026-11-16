#ifdef SE_MATERIAL_SURFACE
#include "ShadowDepthSurface.hlsli"
#else
// W03：蒙皮阴影深度。b0 仍是 16 个 root32 常量（clipFromWorld），
// t8 调色板绑定在根参数 1；带 SE_SKINNED 时使用 v3 顶点布局的 JOINTS0/WEIGHTS0。
// 注意：本布局没有 PbrConstants.skin，因此调色板偏移固定为 0（每次绘制一份调色板）。
cbuffer ShadowConstants:register(b0){column_major float4x4 clipFromWorld;};
#ifdef SE_SKINNED
#include "Skinning.hlsli"
#endif
struct VertexInput
{
    float3 position:POSITION;
#ifdef SE_SKINNED
    uint4 joints:JOINTS0;
    float4 weights:WEIGHTS0;
#endif
};
float4 VSMain(VertexInput input):SV_POSITION
{
#ifdef SE_SKINNED
    const float4x4 skinFromSkinned=WeightedSkinMatrix(0,input.joints,input.weights);
    return mul(clipFromWorld,float4(SkinPosition(skinFromSkinned,input.position),1));
#else
    return mul(clipFromWorld,float4(input.position,1));
#endif
}
void PSMain(){}

#endif
