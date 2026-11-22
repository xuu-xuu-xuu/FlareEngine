#include "PbrBindings.hlsli"
#include "GeometryCoverage.hlsli"
#ifdef SE_MATERIAL_SURFACE
#include "MaterialCoverage.hlsli"
#endif
#ifdef SE_SHADING_EXTENSIONS
#include "ShadingBindings.hlsli"
#include "LocalLightBindings.hlsli"
#define SURFACE_DIRECT(L) SurfaceDirectBrdf(color,metal,rough,n,geometricNormal,v,L,input.tangent.xyz,input.tangent.w*materialParameters.w,logicalFront?1:-1,input.uv,materialUvDx,materialUvDy,surfaceCurvature)
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
    output.uv=input.uv;return output;
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
    float coverageAlpha=MaterialCoverage(input.uv,logicalFront);materialBaseColor=surfaceBaseColorFactor;
#endif
    if(!logicalFront&&materialOptions.z<0.5)discard;
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
    float2 mr=metallicRoughnessMap.Sample(materialSampler,input.uv).gb;
    float metal=saturate(materialParameters.x*mr.y),rough=max(0.045,saturate(materialParameters.y*mr.x));
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
    lit+=EnvironmentBrdf(color,metal,rough,n,v,input.worldPosition,geometricNormal,input.tangent.xyz,input.tangent.w*materialParameters.w,logicalFront?1:-1,input.uv)*screenVisibility.x;
#else
    lit+=EnvironmentBrdf(color,metal,rough,n,v);
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
    float3 shaded=materialOptions.y>0.5?color:ApplyLook(lit,color,n,v,lookNdl,lookVisibility);
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
    float3 shaded=materialOptions.y>0.5?color:ApplyLook(lit,color,n,v,saturate(dot(n,l)),1.0);
    return float4(OutputSrgb(shaded),1);
#endif
}
