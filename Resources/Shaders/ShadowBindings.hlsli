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
float CascadeVisibility(uint slice,float3 position,float3 normal,float3 lightDirection,float2 screenPixel)
{
    float result=1;
#ifdef SE_PCSS
    if(filterParameters.w>.5&&framePointCount.z>.5)result=EvaluateShadowFilter(slice,position,normal,lightDirection,screenPixel).visibility;
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
            if(framePointCount.z<=.5)
            {
                float2 sampleUv=uv;
#if SE_CSM_RECEIVER_PLANE
                // 非 PCF 模式对准真实深度 texel 中心，保持严格单纹素点采样语义。
                sampleUv=(floor(uv/shadowParameters.x)+.5)*shadowParameters.x;
#endif
                float2 centerDelta=sampleUv-uv;
                result=directionalShadow.SampleCmpLevelZero(shadowSampler,float3(sampleUv,slice),reference+dot(gradient,centerDelta));
            }
            else
            {
                // Sigg & Hadwiger (GPU Gems 2)：利用 2×2=4 次硬件双线性比较采样精确求值 4×4 三次 B 样条平滑滤波核。
                float2 maxGrad=0.0015/max(shadowParameters.xx,1e-6);
                float2 clampedGradient=clamp(gradient,-maxGrad,maxGrad);
                float bilinearGuard=0.5*dot(abs(clampedGradient),shadowParameters.xx);
                float2 invTexel=1.0/max(shadowParameters.xx,1e-6);
                float2 texPos=uv*invTexel-0.5;
                float2 baseTex=floor(texPos);
                float2 f=texPos-baseTex;
                float2 oneMinusF=1.0-f;
                float2 g0=(oneMinusF*oneMinusF*oneMinusF)*(1.0/6.0);
                float2 g1=(4.0-6.0*f*f+3.0*f*f*f)*(1.0/6.0);
                float2 g2=(1.0+3.0*f+3.0*f*f-3.0*f*f*f)*(1.0/6.0);
                float2 g3=(f*f*f)*(1.0/6.0);
                float2 w0=g0+g1;
                float2 w1=1.0-w0;
                float2 uv0=(baseTex-0.5+g1/max(w0,1e-6))*shadowParameters.x;
                float2 uv1=(baseTex+1.5+g3/max(w1,1e-6))*shadowParameters.x;
                float baseRef=reference-bilinearGuard;
                float c00=directionalShadow.SampleCmpLevelZero(shadowSampler,float3(uv0.x,uv0.y,slice),baseRef+dot(clampedGradient,float2(uv0.x,uv0.y)-uv));
                float c10=directionalShadow.SampleCmpLevelZero(shadowSampler,float3(uv1.x,uv0.y,slice),baseRef+dot(clampedGradient,float2(uv1.x,uv0.y)-uv));
                float c01=directionalShadow.SampleCmpLevelZero(shadowSampler,float3(uv0.x,uv1.y,slice),baseRef+dot(clampedGradient,float2(uv0.x,uv1.y)-uv));
                float c11=directionalShadow.SampleCmpLevelZero(shadowSampler,float3(uv1.x,uv1.y,slice),baseRef+dot(clampedGradient,float2(uv1.x,uv1.y)-uv));
                result=lerp(lerp(c00,c10,w1.x),lerp(c01,c11,w1.x),w1.y);
            }
        }
    }
    }
    return result;
}
float CascadeVisibility(uint slice,float3 position,float3 normal,float3 lightDirection)
{
    return CascadeVisibility(slice,position,normal,lightDirection,float2(0,0));
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
float ShadowVisibility(float3 position,float3 normal,float3 lightDirection,float2 screenPixel)
{
    float visibility=1;
#ifdef SE_CASCADED
    if(framePointCount.y>.5&&options.w>.5)
    {
        float3 selected=CascadeSelectionForPosition(position);
        if(selected.x>=0)
        {
            visibility=CascadeVisibility((uint)selected.x,position,normal,lightDirection,screenPixel);
            if(selected.y!=selected.x)visibility=lerp(visibility,CascadeVisibility((uint)selected.y,position,normal,lightDirection,screenPixel),selected.z);
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
                if(framePointCount.z<0.5)
                {
                    float2 center=(floor(uv/shadowParameters.x)+0.5)*shadowParameters.x;
                    visibility=directionalShadow.SampleCmpLevelZero(shadowSampler,center,reference);
                }
                else
                {
                    float2 invTexel=1.0/max(shadowParameters.xx,1e-6);
                    float2 texPos=uv*invTexel-0.5;
                    float2 baseTex=floor(texPos);
                    float2 f=texPos-baseTex;
                    float2 oneMinusF=1.0-f;
                    float2 g0=(oneMinusF*oneMinusF*oneMinusF)*(1.0/6.0);
                    float2 g1=(4.0-6.0*f*f+3.0*f*f*f)*(1.0/6.0);
                    float2 g2=(1.0+3.0*f+3.0*f*f-3.0*f*f*f)*(1.0/6.0);
                    float2 g3=(f*f*f)*(1.0/6.0);
                    float2 w0=g0+g1;
                    float2 w1=1.0-w0;
                    float2 uv0=(baseTex-0.5+g1/max(w0,1e-6))*shadowParameters.x;
                    float2 uv1=(baseTex+1.5+g3/max(w1,1e-6))*shadowParameters.x;
                    float c00=directionalShadow.SampleCmpLevelZero(shadowSampler,float2(uv0.x,uv0.y),reference);
                    float c10=directionalShadow.SampleCmpLevelZero(shadowSampler,float2(uv1.x,uv0.y),reference);
                    float c01=directionalShadow.SampleCmpLevelZero(shadowSampler,float2(uv0.x,uv1.y),reference);
                    float c11=directionalShadow.SampleCmpLevelZero(shadowSampler,float2(uv1.x,uv1.y),reference);
                    visibility=lerp(lerp(c00,c10,w1.x),lerp(c01,c11,w1.x),w1.y);
                }
            }
        }
    }
#endif
    return visibility;
}
float ShadowVisibility(float3 position,float3 normal,float3 lightDirection)
{
    return ShadowVisibility(position,normal,lightDirection,float2(0,0));
}
