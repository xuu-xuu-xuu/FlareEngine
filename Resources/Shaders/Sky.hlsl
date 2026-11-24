cbuffer SkyConstants:register(b0)
{
    float4x4 skyFromClip;
    float4 zenithColor;
    float4 horizonColor;
    float4 groundColor;
    float4 sunDirectionIntensity; // xyz 指向太阳，w 强度
    float4 sunColor;
    float4 parameters;            // x=width, y=cosOuter, z=cosInner, w=height
    float4 cloudParameters;       // x=coverage(0..1), y=density, z=speed, w=timeSeconds
};
#include "Fullscreen.hlsli"
#ifdef SE_USER_SKY
#include "UserSkyContract.hlsli"
#endif

uint SkyHashU32(int2 p)
{
    uint2 q=uint2(p)*uint2(1597334677u,3812015801u);
    uint n=(q.x^q.y)*1597334677u;
    return n^(n>>15u);
}
float2 SkyGrad2(int2 p)
{
    uint n=SkyHashU32(p);
    float angle=float(n&65535u)*(6.283185307/65536.0);
    return float2(cos(angle),sin(angle));
}
float2 SkyRand2(int2 p)
{
    uint n=SkyHashU32(p);
    uint m=SkyHashU32(p+int2(37,91));
    return float2(float(n&65535u),float(m&65535u))*(1.0/65536.0);
}
float SkyPerlin2(float2 p)
{
    float2 fl=floor(p);
    int2 i=int2(fl);
    float2 f=p-fl;
    float2 u=f*f*f*(f*(f*6.0-15.0)+10.0);
    float n00=dot(SkyGrad2(i),f);
    float n10=dot(SkyGrad2(i+int2(1,0)),f-float2(1.0,0.0));
    float n01=dot(SkyGrad2(i+int2(0,1)),f-float2(0.0,1.0));
    float n11=dot(SkyGrad2(i+int2(1,1)),f-float2(1.0,1.0));
    return lerp(lerp(n00,n10,u.x),lerp(n01,n11,u.x),u.y)*0.72+0.5;
}
float SkyWorley2(float2 p)
{
    float2 fl=floor(p);
    int2 baseCell=int2(fl);
    float2 f=p-fl;
    float best=4.0;
    [unroll]for(int oy=-1;oy<=1;++oy)
    {
        [unroll]for(int ox=-1;ox<=1;++ox)
        {
            float2 offs=float2(float(ox),float(oy));
            float2 feature=offs+SkyRand2(baseCell+int2(ox,oy))-f;
            best=min(best,dot(feature,feature));
        }
    }
    return 1.0-saturate(sqrt(best));
}
float SkyFbmShape(float2 p)
{
    const float2x2 rot=float2x2(0.80,-0.60,0.60,0.80);
    float sum=0.0;
    float amp=0.52;
    [unroll]for(int k=0;k<4;++k)
    {
        float perlin=SkyPerlin2(p);
        float worley=SkyWorley2(p*0.85);
        sum+=amp*lerp(perlin,worley,0.38);
        p=mul(rot,p)*2.03+float2(17.3,31.7);
        amp*=0.5;
    }
    return saturate(sum);
}
float SkyFbmDetail(float2 p)
{
    const float2x2 rot=float2x2(0.80,0.60,-0.60,0.80);
    float sum=0.0;
    float amp=0.55;
    [unroll]for(int k=0;k<3;++k)
    {
        sum+=amp*SkyPerlin2(p);
        p=mul(rot,p)*2.15+float2(9.1,23.4);
        amp*=0.5;
    }
    return saturate(sum);
}
float SkyCloudDensity(float3 worldPos,float coverage,float densityScale,float2 wind)
{
    const float baseAlt=1050.0;
    const float topAlt=2950.0;
    float h=saturate((worldPos.y-baseAlt)/(topAlt-baseAlt));
    float densityOut=0.0;
    if(h>0.001&&h<0.999)
    {
        float2 uv=(worldPos.xz+wind*(1.0+0.35*h))*0.00019;
        float shape=SkyFbmShape(uv);
        float heightMask=smoothstep(0.0,0.14,h)*smoothstep(1.0,0.38,h);
        float threshold=lerp(0.76,0.22,coverage);
        float baseCloud=saturate((shape*heightMask-threshold)/max(1.0-threshold,0.12));
        if(baseCloud>0.001)
        {
            float detail=SkyFbmDetail(uv*4.3-wind*0.00025+float2(h*1.9,-h*1.3));
            float eroded=saturate(baseCloud-detail*0.30*(1.0-baseCloud*0.65));
            densityOut=eroded*eroded*densityScale*0.0032;
        }
    }
    return densityOut;
}
float SkyRaySphereTop(float3 dir,float radiusEarth,float shellHeight)
{
    float r=radiusEarth+shellHeight;
    float b=radiusEarth*dir.y;
    float c=radiusEarth*radiusEarth-r*r;
    float disc=max(b*b-c,0.0);
    return max(-b+sqrt(disc),0.0);
}
float SkyHg(float cosTheta,float g)
{
    float g2=g*g;
    float denom=max(1.0+g2-2.0*g*cosTheta,1e-3);
    return (1.0-g2)/(12.5663706*pow(denom,1.5));
}

