#include "PostBindings.hlsli"
#include "Fullscreen.hlsli"
Texture2D<float4> inputScene:register(t0);
SamplerState linearClamp:register(s0);
float Luma(float3 color){return dot(color,float3(0.299,0.587,0.114));}
float3 Fetch(float2 uv){return inputScene.SampleLevel(linearClamp,uv,0).rgb;}
float GrainHash(uint2 p,uint seed)
{
    uint n=(p.x*1597334677u)^(p.y*3812015801u)^(seed*2798796415u);
    n=(n^(n>>15u))*2246822519u;
    n^=(n>>13u);
    return float(n)*(1.0/4294967296.0);
}
float4 PSMain(float4 position:SV_POSITION):SV_TARGET
{
    float2 reciprocal=postResolution.zw,uv=position.xy*reciprocal;float3 center=Fetch(uv),filtered=center;
    if(postOptions.x>0.5)
    {
        float nw=Luma(Fetch(uv+float2(-1,-1)*reciprocal)),ne=Luma(Fetch(uv+float2(1,-1)*reciprocal));
        float sw=Luma(Fetch(uv+float2(-1,1)*reciprocal)),se=Luma(Fetch(uv+float2(1,1)*reciprocal)),middle=Luma(center);
        float minimum=min(middle,min(min(nw,ne),min(sw,se))),maximum=max(middle,max(max(nw,ne),max(sw,se)));
        if(maximum-minimum>=max(0.0312,maximum*0.125))
        {
            float2 direction=float2(-((nw+ne)-(sw+se)),(nw+sw)-(ne+se));
            float reduction=max((nw+ne+sw+se)*(0.25/8),1.0/128);
            direction=clamp(direction/(min(abs(direction.x),abs(direction.y))+reduction),-8,8)*reciprocal;
            float3 a=0.5*(Fetch(uv+direction*(-1.0/6))+Fetch(uv+direction*(1.0/6)));
            float3 b=a*0.5+0.25*(Fetch(uv+direction*(-0.5))+Fetch(uv+direction*0.5));
            float luma=Luma(b);filtered=luma<minimum||luma>maximum?a:b;
        }
    }
    // postOptions.y：分辨率/SSAA 自适应 35mm 卤化银胶片颗粒（含柔和有机乳剂团块与微弱三层感光色偏，避免超采样下采样抵消）。
    if(postOptions.y>1e-5)
    {
        uint seed=uint(max(postOptions.z,0.0));
        float grainScale=max(postResolution.y/1080.0,1.0);
        float2 filmPos=position.xy/grainScale;
        float2 fineCoord=filmPos*1.18;
        uint2 fi=uint2(floor(fineCoord));
        float2 ff=frac(fineCoord);
        float2 fh=ff*ff*(3.0-2.0*ff);
        float f00=GrainHash(fi,seed),f10=GrainHash(fi+uint2(1u,0u),seed);
        float f01=GrainHash(fi+uint2(0u,1u),seed),f11=GrainHash(fi+uint2(1u,1u),seed);
        float fine=(lerp(lerp(f00,f10,fh.x),lerp(f01,f11,fh.x),fh.y)-0.5)*2.0;

        float2 rotPos=float2(filmPos.x*0.7986-filmPos.y*0.6018,filmPos.x*0.6018+filmPos.y*0.7986)*0.52;
        uint2 ci=uint2(floor(rotPos));
        float2 cf=frac(rotPos);
        float2 ch=cf*cf*(3.0-2.0*cf);
        float c00=GrainHash(ci,seed+19u),c10=GrainHash(ci+uint2(1u,0u),seed+19u);
        float c01=GrainHash(ci+uint2(0u,1u),seed+19u),c11=GrainHash(ci+uint2(1u,1u),seed+19u);
        float clump=(lerp(lerp(c00,c10,ch.x),lerp(c01,c11,ch.x),ch.y)-0.5)*2.0;

        float mono=fine*0.66+clump*0.34;
        float dyeR=(GrainHash(fi+uint2(13u,7u),seed+41u)-0.5)*2.0;
        float dyeB=(GrainHash(fi+uint2(29u,3u),seed+53u)-0.5)*2.0;
        float3 filmGrain=float3(mono+dyeR*0.14,mono-(dyeR+dyeB)*0.05,mono+dyeB*0.16);
        float lum=saturate(Luma(filtered));
        float response=smoothstep(0.015,0.12,lum)*(4.0*lum*(1.0-lum)*0.68+0.30*sqrt(lum))*(1.0-0.45*smoothstep(0.88,1.0,lum));
        filtered=saturate(filtered+filmGrain*(postOptions.y*0.092*response));
    }
    return float4(DecodeSrgb(filtered),1);
}
