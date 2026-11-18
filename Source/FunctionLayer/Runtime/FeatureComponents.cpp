#include "FeatureComponents.h"
#include <array>
#include <algorithm>
#include <cmath>

namespace se
{
namespace
{
using P=PropertyDescriptor;
const std::array body{
    P{"body.halfExtents","Feature.Body.Extents",PropertyType::Vector3,Vec3{.5f,.5f,.5f},.001f,10000.f},
    P{"body.mass","Feature.Body.Mass",PropertyType::Number,1.f,0.f,100000.f},
    P{"body.friction","Feature.Body.Friction",PropertyType::Number,.6f,0.f,10.f},
    P{"body.restitution","Feature.Body.Restitution",PropertyType::Number,0.f,0.f,1.f},
    P{"body.linearDamping","Feature.Body.LinearDamping",PropertyType::Number,.02f,0.f,100.f},
    P{"body.angularDamping","Feature.Body.AngularDamping",PropertyType::Number,.05f,0.f,100.f}};
const std::array character{
    P{"character.radius","Feature.Character.Radius",PropertyType::Number,.3f,.01f,100.f},
    P{"character.segmentHalfHeight","Feature.Character.HalfHeight",PropertyType::Number,.6f,0.f,100.f},
    P{"character.walkSpeed","Feature.Character.Walk",PropertyType::Number,4.f,0.f,100.f},
    P{"character.jumpSpeed","Feature.Character.Jump",PropertyType::Number,5.f,0.f,100.f},
    P{"character.maximumPushImpulse","Feature.Character.Push",PropertyType::Number,.5f,0.f,100.f}};
constexpr std::array<std::string_view,3> pbrDefaults{"multiscatter","indirect","reflections"};
const std::array rendering{
    P{"render.fullPbr","Feature.Render.FullPbr",PropertyType::Boolean,true},
    P{"render.multiscatter","Feature.Render.Multiscatter",PropertyType::Boolean,true},
    P{"render.specularAA","Feature.Render.SpecularAA",PropertyType::Boolean,true},
    P{"render.bc7Color","Feature.Render.Bc7Color",PropertyType::Boolean,false},
    P{"render.exposure","Feature.Render.Exposure",PropertyType::Number,1.f,.001f,64.f},
    P{"render.post","Feature.Render.Post",PropertyType::Boolean,true},
    P{"render.bloom","Feature.Render.Bloom",PropertyType::Boolean,true},
    P{"render.bloomThreshold","Feature.Render.BloomThreshold",PropertyType::Number,1.f,0.f,100.f},
    P{"render.bloomStrength","Feature.Render.BloomStrength",PropertyType::Number,.08f,0.f,10.f},
    P{"render.fxaa","Feature.Render.Fxaa",PropertyType::Boolean,true},
    P{"render.taa","Feature.Render.Taa",PropertyType::Boolean,false},
    P{"render.autoExposure","Feature.Render.AutoExposure",PropertyType::Boolean,false},
    P{"render.aces","Feature.Render.Aces",PropertyType::Boolean,true},
    P{"render.frustumCulling","Feature.Render.Culling",PropertyType::Boolean,true},
    P{"render.sortOpaque","Feature.Render.Sort",PropertyType::Boolean,true},
    P{"render.cacheBindings","Feature.Render.Bindings",PropertyType::Boolean,true},
    P{"render.shadowEnabled","Feature.Render.Shadow",PropertyType::Boolean,true},
    P{"render.shadowPcf","Feature.Render.Pcf",PropertyType::Boolean,true},
    P{"render.cascades","Feature.Render.Cascades",PropertyType::Boolean,true},
    P{"render.cascadeCount","Feature.Render.CascadeCount",PropertyType::Number,3.f,1.f,3.f},
    P{"render.cascadeResolution","Feature.Render.CascadeResolution",PropertyType::Number,2048.f,256.f,4096.f},
    P{"render.cascadeDistance","Feature.Render.CascadeDistance",PropertyType::Number,100.f,1.f,1000.f},
    P{"render.cascadeDebug","Feature.Render.CascadeDebug",PropertyType::Boolean,false},
    P{"render.cascadeBias","Feature.Render.CascadeBias",PropertyType::Number,.00003f,0.f,.02f},
    P{"render.cascadeSlopeBias","Feature.Render.CascadeSlopeBias",PropertyType::Number,.0002f,0.f,.05f},
    P{"render.cascadeNormalBias","Feature.Render.CascadeNormalBias",PropertyType::Number,.01f,0.f,.25f},
    P{"render.shadowExtent","Feature.Render.ShadowExtent",PropertyType::Number,8.f,.1f,1000.f},
    P{"render.shadowDistance","Feature.Render.ShadowDistance",PropertyType::Number,16.f,1.f,10000.f},
    P{"render.shadowBias","Feature.Render.ShadowBias",PropertyType::Number,.0008f,0.f,.1f},
    P{"render.shadowNormalBias","Feature.Render.NormalBias",PropertyType::Number,.015f,0.f,1.f},
    P{"render.lutLift","Feature.Render.Lift",PropertyType::Vector3,Vec3{},-1.f,1.f},
    P{"render.lutGamma","Feature.Render.Gamma",PropertyType::Vector3,Vec3{1,1,1},.01f,10.f},
    P{"render.lutGain","Feature.Render.Gain",PropertyType::Vector3,Vec3{1,1,1},0.f,10.f},
    P{"render.lutEnabled","Feature.Render.Lut",PropertyType::Boolean,false},
    P{"render.lod","Feature.Render.Lod",PropertyType::Boolean,false},
    P{"render.hlod","Feature.Render.Hlod",PropertyType::Boolean,false},
    P{"render.hlodRadius","Feature.Render.HlodRadius",PropertyType::Number,0.f,0.f,100000.f},
    P{"render.instancing","Feature.Render.Instancing",PropertyType::Boolean,false},
    P{"render.occlusion","Feature.Render.Occlusion",PropertyType::Boolean,false},
    P{"render.occlusionCounters","Feature.Render.OcclusionCounters",PropertyType::Boolean,false},
    P{"render.occlusionTiming","Feature.Render.OcclusionTiming",PropertyType::Boolean,false},
    P{"render.frameGraph","Feature.Render.FrameGraph",PropertyType::Boolean,false},
    // W06：运行时外观覆盖。0=基础色、1=白模、2=Toon；blend 只读取覆盖结果，不回写资产材质。
    P{"render.lookMode","Feature.Render.LookMode",PropertyType::Number,0.f,0.f,2.f},
    P{"render.lookBlend","Feature.Render.LookBlend",PropertyType::Number,0.f,0.f,1.f},
    P{"render.supersample4x","Feature.Render.Supersample4x",PropertyType::Boolean,true},
    P{"render.clusteredLighting","Feature.Render.ClusteredLighting",PropertyType::Boolean,true},
    P{"render.clusterDebug","Feature.Render.ClusterDebug",PropertyType::Boolean,false},
    P{"render.shadowPcss","Feature.Render.ShadowPcss",PropertyType::Boolean,true},
    P{"render.shadowAngularRadius","Feature.Render.ShadowAngularRadius",PropertyType::Number,.00465f,0.f,.2f},
    P{"render.shadowSearchDistance","Feature.Render.ShadowSearchDistance",PropertyType::Number,20.f,.01f,100.f},
    P{"render.shadowFilterRadius","Feature.Render.ShadowFilterRadius",PropertyType::Number,16.f,1.f,32.f},
    P{"render.shadowFilterDebug","Feature.Render.ShadowFilterDebug",PropertyType::Number,0.f,0.f,2.f},
    P{"render.depthNormals","Feature.Render.DepthNormals",PropertyType::Boolean,true},
    P{"render.gtao","Feature.Render.Gtao",PropertyType::Boolean,true},
    P{"render.aoRadius","Feature.Render.AoRadius",PropertyType::Number,.6f,.001f,100.f},
    P{"render.aoIntensity","Feature.Render.AoIntensity",PropertyType::Number,.8f,0.f,1.f},
    P{"render.contactShadows","Feature.Render.ContactShadows",PropertyType::Boolean,true},
    P{"render.contactLength","Feature.Render.ContactLength",PropertyType::Number,.5f,0.f,10.f},
    P{"render.diffuseSh","Feature.Render.DiffuseSh",PropertyType::Boolean,false},
    P{"render.localDiffuseSh","Feature.Render.LocalDiffuseSh",PropertyType::Boolean,false},
    P{"render.diffuseProbe","Feature.Render.DiffuseProbe",PropertyType::AssetId,std::string{}},
    P{"render.gpuDriven","Feature.Render.GpuDriven",PropertyType::Boolean,false},
    P{"render.meshletCulling","Feature.Render.MeshletCulling",PropertyType::Boolean,false},
    P{"render.gpuDrivenHiZ","Feature.Render.GpuDrivenHiZ",PropertyType::Boolean,false},
    P{"render.indirect","Feature.Render.DynamicIndirect",PropertyType::Boolean,true},
    P{"render.indirectStrength","Feature.Render.IndirectStrength",PropertyType::Number,1.f,0.f,4.f},
    P{"render.reflections","Feature.Render.ScreenReflections",PropertyType::Boolean,true},
    P{"render.reflectionStrength","Feature.Render.ReflectionStrength",PropertyType::Number,1.f,0.f,1.f},
    P{"render.volumetricFog","Feature.Render.VolumetricFog",PropertyType::Boolean,false},
    P{"render.fogDensity","Feature.Render.FogDensity",PropertyType::Number,.035f,0.f,1.f},
    P{"render.fogDistance","Feature.Render.FogDistance",PropertyType::Number,25.f,.1f,500.f},
    P{"render.fogAlbedo","Feature.Render.FogAlbedo",PropertyType::Vector3,Vec3{.85f,.9f,1},0.f,1.f}};
const std::array rectangleLight{
    P{"rectangle.color","Feature.Rectangle.Color",PropertyType::Vector3,Vec3{1,1,1},0.f,65504.f},
    P{"rectangle.radiance","Feature.Rectangle.Radiance",PropertyType::Number,10.f,0.f,65504.f},
    P{"rectangle.width","Feature.Rectangle.Width",PropertyType::Number,1.f,.001f,1000.f},
    P{"rectangle.height","Feature.Rectangle.Height",PropertyType::Number,1.f,.001f,1000.f}};
const std::array actor{
    P{"actor.asset","Feature.Actor.Asset",PropertyType::AssetId,std::string{}},
    P{"actor.clip","Feature.Actor.Clip",PropertyType::Text,std::string{"Idle"}},
    P{"actor.speed","Feature.Actor.Speed",PropertyType::Number,1.f,0.f,10.f},
    P{"actor.loop","Feature.Actor.Loop",PropertyType::Boolean,true},
    P{"actor.playing","Feature.Actor.Playing",PropertyType::Boolean,true}};
const std::array emitter{
    P{"emitter.playing","Feature.Particle.Playing",PropertyType::Boolean,true},
    P{"emitter.loop","Feature.Particle.Loop",PropertyType::Boolean,true},
    P{"emitter.prewarm","Feature.Particle.Prewarm",PropertyType::Boolean,false},
    P{"emitter.additive","Feature.Particle.Additive",PropertyType::Boolean,false},
    P{"emitter.shape","Feature.Particle.Shape",PropertyType::Number,2.f,0.f,2.f},
    P{"emitter.emissionRate","Feature.Particle.EmissionRate",PropertyType::Number,20.f,0.f,5000.f},
    P{"emitter.lifetimeSeconds","Feature.Particle.Lifetime",PropertyType::Number,1.5f,.05f,60.f},
    P{"emitter.startSizeMeters","Feature.Particle.StartSize",PropertyType::Number,.25f,.01f,50.f},
    P{"emitter.endSizeMeters","Feature.Particle.EndSize",PropertyType::Number,.05f,0.f,50.f},
    P{"emitter.speedMin","Feature.Particle.SpeedMin",PropertyType::Number,1.5f,0.f,200.f},
    P{"emitter.speedMax","Feature.Particle.SpeedMax",PropertyType::Number,3.f,0.f,200.f},
    P{"emitter.coneAngleDegrees","Feature.Particle.ConeAngle",PropertyType::Number,25.f,0.f,89.f},
    P{"emitter.radiusMeters","Feature.Particle.Radius",PropertyType::Number,.5f,0.f,50.f},
    P{"emitter.gravity","Feature.Particle.Gravity",PropertyType::Vector3,Vec3{0.f,-2.f,0.f},-200.f,200.f},
    P{"emitter.startColor","Feature.Particle.StartColor",PropertyType::Vector3,Vec3{1.f,.85f,.4f},0.f,16.f},
    P{"emitter.startAlpha","Feature.Particle.StartAlpha",PropertyType::Number,1.f,0.f,1.f},
    P{"emitter.endColor","Feature.Particle.EndColor",PropertyType::Vector3,Vec3{1.f,.35f,.1f},0.f,16.f},
    P{"emitter.endAlpha","Feature.Particle.EndAlpha",PropertyType::Number,0.f,0.f,1.f},
    P{"emitter.maxParticles","Feature.Particle.MaxParticles",PropertyType::Number,256.f,1.f,16384.f},
    P{"emitter.seed","Feature.Particle.Seed",PropertyType::Number,1.f,1.f,2147483647.f},
    P{"emitter.softFactor","Feature.Particle.SoftFactor",PropertyType::Number,1.5f,.5f,8.f}};
const std::array environment{
    P{"environmentmap.asset","Feature.Environment.Asset",PropertyType::AssetId,std::string{}},
    P{"environmentmap.intensity","Feature.Environment.Intensity",PropertyType::Number,1.f,0.f,100.f},
    P{"environmentmap.yaw","Feature.Environment.Yaw",PropertyType::Number,0.f,-360.f,360.f}};
const std::array collider{
    P{"collider.shape","Feature.Collider.Shape",PropertyType::Number,0.f,0.f,1.f},
    P{"collider.halfExtentsMeters","Feature.Collider.HalfExtents",PropertyType::Vector3,Vec3{.5f,.5f,.5f},.001f,10000.f},
    P{"collider.radiusMeters","Feature.Collider.Radius",PropertyType::Number,.5f,.001f,10000.f},
    P{"collider.isTrigger","Feature.Collider.IsTrigger",PropertyType::Boolean,false}};
const std::array audioListener{
    P{"listener.volume","Feature.Audio.ListenerVolume",PropertyType::Number,1.f,0.f,4.f}};
const std::array audioSource{
    P{"audio.clip","Feature.Audio.Clip",PropertyType::AssetId,std::string{}},
    P{"audio.volume","Feature.Audio.Volume",PropertyType::Number,1.f,0.f,4.f},
    P{"audio.pitch","Feature.Audio.Pitch",PropertyType::Number,1.f,.1f,4.f},
    P{"audio.loop","Feature.Audio.Loop",PropertyType::Boolean,false},
    P{"audio.playOnStart","Feature.Audio.PlayOnStart",PropertyType::Boolean,true},
    P{"audio.spatialBlend","Feature.Audio.SpatialBlend",PropertyType::Number,1.f,0.f,1.f},
    P{"audio.minDistanceMeters","Feature.Audio.MinDistance",PropertyType::Number,1.f,.01f,10000.f},
    P{"audio.maxDistanceMeters","Feature.Audio.MaxDistance",PropertyType::Number,30.f,.01f,10000.f}};

template<class T> bool ValidFeatureProperty(std::size_t index,const PropertyValue& value,std::string& error)
{
    if constexpr(T::TypeId==RenderSettingsComponent::TypeId)
    {
        if(index>=rendering.size())return false;
        const auto key=rendering[index].key;
        if(key!="render.cascadeCount"&&key!="render.cascadeResolution"&&key!="render.shadowFilterDebug")return true;
        const auto* number=std::get_if<float>(&value);
        if(!number||!std::isfinite(*number)||std::floor(*number)!=*number)
        {error="Metadata.InvalidValue";return false;}
        if(key=="render.cascadeResolution"&&*number!=256&&*number!=512&&*number!=1024&&*number!=1536&&*number!=2048&&*number!=4096)
        {error="Metadata.InvalidValue";return false;}
    }
    else if constexpr(T::TypeId==ColliderSettingsComponent::TypeId)
    {
        if(index>=collider.size())return false;
        if(collider[index].key=="collider.shape")
        {
            const auto* number=std::get_if<float>(&value);
            if(!number||!std::isfinite(*number)||std::floor(*number)!=*number||(*number!=0.f&&*number!=1.f))
            {error="Metadata.InvalidValue";return false;}
        }
    }
    return true;
}
template<class T> bool Read(const World& world,ComponentHandle handle,std::size_t index,PropertyValue& value,std::string& error)
{
    const auto* component=world.GetRegisteredComponent<T>(handle);
    if(!component||index>=component->values.size()){error="Metadata.InvalidValue";return false;}
    value=component->values[index];return true;
}
template<class T> bool Write(World& world,ComponentHandle handle,std::size_t index,const PropertyValue& value,std::string& error)
{
    const auto* component=world.GetRegisteredComponent<T>(handle);
    if(!component||index>=component->values.size()){error="Metadata.InvalidValue";return false;}
    if(!ValidFeatureProperty<T>(index,value,error))return false;
    auto next=*component;
    if constexpr(T::TypeId==RenderSettingsComponent::TypeId)
    {
        // 用户从完整 PBR 切回基础光照时，这三个默认效果会强制选回 PBR。
        // 整组设置一次发布；载入和撤销均走 Create，保留已保存的独立设置。
        if(rendering[index].key=="render.fullPbr"&&std::get<bool>(next.values[index])&&!std::get<bool>(value))
            for(std::size_t i=0;i<std::min(next.values.size(),rendering.size());++i)
                if(std::find(pbrDefaults.begin(),pbrDefaults.end(),SerializedPropertyKey(rendering[i]))!=pbrDefaults.end())
                    next.values[i]=false;
    }
    next.values[index]=value;
    if(world.UpdateRegisteredComponent(handle,next))return true;
    error=world.LastError();return false;
}
template<class T> ComponentHandle Create(World& world,EntityHandle entity,std::span<const PropertyValue> values,bool enabled,std::string& error)
{
    for(std::size_t index=0;index<values.size();++index)if(!ValidFeatureProperty<T>(index,values[index],error))return {};
    if constexpr(T::TypeId==PhysicsBodyComponent::TypeId||T::TypeId==ColliderSettingsComponent::TypeId)
    {
        if(world.FindComponent(entity,CharacterSettingsComponent::TypeId)){error="Feature.Physics.Exclusive";return {};}
    }
    else if constexpr(T::TypeId==CharacterSettingsComponent::TypeId)
    {
        if(world.FindComponent(entity,PhysicsBodyComponent::TypeId)||world.FindComponent(entity,ColliderSettingsComponent::TypeId))
        {error="Feature.Physics.Exclusive";return {};}
    }
    T component;component.values.assign(values.begin(),values.end());
    const auto result=world.AttachRegisteredComponent(entity,component,enabled);
    if(!result)error=world.LastError();return result;
}
template<class T,std::size_t N> ComponentDescriptor Descriptor(const char* key,const char* title,const std::array<P,N>& properties)
{
    // 新字段在旧场景中缺失时使用注册默认值，避免追加属性破坏已保存场景。
    static const auto compatible=[&]{auto result=properties;for(auto& property:result)property.serializedOptional=true;return result;}();
    ComponentDescriptor descriptor{key,title,compatible};descriptor.type=T::TypeId;
    descriptor.read=Read<T>;descriptor.write=Write<T>;descriptor.create=Create<T>;return descriptor;
}
}
void CompleteRenderProperties(std::vector<ComponentProperty>& properties)
{
    const auto find=[&](std::string_view key) {
        return std::find_if(properties.begin(),properties.end(),[&](const auto& property){return property.key==key;});
    };
    const auto pbr=find("fullPbr");
    if(pbr==properties.end())return;
    const auto* fullPbr=std::get_if<bool>(&pbr->value);
    if(!fullPbr||*fullPbr)return;
    // 旧场景可只保存 BasicLit。缺少的从属开关不能替它重新选择 PBR，显式要求的开关仍生效。
    for(const auto key:pbrDefaults)
        if(find(key)==properties.end())properties.push_back({std::string(key),false});
}
void AppendEngineFeatureMetadata(std::vector<ComponentDescriptor>& descriptors)
{
    descriptors.push_back(Descriptor<PhysicsBodyComponent>("body","Feature.Body",body));
    descriptors.push_back(Descriptor<CharacterSettingsComponent>("character","Feature.Character",character));
    descriptors.push_back(Descriptor<RenderSettingsComponent>("render","Feature.Render",rendering));
    descriptors.push_back(Descriptor<EnvironmentSettingsComponent>("environmentmap","Feature.Environment",environment));
    descriptors.push_back(Descriptor<ActorSettingsComponent>("actor","Feature.Actor",actor));
    descriptors.push_back(Descriptor<RectangleLightComponent>("rectangle","Feature.Rectangle",rectangleLight));
    descriptors.push_back(Descriptor<ColliderSettingsComponent>("collider","Feature.Collider",collider));
    descriptors.push_back(Descriptor<AudioListenerComponent>("listener","Feature.AudioListener",audioListener));
    descriptors.push_back(Descriptor<AudioSourceComponent>("audio","Feature.Audio",audioSource));
    descriptors.push_back(Descriptor<ParticleEmitterComponent>("emitter","Feature.ParticleEmitter",emitter));
}
bool IsEngineFeature(ComponentTypeId type)noexcept{return type>=PhysicsBodyComponent::TypeId&&type<=ParticleEmitterComponent::TypeId;}
ComponentHandle SceneFeature(const World& world,ComponentTypeId type,bool* authored)
{
    if(authored)*authored=false;
    ComponentHandle result;StableEntityId best=UINT64_MAX;
    for(auto entity:world.Entities())
    {
        const auto handle=world.FindComponent(entity,type);const auto* component=world.GetComponent(handle);
        if(!component)continue;
        if(authored)*authored=true;
        if(world.IsEnabled(entity)&&component->Enabled()&&world.Get(entity)->stableId<best){result=handle;best=world.Get(entity)->stableId;}
    }
    return result;
}
PropertyValue FeatureValue(const World& world,EntityHandle owner,std::string_view key)
{
    PropertyValue value;std::string error;
    if(ReadProperty(world,owner,key,value,error))return value;
    const auto* property=FindProperty(key);return property?property->defaultValue:PropertyValue{};
}
}
