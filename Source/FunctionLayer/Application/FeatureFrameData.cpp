#include "FeatureFrameData.h"
#include "FrameData.h"
#include "FunctionLayer/Runtime/FeatureComponents.h"
namespace se
{
void ApplyFeatureFrameData(const World& world,FrameData& frame)
{
    if(const auto* component=world.GetComponent(SceneFeature(world,RenderSettingsComponent::TypeId)))
    {
        const auto owner=component->Owner();const auto get=[&](const char* key){return FeatureValue(world,owner,key);};
        const auto number=[&](const char* key){return std::get<float>(get(key));};
        const auto flag=[&](const char* key){return std::get<bool>(get(key));};
        frame.sceneRenderSettings=true;frame.exposure=number("render.exposure");frame.acesToneMap=flag("render.aces");frame.modelLod=flag("render.lod");frame.fullPbr=flag("render.fullPbr");
        frame.instancing=flag("render.instancing");frame.occlusion=flag("render.occlusion");frame.occlusionCounters=flag("render.occlusionCounters");frame.occlusionTiming=flag("render.occlusionTiming");frame.frameGraph=flag("render.frameGraph");
        frame.multiscatter=flag("render.multiscatter");frame.specularAA=flag("render.specularAA");
        frame.bc7Color=flag("render.bc7Color");
        frame.depthNormals=flag("render.depthNormals");frame.gtao=flag("render.gtao");frame.contactShadows=flag("render.contactShadows");
        frame.aoRadius=number("render.aoRadius");frame.aoIntensity=number("render.aoIntensity");frame.contactLength=number("render.contactLength");
        frame.diffuseSh=flag("render.diffuseSh");frame.localDiffuseSh=flag("render.localDiffuseSh");
        frame.diffuseProbeAsset=std::get<std::string>(get("render.diffuseProbe"));
        frame.gpuDriven=flag("render.gpuDriven");frame.meshletCulling=flag("render.meshletCulling");
        frame.gpuDrivenHiZ=flag("render.gpuDrivenHiZ");
        frame.clusteredLighting=flag("render.clusteredLighting");frame.clusterDebug=flag("render.clusterDebug");
        if(frame.instancing)frame.modelLod=false;
        frame.modelHlod=flag("render.hlod")&&!frame.instancing;frame.hlodRadius=number("render.hlodRadius");
        frame.frustumCulling=flag("render.frustumCulling");frame.sortOpaque=flag("render.sortOpaque");frame.cacheOpaqueBindings=flag("render.cacheBindings");
        frame.lookMode=number("render.lookMode");frame.lookBlend=number("render.lookBlend");
        frame.shadow.enabled=flag("render.shadowEnabled");frame.shadow.pcf=flag("render.shadowPcf");frame.shadow.halfExtent=number("render.shadowExtent");
        frame.shadow.lightDistance=number("render.shadowDistance");
        frame.shadow.depthBias=number("render.shadowBias");frame.shadow.normalBias=number("render.shadowNormalBias");
        frame.shadow.cascades=flag("render.cascades");frame.shadow.cascadeDebug=flag("render.cascadeDebug");
        frame.shadow.cascadeCount=static_cast<std::uint32_t>(number("render.cascadeCount"));
        frame.shadow.cascadeResolution=static_cast<std::uint32_t>(number("render.cascadeResolution"));
        frame.shadow.cascadeDistance=number("render.cascadeDistance");
        frame.shadow.cascadeDepthBias=number("render.cascadeBias");frame.shadow.cascadeSlopeBias=number("render.cascadeSlopeBias");
        frame.shadow.cascadeNormalBias=number("render.cascadeNormalBias");
        frame.shadow.filter.pcss=flag("render.shadowPcss");
        frame.shadow.filter.angularRadius=number("render.shadowAngularRadius");
        frame.shadow.filter.blockerSearchDistance=number("render.shadowSearchDistance");
        frame.shadow.filter.maximumRadiusTexels=number("render.shadowFilterRadius");
        frame.shadow.filter.debug=static_cast<rhi::ShadowFilterDebugMode>(static_cast<std::uint32_t>(number("render.shadowFilterDebug")));
        if(flag("render.post"))
        {
            rhi::PostProcessSettings settings;settings.bloom=flag("render.bloom");settings.bloomThreshold=number("render.bloomThreshold");settings.bloomStrength=number("render.bloomStrength");
            settings.fxaa=flag("render.fxaa");settings.taa=flag("render.taa");settings.supersample4x=flag("render.supersample4x");settings.autoExposure=flag("render.autoExposure");settings.lutEnabled=flag("render.lutEnabled");
            const auto lift=std::get<Vec3>(get("render.lutLift")),gamma=std::get<Vec3>(get("render.lutGamma")),gain=std::get<Vec3>(get("render.lutGain"));
            const float l[]{lift.x,lift.y,lift.z},g[]{gamma.x,gamma.y,gamma.z},n[]{gain.x,gain.y,gain.z};
            for(int i=0;i<3;++i){settings.lutLift[i]=l[i];settings.lutGamma[i]=g[i];settings.lutGain[i]=n[i];}
            frame.post=settings;
        }
    }
    if(const auto* component=world.GetComponent(SceneFeature(world,EnvironmentSettingsComponent::TypeId)))
    {
        const auto owner=component->Owner();frame.environment.asset=std::get<std::string>(FeatureValue(world,owner,"environmentmap.asset"));
        frame.environment.intensity=std::get<float>(FeatureValue(world,owner,"environmentmap.intensity"));
        frame.environment.yaw=std::get<float>(FeatureValue(world,owner,"environmentmap.yaw"))*Pi/180.f;
    }
}
}
