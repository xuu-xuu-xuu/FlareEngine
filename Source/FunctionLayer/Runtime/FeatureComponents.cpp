#include "FeatureComponents.h"
#include <array>
#include <algorithm>

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
const std::array rendering{
    P{"render.fullPbr","Feature.Render.FullPbr",PropertyType::Boolean,true},
    P{"render.exposure","Feature.Render.Exposure",PropertyType::Number,1.f,.001f,64.f},
    P{"render.post","Feature.Render.Post",PropertyType::Boolean,true},
    P{"render.bloom","Feature.Render.Bloom",PropertyType::Boolean,true},
    P{"render.bloomThreshold","Feature.Render.BloomThreshold",PropertyType::Number,1.f,0.f,100.f},
    P{"render.bloomStrength","Feature.Render.BloomStrength",PropertyType::Number,.08f,0.f,10.f},
    P{"render.fxaa","Feature.Render.Fxaa",PropertyType::Boolean,true},
    P{"render.taa","Feature.Render.Taa",PropertyType::Boolean,false},
    P{"render.autoExposure","Feature.Render.AutoExposure",PropertyType::Boolean,false},
    P{"render.aces","Feature.Render.Aces",PropertyType::Boolean,false},
    P{"render.frustumCulling","Feature.Render.Culling",PropertyType::Boolean,true},
    P{"render.sortOpaque","Feature.Render.Sort",PropertyType::Boolean,true},
    P{"render.cacheBindings","Feature.Render.Bindings",PropertyType::Boolean,true},
    P{"render.shadowEnabled","Feature.Render.Shadow",PropertyType::Boolean,true},
    P{"render.shadowPcf","Feature.Render.Pcf",PropertyType::Boolean,true},
    P{"render.shadowExtent","Feature.Render.ShadowExtent",PropertyType::Number,8.f,.1f,10000.f},
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
    P{"render.frameGraph","Feature.Render.FrameGraph",PropertyType::Boolean,false}};
const std::array environment{
    P{"environmentmap.asset","Feature.Environment.Asset",PropertyType::AssetId,std::string{}},
    P{"environmentmap.intensity","Feature.Environment.Intensity",PropertyType::Number,1.f,0.f,100.f},
    P{"environmentmap.yaw","Feature.Environment.Yaw",PropertyType::Number,0.f,-360.f,360.f}};

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
    auto next=*component;next.values[index]=value;
    if(world.UpdateRegisteredComponent(handle,next))return true;
    error=world.LastError();return false;
}
template<class T> ComponentHandle Create(World& world,EntityHandle entity,std::span<const PropertyValue> values,bool enabled,std::string& error)
{
    if constexpr(T::TypeId==PhysicsBodyComponent::TypeId||T::TypeId==CharacterSettingsComponent::TypeId)
    {
        const auto other=T::TypeId==PhysicsBodyComponent::TypeId?CharacterSettingsComponent::TypeId:PhysicsBodyComponent::TypeId;
        if(world.FindComponent(entity,other)){error="Feature.Physics.Exclusive";return {};}
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
void AppendEngineFeatureMetadata(std::vector<ComponentDescriptor>& descriptors)
{
    descriptors.push_back(Descriptor<PhysicsBodyComponent>("body","Feature.Body",body));
    descriptors.push_back(Descriptor<CharacterSettingsComponent>("character","Feature.Character",character));
    descriptors.push_back(Descriptor<RenderSettingsComponent>("render","Feature.Render",rendering));
    descriptors.push_back(Descriptor<EnvironmentSettingsComponent>("environmentmap","Feature.Environment",environment));
}
bool IsEngineFeature(ComponentTypeId type)noexcept{return type>=PhysicsBodyComponent::TypeId&&type<=EnvironmentSettingsComponent::TypeId;}
ComponentHandle SceneFeature(const World& world,ComponentTypeId type)
{
    ComponentHandle result;StableEntityId best=UINT64_MAX;
    for(auto entity:world.Entities())
    {
        if(!world.IsEnabled(entity))continue;
        const auto handle=world.FindComponent(entity,type);const auto* component=world.GetComponent(handle);
        if(component&&component->Enabled()&&world.Get(entity)->stableId<best){result=handle;best=world.Get(entity)->stableId;}
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
