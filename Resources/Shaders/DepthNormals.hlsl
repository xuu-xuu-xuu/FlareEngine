#define SE_DEPTH_NORMALS 1
#include "PbrBindings.hlsli"
#include "GeometryCoverage.hlsli"
#ifdef SE_MATERIAL_SURFACE
#include "MaterialCoverage.hlsli"
#if defined(SE_USER_SURFACE) && !defined(SE_DEPTH_NORMAL_BASIC)
#ifdef SE_FORWARD
#include "ForwardBindings.hlsli"
#endif
#ifdef SE_SHADING_EXTENSIONS
#include "ShadingBindings.hlsli"
#endif
#include "UserSurfaceContract.hlsli"
#endif
#endif
struct VertexInput {float3 position:POSITION;float3 normal:NORMAL;float2 uv:TEXCOORD0;float4 tangent:TANGENT;};
struct PixelInput
{
    float4 position:SV_POSITION;
    float3 worldPosition:TEXCOORD1;
    float3 normal:NORMAL;
    float2 uv:TEXCOORD0;
    float4 tangent:TANGENT;
#if defined(SE_MATERIAL_SURFACE) && defined(SE_USER_SURFACE) && !defined(SE_DEPTH_NORMAL_BASIC)
    float3 localPosition:TEXCOORD3;
#endif
};
// 只供双阶段 artifact 的接口反射；真实预绘 PSO 始终使用实际主 Pass 的 VS 字节码。
PixelInput VSMain(VertexInput input)
{
    PixelInput output=(PixelInput)0;
    output.position=mul(clipFromLocal,float4(input.position,1));
    output.worldPosition=mul(worldFromLocal,float4(input.position,1)).xyz;
    output.normal=mul(normalFromLocal,float4(input.normal,0)).xyz;
    output.uv=input.uv;output.tangent=input.tangent;
#if defined(SE_MATERIAL_SURFACE) && defined(SE_USER_SURFACE) && !defined(SE_DEPTH_NORMAL_BASIC)
    output.localPosition=input.position;
#endif
    return output;
}
float2 EncodeOctahedralNormal(float3 n)
{
    n/=abs(n.x)+abs(n.y)+abs(n.z);
    float2 encoded=n.xy;
    if(n.z<0)encoded=(1-abs(encoded.yx))*float2(encoded.x<0?-1:1,encoded.y<0?-1:1);
    return encoded;
}
void EvaluateDepthNormalSurface(PixelInput input,bool frontFace,bool evaluateMaterial,out float2 outOctNormal,out float4 outMaterial)
{
#ifdef SE_DEPTH_NORMAL_BASIC
    if(!isfinite(input.tangent.w))discard;
    const float4 normalSample=normalMap.Sample(materialSampler,input.uv);
    if(!isfinite(normalSample.x))discard;
#ifndef SE_MATERIAL_SURFACE
    bool logicalFront=frontFace;
#else
    bool logicalFront=frontFace==(parameters.w>0);
#endif
    #ifdef SE_MATERIAL_SURFACE
    MaterialCoverage(input.uv,logicalFront);
    #endif
    if(!logicalFront&&options.z<.5)discard;
    if(logicalFront&&options.z>1.5)discard;
    float3 n=UnitVector(input.normal,float3(0,0,1));if(!logicalFront)n=-n;
    outOctNormal=EncodeOctahedralNormal(n);
    outMaterial=0;
    if(evaluateMaterial)
    {
        float2 mr=saturate(metallicRoughnessMap.Sample(materialSampler,input.uv).bg*parameters.xy);
#ifdef SE_MATERIAL_SURFACE
        float3 factor=surfaceBaseColorFactor.rgb;
#else
        float3 factor=baseColor.rgb;
#endif
        float materialCode=round(mr.x*31)*32+round(mr.y*31);
        outMaterial=float4(baseColorMap.Sample(materialSampler,input.uv).rgb*factor,options.y>.5?-1:materialCode);
    }
#elif defined(SE_MATERIAL_SURFACE) && defined(SE_USER_SURFACE)
    ApplyLodCoverage(options.w,input.position.xy);
    bool logicalFront=frontFace==(parameters.w>0);
    if(!logicalFront&&surfaceCoverage.z<.5)discard;
    if(logicalFront&&surfaceCoverage.z>1.5)discard;
    if(!logicalFront&&options.z<.5)discard;
    if(logicalFront&&options.z>1.5)discard;
    float4 baseSample=baseColorMap.Sample(materialSampler,input.uv);
    float3 color=baseSample.rgb*surfaceBaseColorFactor.rgb;
    float2 mrSample=metallicRoughnessMap.Sample(materialSampler,input.uv).gb;
    float metal=saturate(parameters.x*mrSample.y),rough=max(0.045,saturate(parameters.y*mrSample.x));
    float3 defaultNormalTS=float3(0,0,1);
    if(options.x>0.5&&abs(parameters.z)>1e-8)
    {
        defaultNormalTS=SampleNormalTS(input.uv);
        defaultNormalTS.xy*=parameters.z;
        defaultNormalTS=UnitVector(defaultNormalTS,float3(0,0,1));
    }
    float3 baseGeoN=UnitVector(input.normal,float3(0,0,1));
    float3 geometricNormal=logicalFront?baseGeoN:-baseGeoN;
    float3 fallbackAxis=abs(baseGeoN.y)<0.99?UnitVector(cross(float3(0,1,0),baseGeoN),float3(1,0,0)):UnitVector(cross(float3(1,0,0),baseGeoN),float3(0,0,1));
    float3 tBasis=UnitVector(input.tangent.xyz-baseGeoN*dot(baseGeoN,input.tangent.xyz),fallbackAxis);
    float3 bBasis=cross(baseGeoN,tBasis)*input.tangent.w*parameters.w;
    FlareSurfaceInput userIn=(FlareSurfaceInput)0;
    userIn.uv=input.uv;
    userIn.worldPos=input.worldPosition;
    userIn.localPos=input.localPosition;
    userIn.objectPos=mul(worldFromLocal,float4(0,0,0,1)).xyz;
    userIn.cameraPos=cameraPosition.xyz;
    userIn.worldNormal=geometricNormal;
    userIn.worldTangent=tBasis;
    userIn.worldBitangent=bBasis;
    userIn.viewDir=UnitVector(cameraPosition.xyz-input.worldPosition,float3(0,0,1));
#ifdef SE_FORWARD
    userIn.lightDir=frameDirection.w>0.5?UnitVector(frameDirection.xyz,float3(0.35,0.85,0.38)):float3(0.35,0.85,0.38);
    userIn.lightColor=frameDirection.w>0.5?max(frameRadiance.rgb,0.0):float3(0.0,0.0,0.0);
#else
    userIn.lightDir=UnitVector(directionToLight.xyz,float3(0.35,0.85,0.38));
    userIn.lightColor=max(radiance.rgb,0.0);
#endif
    userIn.vertexColor=float4(1,1,1,1);
#ifdef SE_SHADING_EXTENSIONS
    userIn.screenUv=saturate(input.position.xy/max(shadingViewport.xy,1.0));
#else
    userIn.screenUv=frac(input.position.xy/float2(1280.0,720.0));
#endif
    userIn.screenPos=userIn.screenUv;
    userIn.pixelPos=input.position.xy;
    userIn.facing=logicalFront?1.0:-1.0;
    userIn.time=flareTime;
    FlareSurfaceOutput userS=(FlareSurfaceOutput)0;
    userS.baseColor=color;userS.metallic=metal;userS.roughness=rough;
    userS.normalTS=defaultNormalTS;userS.emissive=0;
    userS.opacity=baseSample.a*surfaceBaseColorFactor.a;userS.alphaCutoff=surfaceCoverage.x;
    userS.occlusion=1.0;userS.specular=0.5;userS.translucency=(FLARE_SHADING_MODE==3)?0.45:0.0;
    FlareSurface(userIn,userS);
    if(surfaceCoverage.y>0.5&&surfaceCoverage.y<1.5)clip(userS.opacity-userS.alphaCutoff);
    color=max(userS.baseColor,0.0);
    metal=saturate(userS.metallic);
    rough=max(0.045,saturate(userS.roughness));
    float3 tsN=UnitVector(userS.normalTS,float3(0,0,1));
    float3 n=UnitVector(tBasis*tsN.x+bBasis*tsN.y+baseGeoN*tsN.z,baseGeoN);
    if(!logicalFront)n=-n;
    if(evaluateMaterial&&skin.w>0.5)
    {
        float3 userNormalDx=ddx(n),userNormalDy=ddy(n);
        rough=FilterSpecularRoughness(rough,userNormalDx,userNormalDy,2.0);
    }
    outOctNormal=EncodeOctahedralNormal(n);
    bool isUnlit=(options.y>0.5)||(FLARE_SHADING_MODE==1);
    float materialCode=round(metal*31)*32+round(rough*31);
    outMaterial=float4(color,isUnlit?-1:materialCode);
#else
    ApplyLodCoverage(options.w,input.position.xy);
    bool logicalFront=frontFace==(parameters.w>0);
#ifdef SE_MATERIAL_SURFACE
    MaterialCoverage(input.uv,logicalFront);
#endif
    if(!logicalFront&&options.z<.5)discard;
    if(logicalFront&&options.z>1.5)discard;
    float3 n=UnitVector(input.normal,float3(0,0,1));
    if(options.x>0.5&&abs(parameters.z)>1e-8)
    {
        float3 fallback=abs(n.y)<0.99?UnitVector(cross(float3(0,1,0),n),float3(1,0,0)):UnitVector(cross(float3(1,0,0),n),float3(0,0,1));
        float3 t=UnitVector(input.tangent.xyz-n*dot(n,input.tangent.xyz),fallback);
        float3 b=cross(n,t)*input.tangent.w*parameters.w;
        float4 ns=normalMap.Sample(materialSampler,input.uv);
        float2 nxy=ns.rg*2-1;
        float3 mapped=ns.b>0.004?(ns.xyz*2-1):float3(nxy,sqrt(saturate(1-dot(nxy,nxy))));
        mapped.xy*=parameters.z;mapped=UnitVector(mapped,float3(0,0,1));
        n=UnitVector(t*mapped.x+b*mapped.y+n*mapped.z,n);
    }
    if(!logicalFront)n=-n;
    outOctNormal=EncodeOctahedralNormal(n);
    outMaterial=0;
    if(evaluateMaterial)
    {
        float2 mr=saturate(metallicRoughnessMap.Sample(materialSampler,input.uv).bg*parameters.xy);
        float metal=mr.x,rough=max(0.045,mr.y);
        if(skin.w>0.5)
        {
            float3 normalDx=ddx(n),normalDy=ddy(n);
            uint normalWidth,normalHeight;normalMap.GetDimensions(normalWidth,normalHeight);
            float2 textureSize=float2(normalWidth,normalHeight);
            float footprint=max(length(ddx(input.uv)*textureSize),length(ddy(input.uv)*textureSize));
            if(options.x>0.5&&abs(parameters.z)>1e-8&&footprint>1.0)
                rough=FilterSpecularRoughness(rough,normalDx,normalDy,footprint);
        }
#ifdef SE_MATERIAL_SURFACE
        float3 factor=surfaceBaseColorFactor.rgb;
#else
        float3 factor=baseColor.rgb;
#endif
        // alpha 内的 0..1023 在 FP16 中完全精确。-1 表示自发光着色，不接收漫反射或镜面合成。
        float materialCode=round(metal*31)*32+round(rough*31);
        outMaterial=float4(baseColorMap.Sample(materialSampler,input.uv).rgb*factor,options.y>.5?-1:materialCode);
    }
#endif
}
float2 PSMain(PixelInput input,bool frontFace:SV_IsFrontFace):SV_TARGET
{
    float2 octNormal;float4 unusedMaterial;
    EvaluateDepthNormalSurface(input,frontFace,false,octNormal,unusedMaterial);
    return octNormal;
}
struct IndoorSurface {float2 normal:SV_Target0;float4 material:SV_Target1;};
IndoorSurface PSIndoor(PixelInput input,bool frontFace:SV_IsFrontFace)
{
    IndoorSurface output;
    EvaluateDepthNormalSurface(input,frontFace,true,output.normal,output.material);
    return output;
}
