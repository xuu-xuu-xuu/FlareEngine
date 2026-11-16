#ifdef SE_CASCADED
Texture2DArray<float> directionalShadow:register(t3);
#else
Texture2D<float> directionalShadow:register(t3);
#endif
SamplerComparisonState shadowSampler:register(s1);
#ifdef SE_CASCADED
#ifndef SE_CSM_RECEIVER_PLANE
#define SE_CSM_RECEIVER_PLANE 1
#endif
float2 CascadeReceiverGradient(uint slice,float3 normal)
{
    float2 gradient=float2(0,0);
#if SE_CSM_RECEIVER_PLANE
    // 由正交光矩阵与几何切平面解析求 d(depth)/d(uv)，不借用屏幕 quad 的相邻级导数。
    // 解 row0.dp=2du、row1.dp=-2dv、normal.dp=0；row2 自带 RH 深度的符号。
    float3 row0=csmClipFromWorld[min(slice,2u)][0].xyz;
    float3 row1=csmClipFromWorld[min(slice,2u)][1].xyz;
    float3 row2=csmClipFromWorld[min(slice,2u)][2].xyz;
    float3 planeAxis=cross(row0,row1);
    float determinant=dot(normal,planeAxis);
    if(abs(determinant)>max(length(planeAxis)*1e-5,1e-12))
        gradient=float2(2*dot(row2,cross(row1,normal)),-2*dot(row2,cross(normal,row0)))/determinant;
#endif
    return gradient;
}
float3 CascadeSelectionForPosition(float3 position)
{
    float depth=dot(float4(position,1),csmViewDepth);
    uint count=(uint)clamp(csmOptions.x,1.0,3.0);
    float last=csmSplits.x;if(count>1)last=csmSplits.y;if(count>2)last=csmSplits.z;
    float3 result=float3(-1,-1,0);
    if(depth>=csmSplits.w&&depth<=last)
    {
        uint selected=0;
        if(count>1&&depth>csmSplits.x)selected=1;
        if(count>2&&depth>csmSplits.y)selected=2;
        result=float3(selected,selected,0);
        [unroll]for(uint i=0;i<2;++i)if(i+1<count)
        {
            float split=i==0?csmSplits.x:csmSplits.y;
            float previous=i==0?csmSplits.w:csmSplits.x;
            float width=(split-previous)*csmOptions.y;
            if(width>0&&depth>=split-width&&depth<=split+width)
                result=float3(i,i+1,saturate((depth-split+width)/(2*width)));
        }
    }
    return result;
}
#ifdef SE_PCSS
#include "ShadowFilterBindings.hlsli"
#endif
float CascadeVisibility(uint slice,float3 position,float3 normal,float3 lightDirection)
{
    float result=1;
#ifdef SE_PCSS
    if(filterParameters.w>.5&&framePointCount.z>.5)result=EvaluateShadowFilter(slice,position,normal,lightDirection).visibility;
    else
#endif
    {
    float4 clip=mul(csmClipFromWorld[min(slice,2u)],float4(position+normal*shadowParameters.w,1));
    if(clip.w>0)
    {
        float3 coordinate=clip.xyz/clip.w;
        float2 uv=float2(coordinate.x*.5+.5,.5-coordinate.y*.5);
        if(all(uv>=0)&&all(uv<=1)&&coordinate.z>=0&&coordinate.z<=1)
        {
            float reference=coordinate.z-shadowParameters.y-shadowParameters.z*(1-saturate(dot(normal,lightDirection)));
            float2 gradient=CascadeReceiverGradient(slice,normal);
            float2 sampleUv=uv;
#if SE_CSM_RECEIVER_PLANE
            // 比较采样器为 point：先对准真实深度 texel 中心，再修正平面深度。
            // 只补整数 tap 会残留半个 texel 的量化偏差，仍在斜面上形成条纹。
            sampleUv=(floor(uv/shadowParameters.x)+.5)*shadowParameters.x;
#endif
            float2 centerDelta=sampleUv-uv;
            result=directionalShadow.SampleCmpLevelZero(shadowSampler,float3(sampleUv,slice),reference+dot(gradient,centerDelta));
            if(framePointCount.z>.5)
            {
                float filtered=0;
                [unroll]for(int y=-2;y<=2;++y)[unroll]for(int x=-2;x<=2;++x)
                {
                    float2 tap=float2(x,y)*shadowParameters.x;
                    float tapReference=reference+dot(gradient,centerDelta+tap);
                    filtered+=directionalShadow.SampleCmpLevelZero(shadowSampler,float3(sampleUv+tap,slice),tapReference);
                }
                result=filtered/25;
            }
        }
    }
    }
    return result;
}
float3 CascadeDebugColor(float3 position)
{
    float3 selection=CascadeSelectionForPosition(position);
    float3 result=float3(.1,.1,.1);
    if(selection.x>=0)
    {
        float3 first=float3(.8,.12,.08),second=first;
        if(selection.x>.5)first=float3(.12,.7,.18);
        if(selection.x>1.5)first=float3(.1,.24,.9);
        if(selection.y>.5)second=float3(.12,.7,.18);
        if(selection.y>1.5)second=float3(.1,.24,.9);
        result=lerp(first,second,selection.z);
    }
    return result;
}
#endif
float ShadowVisibility(float3 position,float3 normal,float3 lightDirection)
{
    float visibility=1;
#ifdef SE_CASCADED
    if(framePointCount.y>.5&&options.w>.5)
    {
        float3 selected=CascadeSelectionForPosition(position);
        if(selected.x>=0)
        {
            visibility=CascadeVisibility((uint)selected.x,position,normal,lightDirection);
            if(selected.y!=selected.x)visibility=lerp(visibility,CascadeVisibility((uint)selected.y,position,normal,lightDirection),selected.z);
        }
    }
#else
    if(framePointCount.y>0.5&&options.w>0.5)
    {
        float4 clip=mul(shadowClipFromWorld,float4(position+normal*shadowParameters.w,1));
        if(clip.w>0)
        {
            float3 coordinate=clip.xyz/clip.w;
            float2 uv=float2(coordinate.x*0.5+0.5,0.5-coordinate.y*0.5);
            if(all(uv>=0)&&all(uv<=1)&&coordinate.z>=0&&coordinate.z<=1)
            {
                float reference=coordinate.z-shadowParameters.y-shadowParameters.z*(1-saturate(dot(normal,lightDirection)));
                if(framePointCount.z<0.5)visibility=directionalShadow.SampleCmpLevelZero(shadowSampler,uv,reference);
                else
                {
                    float filtered=0;
                    [unroll]for(int y=-1;y<=1;++y)[unroll]for(int x=-1;x<=1;++x)
                        filtered+=directionalShadow.SampleCmpLevelZero(shadowSampler,uv+float2(x,y)*shadowParameters.x,reference);
                    visibility=filtered/9;
                }
            }
        }
    }
#endif
    return visibility;
}
