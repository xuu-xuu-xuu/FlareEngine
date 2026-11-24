#include "PbrBindings.hlsli"
#include "GeometryCoverage.hlsli"
#ifdef SE_MATERIAL_SURFACE
#include "MaterialCoverage.hlsli"
#endif
#ifdef SE_SHADING_EXTENSIONS
#include "ShadingBindings.hlsli"
#include "LocalLightBindings.hlsli"
#define SURFACE_DIRECT(L) SurfaceDirectBrdfEx(color,metal,rough,n,geometricNormal,v,L,input.tangent.xyz,input.tangent.w*materialParameters.w,logicalFront?1:-1,input.uv,materialUvDx,materialUvDy,surfaceCurvature,specularLevel)
#else
#define SURFACE_DIRECT(L) DirectBrdf(color,metal,rough,n,v,L)
#endif
#ifdef SE_INSTANCED
#include "InstancingBindings.hlsli"
#endif
#ifdef SE_FORWARD
#include "ForwardBindings.hlsli"
#ifdef SE_CLUSTERED
#include "ClusteredBindings.hlsli"
#endif
#ifdef SE_SHADOWS
#include "ShadowBindings.hlsli"
#endif
#ifdef SE_ENVIRONMENT
#include "EnvironmentBindings.hlsli"
#endif
#endif
#if defined(SE_MATERIAL_SURFACE) && defined(SE_USER_SURFACE)
#include "UserSurfaceContract.hlsli"
#endif
#ifdef SE_VERTEX_COMPRESSED
struct VertexInput {float4 position:POSITION;float2 normal:NORMAL;float2 uv:TEXCOORD0;float4 tangent:TANGENT;
#ifdef SE_SKINNED
    uint4 joints:JOINTS0;float4 weights:WEIGHTS0;
#endif
};
float3 DecodeOctahedral(float2 encoded)
{
    float2 e=encoded*2.0-1.0;
    float3 n=float3(e,1.0-abs(e.x)-abs(e.y));
    if(n.z<0)n.xy=(1.0-abs(n.yx))*float2(e.x<0?-1.0:1.0,e.y<0?-1.0:1.0);
    return normalize(n);
}
#else
struct VertexInput {float3 position:POSITION;float3 normal:NORMAL;float2 uv:TEXCOORD0;float4 tangent:TANGENT;
#ifdef SE_SKINNED
    uint4 joints:JOINTS0;float4 weights:WEIGHTS0;
#endif
};
#endif
// W03：蒙皮调色板（t8）。只有蒙皮变体需要，声明放在绑定之后以保持寄存器顺序。
#ifdef SE_SKINNED
#include "Skinning.hlsli"
#endif
struct PixelInput
{
    float4 position:SV_POSITION;
    float3 worldPosition:TEXCOORD1;
    float3 normal:NORMAL;
    float2 uv:TEXCOORD0;
    float4 tangent:TANGENT;
#ifdef SE_INSTANCED
    nointerpolation uint instanceIndex:TEXCOORD2;
#endif
#ifdef SE_USER_SURFACE
    float3 localPosition:TEXCOORD3;
#endif
};
PixelInput VSMain(VertexInput input
#ifdef SE_INSTANCED
    ,uint instanceIndex:SV_InstanceID
#endif
)
{
    PixelInput output;
#ifdef SE_VERTEX_COMPRESSED
    float3 localPosition=input.position.xyz;
    float3 localNormal=DecodeOctahedral(input.normal);
    float4 localTangent=float4(input.tangent.xyz*2.0-1.0,input.tangent.w*2.0-1.0);
#else
    float3 localPosition=input.position;
    float3 localNormal=input.normal;
    float4 localTangent=input.tangent;
#endif
    // W03：先在实体局部空间蒙皮（调色板 = jointWorld * inverseBind），再走原有的
    // clip/world/normal 变换；这样静态与蒙皮共用同一套世界/相机矩阵语义。
#ifdef SE_SKINNED
    const float4x4 skinFromSkinned=WeightedSkinMatrix((uint)max(0.0,skin.x),input.joints,input.weights);
    localPosition=SkinPosition(skinFromSkinned,localPosition);
    localNormal=SkinDirection(skinFromSkinned,localNormal);
    localTangent=float4(SkinDirection(skinFromSkinned,localTangent.xyz),localTangent.w);
#endif
#if defined(SE_USER_SURFACE) && defined(FLARE_HAS_VERTEX)
    FlareVertexInput userV;
    userV.localPos=localPosition;
    userV.localNormal=localNormal;
    userV.uv=input.uv;
    userV.time=flareTime;
    FlareVertex(userV);
    localPosition=userV.localPos;
    localNormal=userV.localNormal;
#endif
#ifdef SE_INSTANCED
    uint start=instanceIndex*instanceStrideBytes;
    float4x4 instanceClip=InstanceMatrix(start),instanceWorld=InstanceMatrix(start+64),instanceNormal=InstanceMatrix(start+128);
    output.position=mul(instanceClip,float4(localPosition,1));
    output.worldPosition=mul(instanceWorld,float4(localPosition,1)).xyz;
    output.normal=mul(instanceNormal,float4(localNormal,0)).xyz;
    output.tangent=float4(mul(instanceWorld,float4(localTangent.xyz,0)).xyz,localTangent.w);
    output.instanceIndex=instanceIndex;
#else
    output.position=mul(clipFromLocal,float4(localPosition,1));
    output.worldPosition=mul(worldFromLocal,float4(localPosition,1)).xyz;
    output.normal=mul(normalFromLocal,float4(localNormal,0)).xyz;
    output.tangent=float4(mul(worldFromLocal,float4(localTangent.xyz,0)).xyz,localTangent.w);
#endif
    output.uv=input.uv;
#ifdef SE_USER_SURFACE
    output.localPosition=localPosition;
#endif
    return output;
}
float4 PSMain(PixelInput input,bool frontFace:SV_IsFrontFace):SV_TARGET
{
#ifdef SE_INSTANCED
    uint start=input.instanceIndex*instanceStrideBytes;
    float4 materialBaseColor=asfloat(instanceData.Load4(start+192)),materialParameters=asfloat(instanceData.Load4(start+208)),materialOptions=asfloat(instanceData.Load4(start+224));
#else
    float4 materialBaseColor=baseColor,materialParameters=parameters,materialOptions=options;
#endif
    // G05.08：LOD 过渡像素 dither。|options.w|<1 时按 4×4 Bayer 阈值二分：
    // 负值绘制旧级（丢弃“已切到新级”的像素），正值绘制新级（丢弃“仍保留旧级”的像素），两组像素互不重叠。
    ApplyLodCoverage(materialOptions.w,input.position.xy);
    bool logicalFront=frontFace==(materialParameters.w>0);
#ifdef SE_MATERIAL_SURFACE
#ifdef SE_USER_SURFACE
    if(!logicalFront&&surfaceCoverage.z<.5)discard;
    if(logicalFront&&surfaceCoverage.z>1.5)discard;
    float coverageAlpha=1.0;materialBaseColor=surfaceBaseColorFactor;
#else
    float coverageAlpha=MaterialCoverage(input.uv,logicalFront);materialBaseColor=surfaceBaseColorFactor;
#endif
#endif
    if(!logicalFront&&materialOptions.z<0.5)discard;
    if(logicalFront&&materialOptions.z>1.5)discard;
    float3 color=baseColorMap.Sample(materialSampler,input.uv).rgb*materialBaseColor.rgb;
    float3 n=UnitVector(input.normal,float3(0,0,1));
    float3 geometricNormal=n;
    if(materialOptions.x>0.5&&abs(materialParameters.z)>1e-8)
    {
        float3 fallback=abs(n.y)<0.99?UnitVector(cross(float3(0,1,0),n),float3(1,0,0)):UnitVector(cross(float3(1,0,0),n),float3(0,0,1));
        float3 t=UnitVector(input.tangent.xyz-n*dot(n,input.tangent.xyz),fallback);
        float3 b=cross(n,t)*input.tangent.w*materialParameters.w;
#ifdef SE_NORMAL_BC5
        // BC5 只存法线 XY，Z 由单位向量约束重建；与 RGBA8/BC1 的 xyz 路径等价。
        float2 mappedXY=normalMap.Sample(materialSampler,input.uv).rg*2-1;
        float3 mapped=float3(mappedXY,sqrt(saturate(1-dot(mappedXY,mappedXY))));
#else
        float3 mapped=normalMap.Sample(materialSampler,input.uv).xyz*2-1;
#endif
        mapped.xy*=materialParameters.z;mapped=UnitVector(mapped,float3(0,0,1));
        n=UnitVector(t*mapped.x+b*mapped.y+n*mapped.z,n);
    }
    if(!logicalFront){n=-n;geometricNormal=-geometricNormal;}
    float3 orm=metallicRoughnessMap.Sample(materialSampler,input.uv).rgb;
    float2 mr=orm.gb;
    float metal=saturate(materialParameters.x*mr.y),rough=max(0.045,saturate(materialParameters.y*mr.x));
    float materialAo=1.0,specularLevel=0.5,translucency=0.0;
#ifdef SE_SHADING_EXTENSIONS
    if(layerCoat.w>0.0)materialAo=lerp(1.0,orm.r,saturate(layerCoat.w));
    specularLevel=saturate(0.5*(1.0+layerAnisotropy.z));
#endif
    if(skin.w>0.5)
    {
        // skin.w 是每 Draw 常量。导数须在逐实例 normalMapping 条件外计算，
        // 不能移入 varying 分支，否则同 quad 的辅助像素可能没有有效导数。
        float3 normalDx=ddx(n),normalDy=ddy(n);
        uint normalWidth,normalHeight;normalMap.GetDimensions(normalWidth,normalHeight);
        float2 textureSize=float2(normalWidth,normalHeight);
        float footprint=max(length(ddx(input.uv)*textureSize),length(ddy(input.uv)*textureSize));
        if(materialOptions.x>0.5&&abs(materialParameters.z)>1e-8&&footprint>1.0)
            rough=FilterSpecularRoughness(rough,normalDx,normalDy,footprint);
    }
    float3 v=UnitVector(cameraPosition.xyz-input.worldPosition,float3(0,0,1));
#ifdef SE_USER_SURFACE
    float3 userEmissive=float3(0,0,0);
    {
        float4 baseSample=baseColorMap.Sample(materialSampler,input.uv);
        float3 defaultNormalTS=float3(0,0,1);
        if(materialOptions.x>0.5&&abs(materialParameters.z)>1e-8)
        {
            defaultNormalTS=SampleNormalTS(input.uv);
            defaultNormalTS.xy*=materialParameters.z;
            defaultNormalTS=UnitVector(defaultNormalTS,float3(0,0,1));
        }
        float3 baseGeoN=UnitVector(input.normal,float3(0,0,1));
        float3 fallbackAxis=abs(baseGeoN.y)<0.99?UnitVector(cross(float3(0,1,0),baseGeoN),float3(1,0,0)):UnitVector(cross(float3(1,0,0),baseGeoN),float3(0,0,1));
        float3 tBasis=UnitVector(input.tangent.xyz-baseGeoN*dot(baseGeoN,input.tangent.xyz),fallbackAxis);
        float3 bBasis=cross(baseGeoN,tBasis)*input.tangent.w*materialParameters.w;
#ifdef SE_INSTANCED
        float3 objOrigin=mul(InstanceMatrix(input.instanceIndex*instanceStrideBytes+64),float4(0,0,0,1)).xyz;
#else
        float3 objOrigin=mul(worldFromLocal,float4(0,0,0,1)).xyz;
#endif
        FlareSurfaceInput userIn=(FlareSurfaceInput)0;
        userIn.uv=input.uv;
        userIn.worldPos=input.worldPosition;
        userIn.localPos=input.localPosition;
        userIn.objectPos=objOrigin;
        userIn.cameraPos=cameraPosition.xyz;
        userIn.worldNormal=geometricNormal;
        userIn.worldTangent=tBasis;
        userIn.worldBitangent=bBasis;
        userIn.viewDir=v;
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
        userS.baseColor=color;
        userS.metallic=metal;
        userS.roughness=rough;
        userS.normalTS=defaultNormalTS;
        userS.emissive=float3(0,0,0);
        userS.opacity=baseSample.a*surfaceBaseColorFactor.a;
        userS.alphaCutoff=surfaceCoverage.x;
        userS.occlusion=materialAo;
        userS.specular=specularLevel;
        userS.translucency=(FLARE_SHADING_MODE==3)?0.45:0.0;

        FlareSurface(userIn,userS);

        if(surfaceCoverage.y>0.5&&surfaceCoverage.y<1.5)
            clip(userS.opacity-userS.alphaCutoff);
        coverageAlpha=saturate(userS.opacity);
        color=max(userS.baseColor,0.0);
        metal=saturate(userS.metallic);
        rough=max(0.045,saturate(userS.roughness));
        userEmissive=max(userS.emissive,0.0);
        materialAo=saturate(userS.occlusion);
        specularLevel=saturate(userS.specular);
        translucency=saturate(userS.translucency);

        float3 tsN=UnitVector(userS.normalTS,float3(0,0,1));
        n=UnitVector(tBasis*tsN.x+bBasis*tsN.y+baseGeoN*tsN.z,baseGeoN);
        if(!logicalFront)n=-n;
        if(skin.w>0.5)
        {
            float3 userNormalDx=ddx(n),userNormalDy=ddy(n);
            float saaStrength=lerp(0.18,1.55,smoothstep(0.035,0.20,rough));
            rough=FilterSpecularRoughness(rough,userNormalDx,userNormalDy,saaStrength);
        }
    }
#if defined(SE_FORWARD) && (FLARE_SHADING_MODE == 1)
    {
        float3 unlitOut=min(max(color+userEmissive,0),65504);
#ifdef SE_MATERIAL_SURFACE
        return float4(unlitOut,MaterialOutputAlpha(coverageAlpha));
#else
        return float4(unlitOut,1);
#endif
    }
#endif
#endif
#ifdef SE_SHADING_EXTENSIONS
    float2 materialUvDx=ddx(input.uv),materialUvDy=ddy(input.uv);
    float3 receiverPlane=UnitVector(cross(ddx(input.worldPosition),ddy(input.worldPosition)),geometricNormal);
    float surfaceCurvature=max(length(ddx(geometricNormal))/max(length(ddx(input.worldPosition)),1e-5),
        length(ddy(geometricNormal))/max(length(ddy(input.worldPosition)),1e-5));
    bool blended=false;
#ifdef SE_MATERIAL_SURFACE
    blended=surfaceCoverage.y>1.5;
#endif
    float2 screenVisibility=ScreenOcclusion(input.position.xy,blended);
#endif
    float3 l=UnitVector(directionToLight.xyz,float3(0,0,1));
#ifdef SE_FORWARD
    float3 lit=float3(0,0,0);
    float lookNdl=0,lookVisibility=1;
    if(frameDirection.w>0.5)
    {
        float3 direction=UnitVector(frameDirection.xyz,float3(0,0,1));
        float visibility=1;
#ifdef SE_SHADOWS
        visibility=ShadowVisibility(input.worldPosition,geometricNormal,direction,input.position.xy);
#endif
        lookNdl=saturate(dot(n,direction));lookVisibility=visibility;
#ifdef SE_SHADING_EXTENSIONS
        if(materialOptions.w!=0)visibility*=screenVisibility.y;
#endif
        lit+=SURFACE_DIRECT(direction)*frameRadiance.rgb*visibility;
        if(translucency>1e-4)
        {
            float ndlSigned=dot(n,direction);
            float wrapFill=max(0.0,saturate((ndlSigned+0.32)/1.32)-saturate(ndlSigned))*0.35;
            float3 transDir=UnitVector(direction+n*0.35,direction);
            float backScatter=pow(saturate(dot(-v,transDir)),3.0)*saturate(0.35-ndlSigned);
            lit+=color*(1.0-metal)*(wrapFill+backScatter*0.85)*translucency*frameRadiance.rgb*visibility;
        }
    }
#ifdef SE_CLUSTERED
    uint cluster=PixelCluster(input.position.xy,input.worldPosition);
    bool useGlobal=clusterCounters[6]!=0&&clusterModes.y<.5;
    if(!useGlobal)
    {
        uint2 range=clusterRanges[cluster];uint count=clusterModes.y>.5?(uint)clusterParameters.z:range.y;
        [loop]for(uint item=0;item<count;++item)
        {
            uint index=clusterModes.y>.5?item:clusterIndices[range.x+item];
            ClusterPointLight light=clusterLights[index];
            float3 delta=light.positionRange.xyz-input.worldPosition;float squared=dot(delta,delta);
            float radius=light.positionRange.w;
            float ratio=squared/(radius*radius),window=saturate(1-ratio*ratio);
            float attenuation=window*window/max(squared,0.0025);
            lit+=SURFACE_DIRECT(UnitVector(delta,n))*light.radiancePad.rgb*attenuation;
        }
    }
    else
#endif
    [loop] for(uint index=0;index<(uint)framePointCount.x;++index)
    {
        float3 delta=pointPositionRadius[index].xyz-input.worldPosition;float squared=dot(delta,delta);
        float radius=pointPositionRadius[index].w;
        float ratio=squared/(radius*radius),window=saturate(1-ratio*ratio);
        float attenuation=window*window/max(squared,0.0025);
        lit+=SURFACE_DIRECT(UnitVector(delta,n))*pointRadiance[index].rgb*attenuation;
    }
#ifdef SE_ENVIRONMENT
#ifdef SE_SHADING_EXTENSIONS
    lit+=EnvironmentBrdfEx(color,metal,rough,n,v,input.worldPosition,geometricNormal,input.tangent.xyz,input.tangent.w*materialParameters.w,logicalFront?1:-1,input.uv,materialAo,specularLevel)*screenVisibility.x;
#else
    lit+=EnvironmentBrdfEx(color,metal,rough,n,v,materialAo,specularLevel);
#endif
#endif
#ifdef SE_SHADING_EXTENSIONS
    [loop]for(uint localIndex=0;localIndex<(uint)localLightInfo.x;++localIndex)
    {
        float3 toLight;
        float attenuation=LocalLightAttenuation(localIndex,input.worldPosition,receiverPlane,materialOptions.w!=0,toLight);
        lit+=SURFACE_DIRECT(toLight)*localRadianceSpot[localIndex].rgb*attenuation;
    }
    lit+=RectangleLighting(color,metal,rough,input.worldPosition,n,geometricNormal,v,input.tangent.xyz,
        input.tangent.w*materialParameters.w,logicalFront?1:-1,input.uv,materialUvDx,materialUvDy,surfaceCurvature);
#endif
#ifdef SE_USER_SURFACE
#if FLARE_SHADING_MODE == 1
    float3 shaded=color+userEmissive;
#elif FLARE_SHADING_MODE == 2
    float toonShaped=saturate(lookNdl)*saturate(lookVisibility);
    float toonLevel=toonShaped>=0.55?1.0:(toonShaped>=0.16?0.55:0.0);
    float3 toonBase=lerp(color*float3(0.58,0.56,0.66),color,toonLevel);
    float3 shaded=(materialOptions.y>0.5?color:lerp(lit,toonBase,0.82))+userEmissive;
#else
    float3 shaded=(materialOptions.y>0.5?color:ApplyLook(lit,color,n,v,lookNdl,lookVisibility))+userEmissive;
#endif
#else
    float3 shaded=materialOptions.y>0.5?color:ApplyLook(lit,color,n,v,lookNdl,lookVisibility);
#endif
#ifdef SE_CASCADED
    if(csmOptions.z>.5)shaded=CascadeDebugColor(input.worldPosition);
#endif
#ifdef SE_CLUSTERED
    if(clusterModes.x>.5)shaded=ClusterHeatmap(cluster);
#endif
#ifdef SE_PCSS
    if(filterParameters.w>.5&&filterOptions.x>.5)shaded=ShadowFilterDebugColor(input.worldPosition,geometricNormal,UnitVector(frameDirection.xyz,float3(0,0,1)),input.position.xy);
#endif
#ifdef SE_MATERIAL_SURFACE
    return float4(min(max(shaded,0),65504),MaterialOutputAlpha(coverageAlpha));
#else
    return float4(min(max(shaded,0),65504),1);
#endif
#else
    float3 lit=SURFACE_DIRECT(l)*radiance.rgb;
#ifdef SE_USER_SURFACE
#if FLARE_SHADING_MODE == 1
    float3 shaded=color+userEmissive;
#elif FLARE_SHADING_MODE == 2
    float toonShaped=saturate(dot(n,l));
    float toonLevel=toonShaped>=0.55?1.0:(toonShaped>=0.16?0.55:0.0);
    float3 shaded=(materialOptions.y>0.5?color:lerp(lit,lerp(color*float3(0.58,0.56,0.66),color,toonLevel),0.82))+userEmissive;
#else
    float3 shaded=(materialOptions.y>0.5?color:ApplyLook(lit,color,n,v,saturate(dot(n,l)),1.0))+userEmissive;
#endif
#else
    float3 shaded=materialOptions.y>0.5?color:ApplyLook(lit,color,n,v,saturate(dot(n,l)),1.0);
#endif
    return float4(OutputSrgb(shaded),1);
#endif
}
