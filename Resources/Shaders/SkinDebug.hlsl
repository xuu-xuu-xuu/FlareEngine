// W03 诊断/测试专用前向变体：把蒙皮后的实体局部位置写进 HDR 目标。
// SkinnedRenderGpuTests 用它回读 GPU 顶点变换结果，并与 CPU 参考矩阵比较精度；
// 不参与任何正式渲染管线，也不需要美术资产。
#include "PbrBindings.hlsli"
#include "ForwardBindings.hlsli"
#include "Skinning.hlsli"
struct VertexInput
{
    float3 position:POSITION;
    float3 normal:NORMAL;
    float2 uv:TEXCOORD0;
    float4 tangent:TANGENT;
    uint4 joints:JOINTS0;
    float4 weights:WEIGHTS0;
};
struct PixelInput
{
    float4 position:SV_POSITION;
    float2 uv:TEXCOORD0;
    float3 localPosition:TEXCOORD1;
};
PixelInput VSMain(VertexInput input)
{
    PixelInput output;
    const float4x4 skinFromSkinned=WeightedSkinMatrix((uint)max(0.0,skin.x),input.joints,input.weights);
    const float3 local=SkinPosition(skinFromSkinned,input.position);
    output.position=mul(clipFromLocal,float4(local,1));
    output.uv=input.uv;
    output.localPosition=local;
    return output;
}
float4 PSMain(PixelInput input):SV_TARGET
{
    // 布局校验要求 b0/b1 的全部字段与三张贴图、采样器都真实参与反射：
    // 这些项只进入 alpha 级别的极小加权，不改变 rgb 的位置读数。
    const float4 material=baseColor+cameraPosition+directionToLight+radiance+parameters+options+look+skin
        +worldFromLocal[0]+normalFromLocal[0];
    const float4 sampled=baseColorMap.Sample(materialSampler,input.uv)+normalMap.Sample(materialSampler,input.uv)
        +metallicRoughnessMap.Sample(materialSampler,input.uv);
    float4 frame=frameDirection+frameRadiance+frameSkyExposure+framePointCount;
    [unroll] for(uint index=0;index<8;++index)frame+=pointPositionRadius[index]+pointRadiance[index];
    const float keep=1e-12*abs(material.x+material.y+sampled.x+sampled.y+frame.x+frame.y);
    return float4(input.localPosition+keep,1);
}
