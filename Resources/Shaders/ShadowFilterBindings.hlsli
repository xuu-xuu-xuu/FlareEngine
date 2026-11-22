// G08.02：PCSS（Blocker Search / 半影估计 / 硬件双线性 PCF）+ Jorge Jimenez IGN 逐像素圆盘旋转。
// 方向面积光世界半径 = tan(角半径) * 世界光轴遮挡间距，配合最小抗锯齿核下限消除接触区阶梯锯齿。
cbuffer ShadowFilterConstants:register(b3)
{
    float4 filterParameters; // tan(angle)、搜索世界距离、最大 texel 半径、启用。
    float4 filterOptions; // debug mode、保留。
    float4 filterCascadeMetrics[3]; // 世界深度跨度、世界 texel XY、保留。
};
// 16 点黄金角 Vogel 低差异圆盘，配合逐像素 IGN 旋转与硬件 2x2 双线性比较采样（等效覆盖 64 纹素）。
static const float2 ShadowFilterVogel16[16]={
    float2( 0.17677670, 0.00000000),
    float2(-0.22577961, 0.20681717),
    float2( 0.03459771,-0.39376771),
    float2( 0.28454071, 0.37119614),
    float2(-0.52220426,-0.09248085),
    float2( 0.49481024,-0.31450410),
    float2(-0.16652690, 0.61523882),
    float2(-0.31543738,-0.60765882),
    float2( 0.68448041, 0.25047271),
    float2(-0.71250380, 0.29340815),
    float2( 0.34377219,-0.73353301),
    float2( 0.25361872, 0.80896700),
    float2(-0.76284738,-0.44644579),
    float2( 0.89703185,-0.19769648),
    float2(-0.54686116, 0.77922580),
    float2(-0.12640504,-0.97610028)
};
struct ShadowFilterEvaluation
{
    float visibility,penumbraWorld,blockerGapWorld,blockerFraction;
};
ShadowFilterEvaluation EvaluateShadowFilter(uint slice,float3 position,float3 normal,float3 lightDirection,float2 screenPixel)
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
                // 零角半径严格保持点采样 texel-center 回退路径；不执行 blocker/filter 循环，metrics 为零。
                float2 center=(floor(uv/shadowParameters.x)+.5)*shadowParameters.x;
                result.visibility=directionalShadow.SampleCmpLevelZero(shadowSampler,float3(center,level),reference+dot(gradient,center-uv));
            }
            else
            {
                float3 metrics=filterCascadeMetrics[level].xyz;
                uint width,height,layers;directionalShadow.GetDimensions(width,height,layers);
                float2 dimensions=float2(width,height),inverseDimensions=1.0/dimensions;
                float2 maxGrad=0.0015/max(inverseDimensions,1e-6);
                float2 clampedGradient=clamp(gradient,-maxGrad,maxGrad);
                float bilinearGuard=0.5*dot(abs(clampedGradient),inverseDimensions);

                float ign=frac(52.9829189*frac(dot(floor(screenPixel),float2(0.06711056,0.00583715))));
                float rotSin,rotCos;
                sincos(ign*6.2831853,rotSin,rotCos);

                float searchWorld=filterParameters.x*filterParameters.y;
                float2 minSearch=min(float2(1.5,1.5),filterParameters.zz);
                float2 searchTexels=clamp(searchWorld/metrics.yz,minSearch,filterParameters.zz);
                float gapSum=0;uint blockers=0;
                [loop]for(uint searchSample=0;searchSample<16;++searchSample)
                {
                    float2 d=ShadowFilterVogel16[searchSample];
                    float2 rot=float2(d.x*rotCos-d.y*rotSin,d.x*rotSin+d.y*rotCos);
                    int2 texel=(int2)floor(uv*dimensions+rot*searchTexels);
                    if(all(texel>=0)&&all(texel<int2(width,height)))
                    {
                        float2 tapUv=(float2(texel)+.5)*inverseDimensions;
                        float depth=directionalShadow.Load(int4(texel,level,0));
                        float classifiedReference=reference+dot(clampedGradient,tapUv-uv);
                        if(depth<1&&depth<classifiedReference)
                        {
                            float receiverPlane=unbiased.z+dot(clampedGradient,tapUv-unbiasedUv);
                            gapSum+=max(0,receiverPlane-depth)*metrics.x;++blockers;
                        }
                    }
                }
                if(blockers>0)
                {
                    result.blockerGapWorld=gapSum/blockers;result.blockerFraction=blockers/16.0;
                    float requestedRadius=filterParameters.x*result.blockerGapWorld;
                    float2 minFilter=min(float2(1.35,1.35),filterParameters.zz);
                    float2 filterTexels=clamp(requestedRadius/metrics.yz,minFilter,filterParameters.zz);
                    result.penumbraWorld=min(filterTexels.x*metrics.y,filterTexels.y*metrics.z);
                    float2 filterRadiusUv=filterTexels*inverseDimensions;
                    float baseRef=reference-bilinearGuard;
                    float visibility=0;
                    [loop]for(uint filterSample=0;filterSample<16;++filterSample)
                    {
                        float2 d=ShadowFilterVogel16[filterSample];
                        float2 rot=float2(d.x*rotCos-d.y*rotSin,d.x*rotSin+d.y*rotCos);
                        float2 offsetUv=rot*filterRadiusUv;
                        float2 tapUv=uv+offsetUv;
                        float sampleVisibility=1;
                        if(all(tapUv>=0)&&all(tapUv<=1))
                        {
                            sampleVisibility=directionalShadow.SampleCmpLevelZero(
                                shadowSampler,float3(tapUv,level),baseRef+dot(clampedGradient,offsetUv));
                        }
                        visibility+=sampleVisibility;
                    }
                    result.visibility=visibility*(1.0/16.0);
                }
            }
        }
    }
    return result;
}
ShadowFilterEvaluation EvaluateShadowFilter(uint slice,float3 position,float3 normal,float3 lightDirection)
{
    return EvaluateShadowFilter(slice,position,normal,lightDirection,float2(0,0));
}
float3 ShadowFilterDebugColor(float3 position,float3 normal,float3 lightDirection,float2 screenPixel)
{
    float3 value=filterOptions.x<1.5?float3(1,1,1):float3(0,0,0);
    if(filterParameters.w>.5&&framePointCount.y>.5&&framePointCount.z>.5&&options.w>.5)
    {
        float3 selection=CascadeSelectionForPosition(position);
        if(selection.x>=0)
        {
            ShadowFilterEvaluation first=EvaluateShadowFilter((uint)selection.x,position,normal,lightDirection,screenPixel),second=first;
            if(selection.y!=selection.x)second=EvaluateShadowFilter((uint)selection.y,position,normal,lightDirection,screenPixel);
            if(filterOptions.x<1.5)value=lerp(first.visibility,second.visibility,selection.z).xxx;
            else value=lerp(float3(first.penumbraWorld,first.blockerGapWorld,first.blockerFraction),
                float3(second.penumbraWorld,second.blockerGapWorld,second.blockerFraction),selection.z);
        }
    }
    return value;
}
float3 ShadowFilterDebugColor(float3 position,float3 normal,float3 lightDirection)
{
    return ShadowFilterDebugColor(position,normal,lightDirection,float2(0,0));
}
