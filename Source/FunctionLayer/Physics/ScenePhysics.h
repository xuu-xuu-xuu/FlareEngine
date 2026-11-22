#pragma once
#include "PhysicsWorld.h"
#include "CoreLayer/PropertyMetadata.h"
#include "FunctionLayer/Runtime/World.h"
#include <unordered_map>
#include <unordered_set>

namespace flare
{
bool ValidateScenePhysics(const World&,std::string& error);
// 只拥有反射组件生成的代理；与脚本手动创建的物理对象共用 PhysicsWorld。
class ScenePhysics
{
public:
    bool Synchronize(const World&,PhysicsWorld&);
    bool ApplyTransforms(World&,const PhysicsWorld&);
    bool Owns(BodyHandle body)const;
    void Clear(PhysicsWorld&);
    std::vector<DebugLine> CharacterLines()const;
    bool MoveCharacter(World&,PhysicsWorld&,StableEntityId,Vec3,bool,float);
    bool AdvanceCharacters(World&,PhysicsWorld&,float);
    const CharacterController* Character(StableEntityId id)const {const auto it=characters_.find(id);return it==characters_.end()?nullptr:&it->second;}
    const std::unordered_map<StableEntityId,CharacterController>& Characters()const noexcept {return characters_;}
    bool TeleportCharacter(World&,StableEntityId,Vec3);
    bool Step(World&,PhysicsWorld&,float);
private:
    struct Body
    {
        BodyHandle handle;
        bool hasBody=false;
        bool hasCollider=false;
        std::uint32_t entityLayer=0;
        std::vector<PropertyValue> bodyProperties;
        std::vector<PropertyValue> colliderProperties;
        Vec3 scale;
    };
    std::unordered_map<StableEntityId,Body> bodies_;
    std::unordered_map<StableEntityId,CharacterController> characters_;
    std::unordered_set<StableEntityId> movedCharacters_;
};
}