float4 PSMain(float4 position:SV_POSITION):SV_TARGET
{
    const float2 invViewport=1.0/max(float2(parameters.x,parameters.w),1.0);
    const float2 screenUv=position.xy*invViewport;
    const float2 ndc=float2(screenUv.x*2.0-1.0,1.0-screenUv.y*2.0);
    float4 farPoint=mul(skyFromClip,float4(ndc,1.0,1.0));
    float4 nearPoint=mul(skyFromClip,float4(ndc,0.0,1.0));
    float3 direction=normalize(farPoint.xyz*nearPoint.w-nearPoint.xyz*farPoint.w);
    const float3 sunDirection=normalize(sunDirectionIntensity.xyz);
    const float intensity=sunDirectionIntensity.w;

#ifdef SE_USER_SKY
    FlareSkyInput skyIn=(FlareSkyInput)0;
    skyIn.viewDir=direction;
    skyIn.screenUv=screenUv;
    skyIn.pixelPos=position.xy;
    skyIn.sunDir=sunDirection;
    skyIn.sunIntensity=intensity;
    skyIn.sunColor=sunColor.rgb;
    skyIn.zenithColor=zenithColor.rgb;
    skyIn.horizonColor=horizonColor.rgb;
    skyIn.groundColor=groundColor.rgb;
    skyIn.cloudCoverage=cloudParameters.x;
    skyIn.cloudDensity=cloudParameters.y;
    skyIn.cloudSpeed=cloudParameters.z;
    skyIn.time=cloudParameters.w;
    float3 customSky=max(FlareSky(skyIn),0.0);
    return float4(min(customSky,65504.0),1.0);
#else
    float3 sky=direction.y>=0.0
        ?lerp(horizonColor.rgb,zenithColor.rgb,pow(saturate(direction.y),0.55))
        :lerp(horizonColor.rgb,groundColor.rgb,pow(saturate(-direction.y),0.35));
    const float cosine=saturate(dot(direction,sunDirection));
    const float disk=smoothstep(parameters.y,parameters.z,cosine);
    const float halo=pow(cosine,64.0)*0.15;
    const float3 sun=sunColor.rgb*(intensity*disk*disk+halo*intensity);
    float3 baseSky=sky+sun;

    if(cloudParameters.x<=0.0001)
        return float4(min(baseSky,65504.0),1.0);

    // 体积暴风云：仅在地平线以上进行地球曲率球壳步进，IGN 逐像素抖动彻底消除水平切片条纹。
    if(direction.y>-0.015)
    {
        float3 rayDir=normalize(float3(direction.x,max(direction.y,0.012),direction.z));
        const float radiusEarth=6360000.0;
        float tStart=SkyRaySphereTop(rayDir,radiusEarth,1050.0);
        float tEnd=min(SkyRaySphereTop(rayDir,radiusEarth,2950.0),36000.0);
        if(tEnd>tStart+1.0)
        {
            const int stepCount=18;
            float dt=(tEnd-tStart)/float(stepCount);
            float ign=frac(52.9829189*frac(dot(position.xy,float2(0.06711056,0.00583715))));
            float coverage=saturate(cloudParameters.x);
            float densityScale=max(cloudParameters.y,0.0);
            float2 wind=float2(1.0,0.42)*(cloudParameters.z*cloudParameters.w*18.0);

            float cosSun=dot(rayDir,sunDirection);
            float phase=lerp(SkyHg(cosSun,0.65),SkyHg(cosSun,-0.25),0.32)*2.6+0.22;
            float3 sunEnergy=sunColor.rgb*min(intensity,16.0)*0.32;
            float3 ambientTop=lerp(zenithColor.rgb,horizonColor.rgb,0.45)*1.15;
            float3 ambientBottom=lerp(groundColor.rgb,zenithColor.rgb,0.50)*0.55;

            float transmittance=1.0;
            float3 cloudLight=float3(0.0,0.0,0.0);
            [loop]for(int i=0;i<stepCount;++i)
            {
                if(transmittance<0.025)break;
                float t=tStart+(float(i)+ign)*dt;
                float3 samplePos=rayDir*t;
                samplePos.y=sqrt(radiusEarth*radiusEarth+t*t+2.0*radiusEarth*rayDir.y*t)-radiusEarth;
                float sigma=SkyCloudDensity(samplePos,coverage,densityScale,wind);
                if(sigma>1e-5)
                {
                    float hNorm=saturate((samplePos.y-1050.0)/1900.0);
                    float3 sunStepPos=samplePos+sunDirection*260.0;
                    float sunSigma=SkyCloudDensity(sunStepPos,coverage,densityScale,wind)*280.0;
                    float lightTrans=exp(-sunSigma*1.35);
                    float powder=1.0-0.65*exp(-sigma*dt*2.4);
                    float3 ambient=lerp(ambientBottom,ambientTop,hNorm);
                    float3 lum=ambient+sunEnergy*lightTrans*powder*phase;
                    float stepTrans=exp(-sigma*dt);
                    cloudLight+=transmittance*(1.0-stepTrans)*lum;
                    transmittance*=stepTrans;
                }
            }
            float horizonFade=smoothstep(-0.01,0.085,direction.y);
            float cloudAlpha=(1.0-transmittance)*horizonFade;
            float3 aerialCloud=lerp(horizonColor.rgb*0.92,cloudLight/max(1.0-transmittance,1e-4),horizonFade);
            baseSky=sky*(1.0-cloudAlpha)+sun*lerp(1.0,transmittance,horizonFade)+aerialCloud*cloudAlpha;
            // 低角度太阳穿过云隙的柔和大气散射辉光
            float mieGlow=pow(cosine,8.0)*0.08*min(intensity,12.0)*(1.0-cloudAlpha*0.65);
            baseSky+=sunColor.rgb*mieGlow;
        }
    }
    return float4(min(baseSky,65504.0),1.0);
#endif
}
