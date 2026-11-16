// G09：ID 已由完整 uint64 稳定排序；GPU 索引是该输入表中的秩，不截断 ID。
struct ClusterPointLight {float4 positionRange;float4 radiancePad;};
StructuredBuffer<ClusterPointLight> clusterLights:register(t9);
#ifndef SE_CLUSTER_BUILD
StructuredBuffer<uint2> clusterRanges:register(t10);
StructuredBuffer<uint> clusterIndices:register(t11);
StructuredBuffer<uint> clusterCounters:register(t12);
uint PixelCluster(float2 pixel,float3 world)
{
    float depth=max(dot(float4(world,1),csmViewDepth),clusterParameters.x);
    uint x=min(15u,(uint)(pixel.x*clusterViewport.z*16));
    uint y=min(8u,(uint)(pixel.y*clusterViewport.w*9));
    uint z=min(23u,(uint)max(0,floor(log(depth/clusterParameters.x)/log(clusterParameters.y/clusterParameters.x)*24)));
    return x+y*16+z*144;
}
float3 ClusterHeatmap(uint index)
{
    float3 result=float3(1,0,1);
    if(clusterCounters[6]==0)
    {
        float value=clusterRanges[index].y/32.0;
        result=float3(value,.15*(1-value),1-value);
    }
    return result;
}
#endif
