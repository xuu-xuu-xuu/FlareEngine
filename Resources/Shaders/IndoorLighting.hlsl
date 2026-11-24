// 室内不透明阶段，输入均来自本帧同一次几何投影。HDR 快照从不读取本次合成的输出。
cbuffer IndoorConstants : register(b0)
{
    column_major float4x4 indoorView,indoorProjection,indoorInverseProjection,indoorInverseView;
    column_major float4x4 indoorPreviousClipFromWorld;
    float4 viewport;      // 宽、高、1/宽、1/高
    float4 strengths;     // 漫反射、镜面、雾消光 (1/m)、雾最大距离 (m)
    float4 fogAlbedo;
    float4 sunDirection,sunRadiance;
    float4 environment;   // 强度、cos(yaw)、sin(yaw)、镜面最大 mip
    float4 ordinaryLightCount;
    float4 ordinaryPosition[8],ordinaryRadiance[8];
    column_major float4x4 sunClip[4];
    float4 sunSplits,sunShadowParameters;
    float4 sampling;      // x=shift, y=1/stride, z=bounceWidth, w=bounceHeight
    uint indoorFrameIndex;
    float indoorHistoryBlendWeight;
    float indoorHistoryValid;
    uint indoorAtrousStep;
    float4 fogParameters; // x=heightFalloff, y=baseHeight, z=sunScatter, w=turbulence
};
cbuffer IndoorFilterPassConstants : register(b1)
{
    uint4 indoorFilterParams; // x=stepSize, y=bounceWidth, z=bounceHeight, w=passIndex
};
Texture2D<float4> sceneRadiance : register(t0);
Texture2D<float> sceneDepth : register(t1);
Texture2D<float2> sceneNormal : register(t2);
Texture2D<float4> sceneMaterial : register(t3);
TextureCube<float4> sceneEnvironment : register(t4);
Texture2D<float4> environmentBrdf : register(t5);
Texture2D<float> sunDepth : register(t6);
Texture2DArray<float> sunCascades : register(t7);
Texture2D<float4> diffuseIrradiance : register(t8);
Texture2D<float4> indoorExtraA : register(t9);
Texture2D<float4> indoorExtraB : register(t10);
RWTexture2D<float4> indoorOutA : register(u0);
RWTexture2D<float4> indoorOutB : register(u1);
#include "LocalLightBindings.hlsli"
SamplerState indoorSampler : register(s0);
static const float Pi=3.14159265;
float4 VSMain(uint id:SV_VertexID):SV_POSITION
{
    float2 uv=float2((id<<1)&2,id&2);
    return float4(uv*float2(2,-2)+float2(-1,1),0,1);
}
float3 Unit(float3 value,float3 fallback)
{
    float s=dot(value,value);return s>1e-10?value*rsqrt(s):fallback;
}
float3 ViewAt(float2 uv,float depth)
{
    float4 p=mul(indoorInverseProjection,float4(uv*float2(2,-2)+float2(-1,1),depth,1));
    return p.xyz/(abs(p.w)>1e-8?p.w:1e-8);
}
float2 PixelUv(int2 pixel){return (float2(pixel)+.5)*viewport.zw;}
bool Project(float3 viewPosition,out float2 uv)
{
    float4 clipPoint=mul(indoorProjection,float4(viewPosition,1));
    uv=clipPoint.xy/max(clipPoint.w,1e-7)*float2(.5,-.5)+.5;
    return clipPoint.w>1e-6&&clipPoint.z>=0&&clipPoint.z<=clipPoint.w&&all(uv>=0)&&all(uv<1);
}
float3 DecodeWorldNormal(float2 packed)
{
    float3 n=float3(packed,1-abs(packed.x)-abs(packed.y));
    if(n.z<0)n.xy=(1-abs(n.yx))*float2(n.x<0?-1:1,n.y<0?-1:1);
    return Unit(n,float3(0,1,0));
}
float3 WorldNormal(int2 pixel)
{
    return DecodeWorldNormal(sceneNormal.Load(int3(pixel,0)));
}
float3 ViewNormal(int2 pixel){return mul((float3x3)indoorView,WorldNormal(pixel));}
bool ClearSegment(float3 a,float3 b)
{
    // 仅比较射线内部样点，避开两端的自身深度误差。
    bool clear=true;
    [unroll]for(uint i=1;i<=3;++i)
    {
        float3 p=lerp(a,b,i*.25);float2 uv;
        bool projected=Project(p,uv);
        int2 pixel=clamp((int2)(uv*viewport.xy),int2(0,0),(int2)viewport.xy-1);
        float depth=sceneDepth.Load(int3(pixel,0));
        clear=clear&&projected&&(depth>=1||ViewAt(PixelUv(pixel),depth).z<=p.z+max(.04,-p.z*.006));
    }
    return clear;
}
uint PixelHash(uint2 p)
{
    uint n=p.x*0x9E3779B9u+p.y*0x85EBCA6Bu;
    n^=n>>16;n*=0x7FEB352Du;n^=n>>15;return n;
}
float2 LowDiscrepancyR2(uint index,uint2 gridPixel)
{
    // Cranley-Patterson 旋转 + R2 低差异序列，确保时域累积逐帧均匀覆盖半球样点。
    const float2 alpha=float2(0.754877666,0.569840291);
    uint h=PixelHash(gridPixel);
    float2 shift=float2((h&0xffffu)/65536.0,((h>>16)&0xffffu)/65536.0);
    return frac(shift+float(index%256u)*alpha);
}
float3 SourceRadiance(int2 samplePixel,float3 samplePoint,float3 sampleNormal,float footprint)
{
    // HDR 是有方向性的屏幕样本。对代表的有限表面元求平均，使小灯泡只按其覆盖面积入射。
    const float2 offsets[5]={float2(0,0),float2(.69,.28),float2(-.28,.69),float2(-.69,-.28),float2(.28,-.69)};
    float3 color=0;
    [unroll]for(uint i=0;i<5;++i)
    {
        int2 texel=samplePixel+(int2)round(offsets[i]*footprint);
        if(any(texel<0)||any(texel>=int2(viewport.xy)))continue;
        float depth=sceneDepth.Load(int3(texel,0));
        if(depth>=1)continue;
        float3 delta=ViewAt(PixelUv(texel),depth)-samplePoint;
        if(abs(dot(delta,sampleNormal))>max(.06,-samplePoint.z*.01)||dot(ViewNormal(texel),sampleNormal)<.75)continue;
        float code=sceneMaterial.Load(int3(texel,0)).a;
        float fraction=code<0?1:1-(((uint)round(max(0,code)))/32)/31.0;
        color+=max(sceneRadiance.Load(int3(texel,0)).rgb,0)*fraction;
    }
    return color*.2;
}
float3 DiffuseBounce(int2 receiver,float3 p,float3 normal,uint2 gridPixel)
{
    // 对其他可见表面像素积分离散形状因子。投影像素面积通过发射面倾角还原。
    float2 uv=PixelUv(receiver);float d=sceneDepth.Load(int3(receiver,0));
    float pixelWidth=length(ViewAt(uv+float2(viewport.z,0),d)-p);
    float radiusPixels=clamp(2.2/max(pixelWidth,1e-5),3,min(viewport.x,viewport.y)*.42);
    float3 incoming=0;float sumWeight=0;
    float2 lds=LowDiscrepancyR2(indoorFrameIndex,gridPixel);
    uint seed=PixelHash(gridPixel^uint2(indoorFrameIndex*1664525u+1013904223u,indoorFrameIndex*22695477u+1u));
    float rotation=lds.x*(2*Pi);
    [loop]for(uint ring=0;ring<4;++ring)
    {
        float inner=ring*.25,outer=(ring+1)*.25;
        float areaPixels=Pi*(outer*outer-inner*inner)*radiusPixels*radiusPixels/12.0;
        [loop]for(uint ray=0;ray<12;++ray)
        {
            float angle=rotation+(ray+ring*.382)*(Pi/6);
            uint jitter=PixelHash(uint2(seed+ray,ring));
            float tRadial=frac(lds.y+((jitter&0xffffu)+.5)/65536.0);
            float radius=sqrt(lerp(inner*inner,outer*outer,tRadial))*radiusPixels;
            int2 samplePixel=receiver+(int2)round(float2(cos(angle),sin(angle))*radius);
            if(any(samplePixel<0)||any(samplePixel>=int2(viewport.xy)))continue;
            float z=sceneDepth.Load(int3(samplePixel,0));if(z>=1)continue;
            float2 sampleUv=PixelUv(samplePixel);float3 q=ViewAt(sampleUv,z);
            float3 delta=q-p;float lengthSquared=dot(delta,delta);if(lengthSquared<.0025||lengthSquared>16)continue;
            float3 l=delta*rsqrt(lengthSquared),other=ViewNormal(samplePixel);
            float receiverCos=saturate(dot(normal,l)),sourceCos=saturate(dot(other,-l));
            if(receiverCos*sourceCos<=.0001)continue;
            float3 dx=ViewAt(sampleUv+float2(viewport.z,0),z)-q;
            float3 dy=ViewAt(sampleUv+float2(0,viewport.w),z)-q;
            float3 eye=Unit(ViewAt(sampleUv,0)-q,float3(0,0,1));
            float area=length(cross(dx,dy))*areaPixels/max(.2,abs(dot(other,eye)));
            float weight=min(.35,receiverCos*sourceCos*area/(Pi*lengthSquared));
            if(!ClearSegment(p+normal*.025,q+other*.025))continue;
            incoming+=SourceRadiance(samplePixel,q,other,min(14,sqrt(areaPixels)*.48))*weight;
            sumWeight+=weight;
        }
    }
    return incoming/max(1,sumWeight);
}
int2 BouncePixel(int2 gridPixel)
{
    const int shift=max(2,(int)sampling.x),stride=1<<shift;
    return min((gridPixel<<shift)+(stride>>1),(int2)viewport.xy-1);
}
[numthreads(8,8,1)]
void CSSampleBounce(uint3 dispatchId:SV_DispatchThreadID)
{
    int2 gridPixel=(int2)dispatchId.xy;
    int2 extent=max(int2(1,1),(int2)indoorFilterParams.yz);
    if(any(gridPixel>=extent))return;
    int2 pixel=BouncePixel(gridPixel);
    float depth=sceneDepth.Load(int3(pixel,0));
    float4 output=0;
    if(depth<1)
    {
        float3 p=ViewAt(PixelUv(pixel),depth);
        output=float4(DiffuseBounce(pixel,p,ViewNormal(pixel),(uint2)gridPixel),max(-p.z,1e-4));
    }
    indoorOutA[gridPixel]=output;
}
[numthreads(8,8,1)]
void CSTemporalAccumulate(uint3 dispatchId:SV_DispatchThreadID)
{
    int2 gridPixel=(int2)dispatchId.xy;
    int2 extent=max(int2(1,1),(int2)indoorFilterParams.yz);
    if(any(gridPixel>=extent))return;
    int2 pixel=BouncePixel(gridPixel);
    float depth=sceneDepth.Load(int3(pixel,0));
    if(depth>=1)
    {
        indoorOutA[gridPixel]=0;
        indoorOutB[gridPixel]=0;
        return;
    }
    float4 currSample=diffuseIrradiance.Load(int3(gridPixel,0));
    float3 p=ViewAt(PixelUv(pixel),depth);
    float3 currViewN=ViewNormal(pixel);
    float2 currWorldNOct=sceneNormal.Load(int3(pixel,0));
    float3 currWorldN=DecodeWorldNormal(currWorldNOct);
    float currDepthMetric=max(-p.z,1e-4);

    // 3x3 法线/深度门控邻域矩估计与方差夹取盒，用于抑制移动/熄灭光源拖影。
    float3 mean1=0,mean2=0;float sumW=0;
    float depthSigma=max(.05,currDepthMetric*.02);
    [unroll]for(int dy=-1;dy<=1;++dy)[unroll]for(int dx=-1;dx<=1;++dx)
    {
        int2 q=gridPixel+int2(dx,dy);
        if(any(q<0)||any(q>=extent))continue;
        float4 s=diffuseIrradiance.Load(int3(q,0));
        if(s.a<=0)continue;
        float w=(dx==0&&dy==0)?1.0:saturate(dot(currViewN,ViewNormal(BouncePixel(q))));
        if(w<.7)continue;
        float dz=abs(s.a-currDepthMetric)/depthSigma;
        w*=exp(-dz*dz*.5);
        if(w<=1e-4)continue;
        mean1+=s.rgb*w;
        mean2+=(s.rgb*s.rgb)*w;
        sumW+=w;
    }
    mean1/=max(sumW,1e-5);
    mean2/=max(sumW,1e-5);
    float3 variance=max(0,mean2-mean1*mean1);
    float3 stddev=sqrt(variance);
    float3 boxMin=max(0,mean1-1.25*stddev);
    float3 boxMax=mean1+1.25*stddev;

    float3 accumulated=currSample.rgb;
    float lum=dot(currSample.rgb,float3(.2126,.7152,.0722));
    float2 moments=float2(lum,lum*lum);
    if(indoorHistoryValid>.5)
    {
        float3 worldPos=mul(indoorInverseView,float4(p,1)).xyz;
        float4 prevClip=mul(indoorPreviousClipFromWorld,float4(worldPos,1));
        if(prevClip.w>1e-6)
        {
            float2 prevUv=prevClip.xy/prevClip.w*float2(.5,-.5)+.5;
            if(all(prevUv>=0)&&all(prevUv<1))
            {
                float2 prevTexel=prevUv*float2(extent)-.5;
                int2 baseTexel=(int2)floor(prevTexel);
                float2 fracPos=frac(prevTexel);
                float3 histSum=0;float2 momSum=0;float wSum=0;
                float relDepthTol=max(.08,currDepthMetric*.08);
                [unroll]for(int oy=0;oy<=1;++oy)[unroll]for(int ox=0;ox<=1;++ox)
                {
                    int2 tc=clamp(baseTexel+int2(ox,oy),int2(0,0),extent-1);
                    float4 hRad=indoorExtraA.Load(int3(tc,0));
                    if(hRad.a<=0)continue;
                    if(abs(hRad.a-currDepthMetric)>relDepthTol)continue;
                    float4 hMom=indoorExtraB.Load(int3(tc,0));
                    float3 prevWorldN=DecodeWorldNormal(hMom.zw);
                    if(dot(currWorldN,prevWorldN)<.85)continue;
                    float bw=(ox==0?1-fracPos.x:fracPos.x)*(oy==0?1-fracPos.y:fracPos.y);
                    histSum+=hRad.rgb*bw;
                    momSum+=hMom.rg*bw;
                    wSum+=bw;
                }
                if(wSum>.05)
                {
                    float3 prevColor=clamp(histSum/wSum,boxMin,boxMax);
                    float2 prevMom=momSum/wSum;
                    float blend=clamp(indoorHistoryBlendWeight,0,.96);
                    accumulated=lerp(currSample.rgb,prevColor,blend);
                    moments=lerp(moments,prevMom,blend);
                }
            }
        }
    }
    indoorOutA[gridPixel]=float4(max(accumulated,0),currDepthMetric);
    indoorOutB[gridPixel]=float4(moments,currWorldNOct);
}
[numthreads(8,8,1)]
void CSATrousFilter(uint3 dispatchId:SV_DispatchThreadID)
{
    int2 gridPixel=(int2)dispatchId.xy;
    int2 extent=max(int2(1,1),(int2)indoorFilterParams.yz);
    if(any(gridPixel>=extent))return;
    float4 centerSample=diffuseIrradiance.Load(int3(gridPixel,0));
    if(centerSample.a<=0){indoorOutA[gridPixel]=0;return;}
    int2 centerPixel=BouncePixel(gridPixel);
    float centerDepth=sceneDepth.Load(int3(centerPixel,0));
    if(centerDepth>=1){indoorOutA[gridPixel]=0;return;}
    float3 centerP=ViewAt(PixelUv(centerPixel),centerDepth);
    float3 centerN=ViewNormal(centerPixel);
    float4 centerMom=indoorExtraA.Load(int3(gridPixel,0));
    float variance=max(1e-6,centerMom.g-centerMom.r*centerMom.r);
    float sigmaL=max(.025,4.0*sqrt(variance));
    float centerLum=dot(centerSample.rgb,float3(.2126,.7152,.0722));
    int stepSize=max(1,(int)indoorFilterParams.x);
    float sigmaZ=max(.035,-centerP.z*.012*float(stepSize));
    const float kWeights[5]={1.0/16.0,4.0/16.0,6.0/16.0,4.0/16.0,1.0/16.0};
    float3 sum=0;float sumWeight=0;
    [unroll]for(int oy=-2;oy<=2;++oy)[unroll]for(int ox=-2;ox<=2;++ox)
    {
        int2 qGrid=gridPixel+int2(ox,oy)*stepSize;
        if(any(qGrid<0)||any(qGrid>=extent))continue;
        float4 qSample=diffuseIrradiance.Load(int3(qGrid,0));
        if(qSample.a<=0)continue;
        float kw=kWeights[ox+2]*kWeights[oy+2];
        if(ox==0&&oy==0)
        {
            sum+=qSample.rgb*kw;sumWeight+=kw;continue;
        }
        int2 qPixel=BouncePixel(qGrid);
        float qDepth=sceneDepth.Load(int3(qPixel,0));
        if(qDepth>=1)continue;
        float3 qN=ViewNormal(qPixel);
        float cosN=saturate(dot(centerN,qN));
        if(cosN<.70)continue;
        float wNormal=pow(cosN,16.0);
        float3 qP=ViewAt(PixelUv(qPixel),qDepth);
        float3 delta=qP-centerP;
        float planeDist=max(abs(dot(delta,centerN)),abs(dot(delta,qN)));
        float wDepth=exp(-(planeDist*planeDist)/(2.0*sigmaZ*sigmaZ));
        float qLum=dot(qSample.rgb,float3(.2126,.7152,.0722));
        float wLum=exp(-abs(centerLum-qLum)/sigmaL);
        float w=kw*wNormal*wDepth*wLum;
        sum+=qSample.rgb*w;
        sumWeight+=w;
    }
    indoorOutA[gridPixel]=float4(sum/max(sumWeight,1e-6),centerSample.a);
}
float3 ReconstructDiffuse(int2 receiver,float3 p,float3 n)
{
    // 对已时域累积与 3 趟 À-Trous 降噪的稀疏辐亮度做全分辨率联合双边升采样。
    // 接收面颜色留待升采样后再乘，墙上花纹不会在四分之一分辨率处被抹掉。
    const int shift=max(2,(int)sampling.x);
    int2 cell=receiver>>shift,extent=((int2)viewport.xy+(1<<shift)-1)>>shift;
    float sigma=max(.035,-p.z*.008),total=0;float3 sum=0;
    [loop]for(int y=-2;y<=2;++y)[loop]for(int x=-2;x<=2;++x)
    {
        int2 q=cell+int2(x,y);if(any(q<0)||any(q>=extent))continue;
        float4 sampleValue=diffuseIrradiance.Load(int3(q,0));
        if(sampleValue.a<=0)continue;
        int2 neighbor=BouncePixel(q);
        float3 qNormal=ViewNormal(neighbor);float cosine=saturate(dot(n,qNormal));
        if(cosine<.75)continue;
        float depth=sceneDepth.Load(int3(neighbor,0));
        float3 delta=ViewAt(PixelUv(neighbor),depth)-p;
        float separation=max(abs(dot(delta,n)),abs(dot(delta,qNormal)))/sigma;
        float2 offset=(float2(neighbor)-float2(receiver))*sampling.y;
        float weight=exp(-.16*dot(offset,offset)-separation*separation)*pow(cosine,12);
        sum+=sampleValue.rgb*weight;total+=weight;
    }
    return sum/max(total,1e-5);
}
bool ReflectionHit(float3 origin,float3 direction,out float2 hitUv,out float confidence)
{
    hitUv=0;confidence=0;bool found=false;
    float previous=.035;
    [loop]for(uint i=1;i<=40;++i)
    {
        float distance=.035+.027*i+.011*i*i;float3 p=origin+direction*distance;
        float2 uv;if(!Project(p,uv))break;
        int2 pixel=(int2)(uv*viewport.xy);
        float depth=sceneDepth.Load(int3(pixel,0));
        if(depth<1)
        {
            float gap=ViewAt(PixelUv(pixel),depth).z-p.z;
            // 在射线穿入表面的第一步做细分，厚度以视空间米计。
            if(gap>=0&&gap<.10+abs(direction.z)*(distance-previous)*1.4)
            {
                float lo=previous,hi=distance;
                [unroll]for(uint b=0;b<5;++b)
                {
                    float mid=(lo+hi)*.5;float3 test=origin+direction*mid;float2 u;
                    Project(test,u);
                    int2 samplePixel=(int2)(u*viewport.xy);
                    float sampleDepth=sceneDepth.Load(int3(samplePixel,0));
                    if(sampleDepth<1&&ViewAt(PixelUv(samplePixel),sampleDepth).z>=test.z)hi=mid;else lo=mid;
                }
                float3 hit=origin+direction*hi;
                if(!Project(hit,hitUv))break;
                int2 hitPixel=(int2)(hitUv*viewport.xy);float hitDepth=sceneDepth.Load(int3(hitPixel,0));
                float residual=ViewAt(PixelUv(hitPixel),hitDepth).z-hit.z;
                float facing=dot(ViewNormal(hitPixel),-direction);
                if(hitDepth<1&&abs(residual)<.13&&facing>.05&&hi>.08)
                {
                    float2 edge=min(hitUv,1-hitUv);
                    confidence=saturate(min(edge.x,edge.y)*14)*saturate(facing*5)*saturate(1-hi/24);
                    found=confidence>0;break;
                }
            }
        }
        previous=distance;
    }
    return found;
}
float3 ReflectedChange(float3 p,float3 n,float3 eye,float3 f0,float rough)
{
    float3 change=0;
    float3 direction=reflect(-eye,n);float2 uv=0;float confidence=0;
    if(rough<.68&&ReflectionHit(p+n*max(.025,-p.z*.001),direction,uv,confidence))
    {
    float3 observed=sceneRadiance.SampleLevel(indoorSampler,uv,0).rgb;
    float nv=saturate(dot(n,eye));
    float3 specularWeight=f0+(1-f0)*pow(1-nv,5);
    float3 original=0;
    if(environment.x>0)
    {
        float3 r=mul((float3x3)indoorInverseView,direction);
        float3 envDirection=float3(environment.y*r.x-environment.z*r.z,r.y,environment.z*r.x+environment.y*r.z);
        original=sceneEnvironment.SampleLevel(indoorSampler,envDirection,rough*environment.w).rgb*environment.x;
        float2 ab=environmentBrdf.SampleLevel(indoorSampler,float2(nv,rough),0).rg;
        specularWeight=f0*ab.x+ab.y;
    }
    // 命中在同一镜面瓣上替换环境辐亮度，屏外或背面命中保留原环境。
    change=(observed-original)*specularWeight*(confidence*(1-smoothstep(.06,.68,rough)));
    }
    return change;
}
float SunVisibility(float3 world,float distanceView)
{
    int count=(int)sunShadowParameters.x;
    float visibility=1;
    if(count>0&&!(sunShadowParameters.z>.5&&distanceView>sunSplits[max(count-1,0)]))
    {
        uint startSlice=0;
        if(sunShadowParameters.z>.5)
        {
            startSlice=(distanceView>sunSplits.x?1u:0u)+(distanceView>sunSplits.y&&count>1?1u:0u)+(distanceView>sunSplits.z&&count>2?1u:0u);
            startSlice=min(startSlice,(uint)(count-1));
        }
        uint maxSlice=sunShadowParameters.z>.5?(uint)(count-1):0u;
        [loop]for(uint slice=startSlice;slice<=maxSlice;++slice)
        {
            float4 projected=mul(sunClip[slice],float4(world,1));
            float3 ndc=projected.xyz/max(projected.w,1e-8);
            if(projected.w>0&&all(abs(ndc.xy)<0.992)&&ndc.z>0.0005&&ndc.z<0.9995)
            {
                int res=max((int)sunShadowParameters.y,1);
                float2 texCoord=(ndc.xy*float2(.5,-.5)+.5)*sunShadowParameters.y-0.5;
                int2 basePixel=(int2)floor(texCoord);
                float2 fracUv=saturate(texCoord-float2(basePixel));
                int2 p00=clamp(basePixel,int2(0,0),int2(res-1,res-1));
                int2 p10=clamp(basePixel+int2(1,0),int2(0,0),int2(res-1,res-1));
                int2 p01=clamp(basePixel+int2(0,1),int2(0,0),int2(res-1,res-1));
                int2 p11=clamp(basePixel+int2(1,1),int2(0,0),int2(res-1,res-1));
                float ref=ndc.z-sunShadowParameters.w;
                float d00=sunShadowParameters.z>.5?sunCascades.Load(int4(p00,slice,0)):sunDepth.Load(int3(p00,0));
                float d10=sunShadowParameters.z>.5?sunCascades.Load(int4(p10,slice,0)):sunDepth.Load(int3(p10,0));
                float d01=sunShadowParameters.z>.5?sunCascades.Load(int4(p01,slice,0)):sunDepth.Load(int3(p01,0));
                float d11=sunShadowParameters.z>.5?sunCascades.Load(int4(p11,slice,0)):sunDepth.Load(int3(p11,0));
                visibility=lerp(lerp(step(ref,d00),step(ref,d10),fracUv.x),lerp(step(ref,d01),step(ref,d11),fracUv.x),fracUv.y);
                break;
            }
        }
    }
    return visibility;
}
float Phase(float cosine)
{
    const float g=.18;float t=max(.01,1+g*g-2*g*cosine);
    return (1-g*g)/(4*Pi*t*sqrt(t));
}
float IntegrateLocalFog(float3 origin,float3 direction,float rayLength,float3 center,float radius,uint lightIndex,bool special)
{
    // 沿光线与灯的影响球求交。在离灯最近处按等角度取样，ds/dθ=(ρ²+(s-s0)²)/ρ。
    // 逆平方亮度的尖峰因而得到密集采样，步长不随墙面深度忽快忽慢。
    float3 offset=center-origin;float closest=dot(offset,direction);
    float3 lateral=offset-closest*direction;float rhoSquared=dot(lateral,lateral);
    float chord=sqrt(max(0,radius*radius-rhoSquared));
    float start=max(0,closest-chord),end=min(rayLength,closest+chord),result=0;
    if(end>start)
    {
        // 普通直接光同样将最小距离限制为 .05m。此处仅正则化坐标变换。
        float rho=sqrt(max(.0025,rhoSquared));
        float a=atan((start-closest)/rho),b=atan((end-closest)/rho);
        float angleStep=(b-a)/16;
        [loop]for(uint i=0;i<16;++i)
        {
            float h=rho*tan(a+(i+.5)*angleStep);float t=clamp(closest+h,start,end);
            float3 world=origin+t*direction,delta=center-world;
            float distanceSquared=dot(delta,delta);float3 toLight=Unit(delta,float3(0,1,0));
            float ratio=distanceSquared/max(radius*radius,.0025),window=saturate(1-ratio*ratio);
            float attenuation=window*window/max(.0025,distanceSquared);
            if(special)attenuation=LocalLightAttenuation(lightIndex,world,float3(0,0,0),true,toLight);
            float ds=angleStep*(rho*rho+h*h)/rho;
            result+=exp(-strengths.z*t)*attenuation*Phase(dot(direction,toLight))*ds;
        }
    }
    return strengths.z*result;
}
float FogOpticalDepth(float density,float falloff,float baseHeight,float originY,float dirY,float rayLength)
{
    float opticalDepth=density*rayLength;
    if(falloff>1e-6)
    {
        float sigma0=density*exp(clamp(-falloff*(originY-baseHeight),-12.0,4.0));
        float k=-falloff*dirY*rayLength;
        opticalDepth=abs(k)>1e-4?sigma0*rayLength*(exp(clamp(k,-20.0,12.0))-1.0)/k:sigma0*rayLength*(1.0+0.5*k);
    }
    return opticalDepth;
}
float FogHash31(float3 p)
{
    uint3 q=uint3(int3(floor(p))*int3(1597334673,3812015801u,2798796415u));
    uint n=(q.x^q.y^q.z)*1597334673u;
    n^=n>>15;
    return float(n)*(1.0/4294967296.0);
}
float FogValueNoise3D(float3 p)
{
    float3 i=floor(p),f=frac(p);
    float3 u=f*f*(3.0-2.0*f);
    float n000=FogHash31(i),n100=FogHash31(i+float3(1,0,0));
    float n010=FogHash31(i+float3(0,1,0)),n110=FogHash31(i+float3(1,1,0));
    float n001=FogHash31(i+float3(0,0,1)),n101=FogHash31(i+float3(1,0,1));
    float n011=FogHash31(i+float3(0,1,1)),n111=FogHash31(i+float3(1,1,1));
    return lerp(lerp(lerp(n000,n100,u.x),lerp(n010,n110,u.x),u.y),
                lerp(lerp(n001,n101,u.x),lerp(n011,n111,u.x),u.y),u.z);
}
float FogTurbulentDensityFactor(float3 worldPos,float turbulence,float windPhase)
{
    float3 windOffset=float3(windPhase*3.2,windPhase*0.45,windPhase*1.7);
    float3 q=(worldPos+windOffset)*float3(0.014,0.032,0.014);
    float fbm=FogValueNoise3D(q)*0.65+FogValueNoise3D(q*2.37+float3(11.3,4.7,19.1))*0.35;
    // 保持均值接近 1.0，使开合处产生浓密云雾团块与清透缝隙。
    float billow=smoothstep(0.20,0.80,fbm)*1.65+0.18;
    return lerp(1.0,billow,saturate(turbulence));
}
float SoftVolumetricSunVisibility(float3 sampleWorld,float distanceView,float t,float turbulence,float windPhase)
{
    float result=1.0;
    if(sunShadowParameters.x>0)
    {
        // 气溶胶前向散射与太阳视直径半影：沿垂直日光平面做平滑 3D 扰动 + 4 点旋转柔化采样，根除合成感激光硬切边。
        float3 L=Unit(sunDirection.xyz,float3(0,1,0));
        float3 T=Unit(cross(abs(L.y)<0.98?float3(0,1,0):float3(1,0,0),L),float3(1,0,0));
        float3 B=cross(L,T);
        float3 wind=float3(windPhase*2.6,windPhase*0.35,windPhase*1.4);
        float3 wq=(sampleWorld+wind)*0.022;
        float2 aeroDrift=float2(FogValueNoise3D(wq)-0.5,FogValueNoise3D(wq+float3(13.7,5.3,9.1))-0.5);
        float penumbraRadius=turbulence*(2.6+0.036*t);
        float3 centerPos=sampleWorld+(T*aeroDrift.x+B*aeroDrift.y)*(penumbraRadius*2.2);

        float v0=SunVisibility(centerPos+T*penumbraRadius,distanceView);
        float v1=SunVisibility(centerPos-T*penumbraRadius,distanceView);
        float v2=SunVisibility(centerPos+B*penumbraRadius,distanceView);
        float v3=SunVisibility(centerPos-B*penumbraRadius,distanceView);
        float softVis=0.25*(v0+v1+v2+v3);

        // 高空流云斑驳透射调制（Cloud Gobo）：让丁达尔光柱呈现自然破云浓淡层次而非均匀光幕。
        float cloudProjScale=(140.0-sampleWorld.y)/max(L.y,0.14);
        float2 cloudUv=(sampleWorld.xz+L.xz*cloudProjScale+float2(windPhase*4.2,windPhase*1.9))*0.0075;
        float cloudFbm=FogValueNoise3D(float3(cloudUv.x,0.35,cloudUv.y))*0.65
                      +FogValueNoise3D(float3(cloudUv.x*2.4+7.1,1.7,cloudUv.y*2.4+3.9))*0.35;
        float cloudGobo=lerp(1.0,smoothstep(0.20,0.78,cloudFbm)*0.82+0.26,saturate(turbulence*0.92));
        result=softVis*cloudGobo;
    }
    return result;
}
float FogNearMoteScintillation(float3 worldPos,float t,float turbulence,float windPhase)
{
    float result=0.0;
    float nearMask=(1.0-smoothstep(5.0,32.0,t))*smoothstep(0.8,3.6,t);
    if(nearMask>1e-4)
    {
        float3 drift=float3(windPhase*1.2,-windPhase*0.22,windPhase*0.8);
        float m1=FogValueNoise3D((worldPos+drift)*0.55);
        float m2=FogValueNoise3D((worldPos-drift*0.6)*1.15+float3(5.2,9.8,3.1));
        float speck=smoothstep(0.24,0.74,m1*m2*1.90);
        result=speck*nearMask*saturate(turbulence)*0.28;
    }
    return result;
}
float3 ApplyFog(float3 color,float3 nearView,float3 endView,int2 pixel)
{
    float3 segment=endView-nearView;float lengthView=length(segment);
    float3 dir=segment/max(lengthView,1e-6),worldDirection=mul((float3x3)indoorInverseView,dir);
    float rayLength=min(lengthView,strengths.w);
    float3 worldOrigin=mul(indoorInverseView,float4(nearView,1)).xyz;
    float transmittance=1.0;
    float3 scattering=0;
    const float turbulence=saturate(fogParameters.w);
    const float windPhase=fogAlbedo.w;
    const bool heightOrScatter=(fogParameters.x>1e-6)||(fogParameters.z>1e-6)||(turbulence>1e-4);
    if(!heightOrScatter)
    {
        transmittance=exp(-strengths.z*rayLength);
        if(any(sunRadiance.rgb>0))
        {
            float sunIntegral=1-transmittance;
            if(sunShadowParameters.x>0)
            {
                float ds=rayLength/16,stepT=exp(-strengths.z*ds),running=1;sunIntegral=0;
                [loop]for(uint i=0;i<16;++i)
                {
                    float t=(i+.5)*ds;
                    sunIntegral+=running*(1-stepT)*SunVisibility(worldOrigin+worldDirection*t,-(nearView.z+dir.z*t));
                    running*=stepT;
                }
            }
            scattering+=sunRadiance.rgb*Phase(dot(worldDirection,sunDirection.xyz))*sunIntegral;
        }
    }
    else
    {
        float falloff=max(fogParameters.x,0.0);
        float baseHeight=fogParameters.y;
        float ign=frac(52.9829189*frac(dot(float2(pixel),float2(0.06711056,0.00583715))));
        if(turbulence>1e-4)
        {
            // 非线性平方分布步进：近景山谷雾团、柔边斑驳丁达尔光束与逆光微尘高精度采样。
            float running=1.0;
            float sunIntegral=0.0;
            float moteIntegral=0.0;
            const bool evalSun=any(sunRadiance.rgb>0);
            [loop]for(uint i=0;i<24;++i)
            {
                float u0=float(i)/24.0,u1=float(i+1u)/24.0;
                float t0=rayLength*lerp(u0,u0*u0,0.48);
                float t1=rayLength*lerp(u1,u1*u1,0.48);
                float ds=max(t1-t0,1e-4);
                float t=lerp(t0,t1,ign);
                float3 sampleWorld=worldOrigin+worldDirection*t;
                float baseSigma=strengths.z*exp(clamp(-falloff*(sampleWorld.y-baseHeight),-12.0,4.0));
                float localSigma=baseSigma*FogTurbulentDensityFactor(sampleWorld,turbulence,windPhase);
                float stepT=exp(-localSigma*ds);
                if(evalSun)
                {
                    float vis=SoftVolumetricSunVisibility(sampleWorld,-(nearView.z+dir.z*t),t,turbulence,windPhase);
                    sunIntegral+=running*(1.0-stepT)*vis;
                    float moteStepT=exp(-localSigma*min(ds,1.25));
                    moteIntegral+=running*(1.0-moteStepT)*vis*FogNearMoteScintillation(sampleWorld,t,turbulence,windPhase);
                }
                running*=stepT;
            }
            transmittance=running;
            if(evalSun)
            {
                float cosSun=dot(worldDirection,sunDirection.xyz);
                float g=0.74,g2=g*g;
                float hgForward=(1.0-g2)/(4.0*Pi*pow(max(1.0+g2-2.0*g*cosSun,1e-3),1.5));
                float phaseVal=Phase(cosSun)+fogParameters.z*hgForward;
                scattering+=sunRadiance.rgb*(phaseVal*sunIntegral+(min(phaseVal,0.65)*0.55+0.08)*moteIntegral);
            }
            // 天光环境多重散射底光（对齐 UE5 VolumetricFog Sky Inscattering）：消除前景物体与背景阴影雾的抠图脱节感。
            if(environment.x>0)
            {
                float3 ambDir=Unit(lerp(worldDirection,float3(0,1,0),0.48),float3(0,1,0));
                float3 envDir=float3(environment.y*ambDir.x-environment.z*ambDir.z,ambDir.y,environment.z*ambDir.x+environment.y*ambDir.z);
                float3 skyAmb=sceneEnvironment.SampleLevel(indoorSampler,envDir,environment.w).rgb*environment.x;
                scattering+=skyAmb*(1.0-transmittance)*0.85;
            }
        }
        else
        {
            float tauTotal=FogOpticalDepth(strengths.z,falloff,baseHeight,worldOrigin.y,worldDirection.y,rayLength);
            transmittance=exp(-tauTotal);
            if(any(sunRadiance.rgb>0))
            {
                float cosSun=dot(worldDirection,sunDirection.xyz);
                float g=0.76,g2=g*g;
                float hgForward=(1.0-g2)/(4.0*Pi*pow(max(1.0+g2-2.0*g*cosSun,1e-3),1.5));
                float phaseVal=Phase(cosSun)+fogParameters.z*hgForward;
                float sunIntegral=1.0-transmittance;
                if(sunShadowParameters.x>0)
                {
                    float ds=rayLength/16.0;
                    float running=1.0;
                    sunIntegral=0.0;
                    [loop]for(uint i=0;i<16;++i)
                    {
                        float t=(float(i)+ign)*ds;
                        float3 sampleWorld=worldOrigin+worldDirection*t;
                        float localSigma=strengths.z*exp(clamp(-falloff*(sampleWorld.y-baseHeight),-12.0,4.0));
                        float stepT=exp(-localSigma*ds);
                        sunIntegral+=running*(1.0-stepT)*SunVisibility(sampleWorld,-(nearView.z+dir.z*t));
                        running*=stepT;
                    }
                }
                scattering+=sunRadiance.rgb*phaseVal*sunIntegral;
            }
        }
    }
    [loop]for(uint k=0;k<(uint)ordinaryLightCount.x;++k)
        scattering+=ordinaryRadiance[k].rgb*IntegrateLocalFog(worldOrigin,worldDirection,rayLength,ordinaryPosition[k].xyz,ordinaryPosition[k].w,k,false);
    [loop]for(uint j=0;j<(uint)localLightInfo.x;++j)
        scattering+=localRadianceSpot[j].rgb*IntegrateLocalFog(worldOrigin,worldDirection,rayLength,localPositionRange[j].xyz,localPositionRange[j].w,j,true);
    return color*transmittance+scattering*fogAlbedo.rgb;
}
float4 PSMain(float4 position:SV_POSITION):SV_Target
{
    int2 pixel=(int2)position.xy;float4 color=sceneRadiance.Load(int3(pixel,0));
    float depth=sceneDepth.Load(int3(pixel,0));float2 uv=PixelUv(pixel);
    float3 nearView=ViewAt(uv,0),p=ViewAt(uv,depth);
    if(depth<1&&(strengths.x>0||strengths.y>0))
    {
        float4 material=sceneMaterial.Load(int3(pixel,0));
        if(material.a>=0)
        {
            uint code=(uint)round(material.a);
            float metal=(code/32)/31.0,rough=max(.045,(code%32)/31.0);
            float3 n=ViewNormal(pixel),eye=Unit(nearView-p,float3(0,0,1));
            float3 f0=lerp(.04.xxx,material.rgb,metal);
            if(strengths.x>0&&metal<1)
                color.rgb+=strengths.x*ReconstructDiffuse(pixel,p,n)*((1-metal)*material.rgb*(1-f0));
            if(strengths.y>0)
                color.rgb+=strengths.y*ReflectedChange(p,n,eye,f0,rough);
        }
    }
    if(strengths.z>0)color.rgb=ApplyFog(max(color.rgb,0),nearView,p,pixel);
    return float4(clamp(color.rgb,0,65504),color.a);
}
