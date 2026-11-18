#include "FeatureFrameData.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include "FunctionLayer/Particles/SceneParticles.h"
#include "FunctionLayer/Runtime/FeatureComponents.h"
namespace se
{
namespace
{
float ClampFinite(float value,float low,float high)
{
    return std::isfinite(value)?std::clamp(value,low,high):low;
}
Vec3 ClampNonNegative(Vec3 v)
{
    return {std::isfinite(v.x)?std::max(0.f,v.x):0.f,std::isfinite(v.y)?std::max(0.f,v.y):0.f,std::isfinite(v.z)?std::max(0.f,v.z):0.f};
}
Vec3 Clamp01(Vec3 v)
{
    return {ClampFinite(v.x,0.f,1.f),ClampFinite(v.y,0.f,1.f),ClampFinite(v.z,0.f,1.f)};
}
Vec3 LerpVec3(Vec3 a,Vec3 b,float t)
{
    return a+(b-a)*t;
}
float SmoothStep(float edge0,float edge1,float x)
{
    const float t=std::clamp((x-edge0)/std::max(1e-6f,edge1-edge0),0.f,1.f);
    return t*t*(3.f-2.f*t);
}
Quat DirectionToLightRotation(Vec3 directionToLight)
{
    const auto target=Normalize(directionToLight);
    if(LengthSquared(target)<1e-12f)return {};
    constexpr Vec3 source{0,0,1};
    const float cosine=Dot(source,target);
    if(cosine>0.999999f)return {};
    if(cosine<-0.999999f)return AxisAngle({0,1,0},Pi);
    const auto axis=Normalize(Cross(source,target));
    if(LengthSquared(axis)<1e-12f)return AxisAngle({0,1,0},Pi);
    return AxisAngle(axis,std::acos(std::clamp(cosine,-1.f,1.f)));
}
}
void ApplyFeatureFrameData(const World& world,FrameData& frame,RenderDefaults defaults)
{
    frame.todApplied=false;
    const auto* render=world.GetRegisteredComponent<RenderSettingsComponent>(SceneFeature(world,RenderSettingsComponent::TypeId));
    if(render||defaults==RenderDefaults::Project)
    {
        // 组件和无组件场景共用注册表默认值。字段位置只解析一次，不在逐帧提取时遍历反射表。
        static const auto fields=FindComponentDescriptor(RenderSettingsComponent::TypeId)->properties;
        static const auto indices=[] {
            std::unordered_map<std::string_view,std::size_t> result;
            for(std::size_t i=0;i<fields.size();++i)result.emplace(fields[i].key,i);
            return result;
        }();
        const auto get=[&](std::string_view key)->const PropertyValue& {
            const auto index=indices.at(key);
            return render&&index<render->values.size()?render->values[index]:fields[index].defaultValue;
        };
        const auto number=[&](const char* key){return std::get<float>(get(key));};
        const auto flag=[&](const char* key){return std::get<bool>(get(key));};
        frame.sceneRenderSettings=true;frame.exposure=number("render.exposure");frame.acesToneMap=flag("render.aces");frame.modelLod=flag("render.lod");frame.fullPbr=flag("render.fullPbr");
        frame.instancing=flag("render.instancing");frame.occlusion=flag("render.occlusion");frame.occlusionCounters=flag("render.occlusionCounters");frame.occlusionTiming=flag("render.occlusionTiming");frame.frameGraph=flag("render.frameGraph");
        frame.multiscatter=flag("render.multiscatter");frame.specularAA=flag("render.specularAA");
        frame.bc7Color=flag("render.bc7Color");
        frame.indoor.indirect=flag("render.indirect");
        frame.indoor.indirectStrength=number("render.indirectStrength");
        frame.indoor.reflections=flag("render.reflections");
        frame.indoor.reflectionStrength=number("render.reflectionStrength");
        frame.indoor.volumetricFog=flag("render.volumetricFog");
        frame.indoor.fogDensity=number("render.fogDensity");
        frame.indoor.fogDistance=number("render.fogDistance");
        frame.indoor.fogAlbedo=std::get<Vec3>(get("render.fogAlbedo"));
        frame.todLighting=flag("render.todLighting");
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
        frame.post.reset();
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
    frame.environment={};
    bool authoredEnvironment=false;
    if(const auto* component=world.GetComponent(SceneFeature(world,EnvironmentSettingsComponent::TypeId,&authoredEnvironment)))
    {
        const auto owner=component->Owner();frame.environment.asset=std::get<std::string>(FeatureValue(world,owner,"environmentmap.asset"));
        frame.environment.intensity=std::get<float>(FeatureValue(world,owner,"environmentmap.intensity"));
        frame.environment.yaw=std::get<float>(FeatureValue(world,owner,"environmentmap.yaw"))*Pi/180.f;
    }
    else frame.environment.useEngineDefault=defaults==RenderDefaults::Project&&frame.fullPbr&&!authoredEnvironment;
}
void ApplyTimeOfDayFrameData(FrameData& frame)
{
    if(frame.todApplied||!frame.todLighting||!frame.timeOfDay.has_value())return;
    const auto& tod=*frame.timeOfDay;
    if(!IsFinite(tod.sunDirection)||LengthSquared(tod.sunDirection)<1e-8f||!std::isfinite(tod.time))return;
    frame.todApplied=true;

    // tod.sunDirection 由 TODSystem 定义为从太阳射向世界的单位向量；渲染期望指向光源的方向。
    const float sinEl=std::clamp(-tod.sunDirection.y,-1.f,1.f);
    const float cosEl=std::sqrt(std::max(0.f,1.f-sinEl*sinEl));
    const Vec3 horiz{-tod.sunDirection.x,0.f,-tod.sunDirection.z};
    const Vec3 southAxis=LengthSquared(horiz)>1e-8f?Normalize(horiz):Vec3{0.f,0.f,1.f};
    const Vec3 eastAxis=Normalize(Cross(Vec3{0.f,1.f,0.f},southAxis));
    const double wrappedTime=tod.time-std::floor(tod.time);
    const double phi=(wrappedTime-0.5)*2.0*static_cast<double>(Pi);
    const Vec3 horizontalDir=eastAxis*static_cast<float>(-std::sin(phi))+southAxis*static_cast<float>(std::cos(phi));
    const Vec3 sunToLight=Normalize(horizontalDir*cosEl+Vec3{0.f,sinEl,0.f});

    const Vec3 rawSunColor=ClampNonNegative(tod.parameters.sunColor);
    const float rawSunIntensity=ClampFinite(tod.parameters.sunIntensity,0.f,10000.f);
    const float excessAirMass=std::clamp(1.f/std::max(0.14f,sinEl+0.14f)-1.f,0.f,6.f);
    const Vec3 extinction{std::exp(-0.045f*excessAirMass),std::exp(-0.095f*excessAirMass),std::exp(-0.21f*excessAirMass)};
    const float sunVis=SmoothStep(0.f,0.28f,sinEl);
    const float moonVis=SmoothStep(0.02f,0.14f,-sinEl);

    Vec3 dirToLight{};
    Vec3 lightColor{};
    float lightIntensity=0.f;
    if(sinEl>=-0.01f)
    {
        dirToLight=Normalize(Vec3{sunToLight.x,std::max(sunToLight.y,0.05f),sunToLight.z});
        lightColor=Hadamard(rawSunColor,extinction);
        lightIntensity=rawSunIntensity*sunVis;
    }
    else
    {
        dirToLight=Normalize(Vec3{-sunToLight.x,std::max(-sunToLight.y,0.08f),-sunToLight.z});
        lightColor=Clamp01(rawSunColor*0.20f+Vec3{0.30f,0.42f,0.68f});
        lightIntensity=std::clamp(rawSunIntensity*0.08f+0.04f,0.f,0.40f)*moonVis;
    }

    const Mat4 rotMatrix=Rotation(DirectionToLightRotation(dirToLight));
    LightInstance* primaryDirectional=nullptr;
    for(auto& item:frame.lights)
    {
        if(item.light.type==LightType::Directional&&(!primaryDirectional||item.entity<primaryDirectional->entity))
            primaryDirectional=&item;
    }
    if(primaryDirectional)
    {
        for(std::size_t row=0;row<3;++row)
            for(std::size_t col=0;col<3;++col)
                primaryDirectional->world(row,col)=rotMatrix(row,col);
        primaryDirectional->light.color=lightColor;
        primaryDirectional->light.intensity=lightIntensity;
    }
    else
    {
        frame.lights.push_back({0,rotMatrix,{lightColor,lightIntensity,LightType::Directional,10.f,true}});
    }

    const float daySky=SmoothStep(-0.16f,0.40f,sinEl);
    const float twilight=SmoothStep(-0.30f,0.02f,sinEl)*(1.f-SmoothStep(0.02f,0.45f,sinEl));
    const Vec3 skyBase=Clamp01(tod.parameters.skyColor);
    const Vec3 nightZenith{0.008f,0.014f,0.036f};
    const Vec3 dayZenith=Clamp01(Hadamard(skyBase,Vec3{0.62f,0.85f,1.18f})+Vec3{0.03f,0.09f,0.24f});
    const Vec3 zenith=Clamp01(LerpVec3(nightZenith,dayZenith,daySky)+Vec3{0.05f,0.03f,0.08f}*twilight);
    const Vec3 nightHorizon{0.018f,0.028f,0.058f};
    const Vec3 dayHorizon=Clamp01(skyBase*0.90f+Vec3{0.24f,0.34f,0.46f});
    const Vec3 twilightHorizon=Clamp01(Hadamard(rawSunColor,Vec3{0.72f,0.38f,0.14f})+Vec3{0.24f,0.10f,0.03f});
    const Vec3 horizon=Clamp01(LerpVec3(LerpVec3(nightHorizon,dayHorizon,daySky),twilightHorizon,std::min(1.f,twilight*0.85f)));
    const Vec3 ground=Clamp01(horizon*0.28f+Vec3{0.01f,0.01f,0.014f});

    frame.sky.enabled=true;
    frame.sky.zenith=zenith;
    frame.sky.horizon=horizon;
    frame.sky.ground=ground;
    if(sinEl>=-0.01f)
    {
        frame.sky.sunColor=Clamp01(lightColor);
        frame.sky.sunIntensity=std::clamp(rawSunIntensity*3.5f*sunVis,0.f,1000.f);
        frame.sky.sunAngularRadius=0.016f+(0.012f-0.016f)*SmoothStep(0.f,0.35f,sinEl);
    }
    else
    {
        frame.sky.sunColor={0.55f,0.65f,0.88f};
        frame.sky.sunIntensity=0.8f*moonVis;
        frame.sky.sunAngularRadius=0.011f;
    }
    frame.skyColor=Clamp01(zenith*0.45f+horizon*0.55f);

    const float ambientScale=0.12f+(1.f-0.12f)*SmoothStep(-0.12f,0.25f,sinEl)+0.08f*twilight;
    frame.environment.intensity*=ambientScale;

    const float todFogDensity=ClampFinite(tod.parameters.fogDensity,0.f,1.f);
    const Vec3 todFogColor=Clamp01(ClampNonNegative(tod.parameters.fogColor)*(0.22f+(1.f-0.22f)*daySky)+Vec3{0.16f,0.08f,0.03f}*twilight);
    if(todFogDensity>0.f)
    {
        frame.indoor.volumetricFog=true;
        frame.indoor.fogDensity=todFogDensity;
        frame.indoor.fogAlbedo=todFogColor;
        frame.indoor.fogDistance=std::max(frame.indoor.fogDistance,25.f);
    }
    else if(frame.indoor.volumetricFog)
    {
        frame.indoor.fogAlbedo=todFogColor;
    }
}
void PopulateParticleFrame(const SceneParticles& particles,FrameData& output)
{
    Vec3 cameraPosition{};
    if(output.editorCamera)
    {
        cameraPosition=output.editorCamera->position;
    }
    else if(!output.cameras.empty())
    {
        const CameraInstance* selected=nullptr;
        if(output.cameraBinding)
        {
            for(const auto& item:output.cameras)if(item.entity==*output.cameraBinding){selected=&item;break;}
        }
        if(!selected)
        {
            for(const auto& item:output.cameras)if(item.camera.primary){selected=&item;break;}
        }
        if(!selected)selected=&output.cameras.front();
        cameraPosition={selected->world(0,3),selected->world(1,3),selected->world(2,3)};
    }
    particles.BuildBatches(cameraPosition,output.particles.batches);
}
}
