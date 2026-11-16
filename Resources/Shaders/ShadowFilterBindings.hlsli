// G08.02：PCSS 的 blocker / 半影估计 / PCF 三步骤参考 NVIDIA PCSS Integration：
// https://developer.download.nvidia.com/whitepapers/2008/PCSS_Integration.pdf
// 此处为方向面积光：世界半径 = tan(角半径) * 世界光轴 gap，不使用透视光源的除 zB 公式。
cbuffer ShadowFilterConstants:register(b3)
{
    float4 filterParameters; // tan(angle)、搜索世界距离、最大 texel 半径、启用。
    float4 filterOptions; // debug mode、保留。
    float4 filterCascadeMetrics[3]; // 世界深度跨度、世界 texel XY、保留。
};
// 固定中心 + 31 点 Vogel 低差异圆盘；没有逐帧旋转/随机数，便于独立 raw-depth 复核。
static const float2 ShadowFilterDisk[32]={
    float2(0.0000000000,0.0000000000),
    float2(-0.0936459412,0.0857873532),
    float2(0.0192310955,-0.2191284138),
    float2(0.1727850258,0.2253674692),
    float2(-0.3308743192,-0.0585270082),
    float2(0.3214710888,-0.2044935925),
    float2(-0.1093483887,0.4067705554),
    float2(-0.2110518477,-0.4063674900),
    float2(0.4620233807,0.1687301976),
    float2(-0.4840196281,0.1997962663),
    float2(0.2346329709,-0.5013970302),
    float2(0.1741795265,0.5553117773),
    float2(-0.5269752406,-0.3053929235),
    float2(0.6201897364,-0.1363469738),
    float2(-0.3795348220,0.5398492288),
    float2(-0.0878905959,-0.6782456625),
    float2(0.5406884899,0.4556928317),
    float2(-0.7289371807,0.0301438410),
    float2(0.5325739363,-0.5299821992),
    float2(-0.0356834284,0.7716870392),
    float2(-0.5081563788,-0.6089411735),
    float2(0.8059352593,0.1084374493),
    float2(-0.6836071500,0.4756360495),
    float2(0.1869855616,-0.8311695684),
    float2(0.4328799777,0.7554332803),
    float2(-0.8469447259,-0.2701984677),
    float2(0.8233323502,-0.3804004288),
    float2(-0.3569423790,0.8528955668),
    float2(-0.3187740377,-0.8862730319),
    float2(0.8487461282,0.4460771779),
    float2(-0.9432855114,0.2486470335),
    float2(0.5364584133,-0.8343160903)
};
struct ShadowFilterEvaluation
{
    float visibility,penumbraWorld,blockerGapWorld,blockerFraction;
};
ShadowFilterEvaluation EvaluateShadowFilter(uint slice,float3 position,float3 normal,float3 lightDirection)
{
    ShadowFilterEvaluation result;result.visibility=1;result.penumbraWorld=0;result.blockerGapWorld=0;result.blockerFraction=0;
    uint level=min(slice,2u);
    float4 shifted=mul(csmClipFromWorld[level],float4(position+normal*shadowParameters.w,1));
    float4 original=mul(csmClipFromWorld[level],float4(position,1));
    if(shifted.w>0&&original.w>0)
    {
        float3 coordinate=shifted.xyz/shifted.w,unbiased=original.xyz/original.w;
        float2 uv=float2(coordinate.x*.5+.5,.5-coordinate.y*.5);
        float2 unbiasedUv=float2(unbiased.x*.5+.5,.5-unbiased.y*.5);
        if(all(uv>=0)&&all(uv<=1)&&coordinate.z>=0&&coordinate.z<=1)
        {
            float reference=coordinate.z-shadowParameters.y-shadowParameters.z*(1-saturate(dot(normal,lightDirection)));
            float2 gradient=CascadeReceiverGradient(level,normal);
            if(filterParameters.x==0)
            {
                // 零角半径严格使用旧 point 的 texel-center 路径；不执行 blocker/filter 循环，metrics 为零。
                float2 center=(floor(uv/shadowParameters.x)+.5)*shadowParameters.x;
                result.visibility=directionalShadow.SampleCmpLevelZero(shadowSampler,float3(center,level),reference+dot(gradient,center-uv));
            }
            else
            {
            float3 metrics=filterCascadeMetrics[level].xyz;
            uint width,height,layers;directionalShadow.GetDimensions(width,height,layers);
            float2 dimensions=float2(width,height),inverseDimensions=1.0/dimensions;
            float searchWorld=filterParameters.x*filterParameters.y;
            float2 searchTexels=min(searchWorld/metrics.yz,filterParameters.zz);
            float gapSum=0;uint blockers=0;
            [loop]for(uint searchSample=0;searchSample<32;++searchSample)
            {
                int2 texel=(int2)floor(uv*dimensions+ShadowFilterDisk[searchSample]*searchTexels);
                if(all(texel>=0)&&all(texel<int2(width,height)))
                {
                    float2 tapUv=(float2(texel)+.5)*inverseDimensions;
                    float depth=directionalShadow.Load(int4(texel,level,0));
                    float classifiedReference=reference+dot(gradient,tapUv-uv);
                    if(depth<1&&depth<classifiedReference)
                    {
                        // 分类/比较使用偏移与 bias；物理间距来自未偏移接收平面，单位为世界米。
                        float receiverPlane=unbiased.z+dot(gradient,tapUv-unbiasedUv);
                        gapSum+=max(0,receiverPlane-depth)*metrics.x;++blockers;
                    }
                }
            }
            if(blockers>0)
            {
                result.blockerGapWorld=gapSum/blockers;result.blockerFraction=blockers/32.0;
                float requestedRadius=filterParameters.x*result.blockerGapWorld;
                float2 filterTexels=min(requestedRadius/metrics.yz,filterParameters.zz);
                result.penumbraWorld=min(filterTexels.x*metrics.y,filterTexels.y*metrics.z);
                float visibility=0;
                [loop]for(uint filterSample=0;filterSample<32;++filterSample)
                {
                    int2 texel=(int2)floor(uv*dimensions+ShadowFilterDisk[filterSample]*filterTexels);
                    float sampleVisibility=1;
                    if(all(texel>=0)&&all(texel<int2(width,height)))
                    {
                        float2 tapUv=(float2(texel)+.5)*inverseDimensions;
                        sampleVisibility=directionalShadow.SampleCmpLevelZero(shadowSampler,float3(tapUv,level),reference+dot(gradient,tapUv-uv));
                    }
                    visibility+=sampleVisibility;
                }
                result.visibility=visibility/32;
            }
            }
        }
    }
    return result;
}
float3 ShadowFilterDebugColor(float3 position,float3 normal,float3 lightDirection)
{
    float3 value=filterOptions.x<1.5?float3(1,1,1):float3(0,0,0);
    if(filterParameters.w>.5&&framePointCount.y>.5&&framePointCount.z>.5&&options.w>.5)
    {
        float3 selection=CascadeSelectionForPosition(position);
        if(selection.x>=0)
        {
            ShadowFilterEvaluation first=EvaluateShadowFilter((uint)selection.x,position,normal,lightDirection),second=first;
            if(selection.y!=selection.x)second=EvaluateShadowFilter((uint)selection.y,position,normal,lightDirection);
            if(filterOptions.x<1.5)value=lerp(first.visibility,second.visibility,selection.z).xxx;
            else value=lerp(float3(first.penumbraWorld,first.blockerGapWorld,first.blockerFraction),
                float3(second.penumbraWorld,second.blockerGapWorld,second.blockerFraction),selection.z);
        }
    }
    return value;
}
