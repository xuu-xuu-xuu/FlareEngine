#include "ScenePhysics.h"
#include "FunctionLayer/Runtime/FeatureComponents.h"
#include "CoreLayer/Math/TransformDecomposition.h"
#include <unordered_set>

namespace se
{
namespace
{
bool SameProperties(const std::vector<PropertyValue>& first,const std::vector<PropertyValue>& second)
{
    if(first.size()!=second.size())return false;
    for(std::size_t i=0;i<first.size();++i)
    {
        if(first[i].index()!=second[i].index())return false;
        if(const auto* number=std::get_if<float>(&first[i])){if(*number!=std::get<float>(second[i]))return false;}
        else if(const auto* vector=std::get_if<Vec3>(&first[i])){if(LengthSquared(*vector-std::get<Vec3>(second[i]))!=0)return false;}
        else return false;
    }
    return true;
}
bool WorldTransform(const World& world,EntityHandle entity,Transform& transform)
{return TryDecomposeTrs(world.GetWorldMatrix(entity),transform);}
bool ApplyWorld(World& world,EntityHandle entity,Vec3 position,Quat rotation)
{
    auto transform=*world.GetTransform(entity);Transform desired=transform;desired.position=position;desired.rotation=rotation;
    const auto parent=world.Get(entity)->parent;
    if(parent)
    {
        Transform original;if(!WorldTransform(world,entity,original))return false;desired.scale=original.scale;
        const auto inverse=Inverse(world.GetWorldMatrix(parent));if(!inverse)return false;
        if(!TryDecomposeTrs(*inverse*desired.Matrix(),transform))return false;
    }
    else {transform.position=position;transform.rotation=rotation;}
    return world.SetTransform(entity,transform);
}
}
bool ScenePhysics::Owns(BodyHandle body)const
{for(const auto& [id,entry]:bodies_)if(entry.handle==body)return true;return false;}
void ScenePhysics::Clear(PhysicsWorld& physics)
{for(const auto& [id,entry]:bodies_)if(physics.IsAlive(entry.handle))physics.DestroyBody(entry.handle);bodies_.clear();characters_.clear();movedCharacters_.clear();}
bool ScenePhysics::Synchronize(const World& world,PhysicsWorld& physics)
{
    std::unordered_set<StableEntityId> wanted,characters;
    for(const auto entity:world.Entities())
    {
        if(!world.IsEnabled(entity))continue;const auto id=world.Get(entity)->stableId;
        const auto* component=world.GetRegisteredComponent<PhysicsBodyComponent>(world.FindComponent(entity,PhysicsBodyComponent::TypeId));
        Transform transform;if(!WorldTransform(world,entity,transform))continue;
        if(component&&component->Enabled())
        {
            wanted.insert(id);auto existing=bodies_.find(id);
            if(existing!=bodies_.end()&&(!physics.IsAlive(existing->second.handle)||!SameProperties(existing->second.properties,component->values)||LengthSquared(existing->second.scale-transform.scale)!=0))
            {if(physics.IsAlive(existing->second.handle))physics.DestroyBody(existing->second.handle);bodies_.erase(existing);existing=bodies_.end();}
            if(existing==bodies_.end())
            {
                bool external=false;for(auto body:physics.Bodies())if(physics.GetBody(body)->description.entityStableId==id){external=true;break;}
                if(!external)
                {
                    BodyDesc description;const auto extents=std::get<Vec3>(component->values[0]);
                    description.shape={transform.position,{extents.x*std::abs(transform.scale.x),extents.y*std::abs(transform.scale.y),extents.z*std::abs(transform.scale.z)},transform.rotation};
                    description.mass=std::get<float>(component->values[1]);description.friction=std::get<float>(component->values[2]);
                    description.restitution=std::get<float>(component->values[3]);description.linearDamping=std::get<float>(component->values[4]);description.angularDamping=std::get<float>(component->values[5]);description.entityStableId=id;
                    const auto handle=physics.CreateBody(description);if(!handle)return false;
                    bodies_.emplace(id,Body{handle,component->values,transform.scale});
                }
            }
            else if(const auto* state=physics.GetBody(existing->second.handle))
            {
                const auto& rotation=state->description.shape.rotation;const auto& target=transform.rotation;
                const float dot=rotation.x*target.x+rotation.y*target.y+rotation.z*target.z+rotation.w*target.w;
                if(LengthSquared(state->description.shape.center-transform.position)>1e-8f||std::abs(dot)<.999999f)
                    if(!physics.Teleport(existing->second.handle,transform.position,transform.rotation,false))return false;
            }
        }
        const auto* character=world.GetRegisteredComponent<CharacterSettingsComponent>(world.FindComponent(entity,CharacterSettingsComponent::TypeId));
        if(character&&character->Enabled())
        {
            characters.insert(id);auto& controller=characters_[id];controller.position=transform.position;
            controller.radius=std::get<float>(character->values[0]);controller.segmentHalfHeight=std::get<float>(character->values[1]);
            controller.walkSpeed=std::get<float>(character->values[2]);controller.jumpSpeed=std::get<float>(character->values[3]);controller.maximumPushImpulse=std::get<float>(character->values[4]);
        }
    }
    std::erase_if(bodies_,[&](const auto& item){if(wanted.contains(item.first))return false;if(physics.IsAlive(item.second.handle))physics.DestroyBody(item.second.handle);return true;});
    std::erase_if(characters_,[&](const auto& item){return !characters.contains(item.first);});return true;
}
bool ScenePhysics::ApplyTransforms(World& world,const PhysicsWorld& physics)
{
    for(const auto& [id,entry]:bodies_)
    {
        const auto entity=world.FindByStableId(id);const auto* body=physics.GetBody(entry.handle);if(!entity||!body)continue;
        if(!ApplyWorld(world,entity,body->description.shape.center,body->description.shape.rotation))return false;
    }
    return true;
}
bool ScenePhysics::MoveCharacter(World& world,PhysicsWorld& physics,StableEntityId id,Vec3 direction,bool jump,float seconds)
{
    auto it=characters_.find(id);const auto entity=world.FindByStableId(id);if(it==characters_.end()||!entity)return false;
    if(!it->second.Move(physics,direction,jump,seconds))return false;
    movedCharacters_.insert(id);
    Transform transform;if(!WorldTransform(world,entity,transform))return false;
    return ApplyWorld(world,entity,it->second.position,transform.rotation);
}
bool ScenePhysics::Step(World& world,PhysicsWorld& physics,float seconds)
{
    if(!Synchronize(world,physics)||!physics.Step(seconds)||!ApplyTransforms(world,physics))return false;
    return AdvanceCharacters(world,physics,seconds);
}
bool ScenePhysics::AdvanceCharacters(World& world,PhysicsWorld& physics,float seconds)
{
    // 无脚本输入时仍推进重力与接地；脚本可显式调用 MoveCharacter。
    for(auto& [id,character]:characters_)if(!movedCharacters_.contains(id)&&!MoveCharacter(world,physics,id,{},false,seconds))return false;
    movedCharacters_.clear();return true;
}
bool ScenePhysics::TeleportCharacter(World& world,StableEntityId id,Vec3 position)
{
    auto it=characters_.find(id);const auto entity=world.FindByStableId(id);if(it==characters_.end()||!entity||!IsFinite(position))return false;
    Transform transform;if(!WorldTransform(world,entity,transform)||!ApplyWorld(world,entity,position,transform.rotation))return false;
    it->second.position=position;it->second.verticalVelocity=0;it->second.grounded=false;return true;
}
std::vector<DebugLine> ScenePhysics::CharacterLines()const
{std::vector<DebugLine> lines;for(const auto& [id,character]:characters_){const auto value=character.DebugLines();lines.insert(lines.end(),value.begin(),value.end());}return lines;}
bool ValidateScenePhysics(const World& world,std::string& error)
{
    error.clear();for(auto entity:world.Entities())
    {
        if(!world.IsEnabled(entity))continue;
        if(!world.FindComponent(entity,PhysicsBodyComponent::TypeId)&&!world.FindComponent(entity,CharacterSettingsComponent::TypeId))continue;
        Transform transform;if(!WorldTransform(world,entity,transform)){error="Feature.Physics.InvalidTransform";return false;}
        if(std::abs(transform.scale.x)<.0001f||std::abs(transform.scale.y)<.0001f||std::abs(transform.scale.z)<.0001f){error="Feature.Physics.InvalidTransform";return false;}
    }
    return true;
}
}
