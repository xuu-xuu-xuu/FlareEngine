#include "FrameData.h"
#include "FeatureFrameData.h"
#include "FunctionLayer/Runtime/FeatureComponents.h"
namespace flare
{
void BuildFrameData(const Environment& environment,const UiTree* ui,const PhysicsWorld* physics,FrameData& output)
{
    BuildFrameData(environment.GetWorld(),ui,physics,output);
    if(environment.TimeOfDay().Profile())
    {
        output.timeOfDay=environment.Snapshot();
        ApplyTimeOfDayFrameData(output);
    }
    // 蒙皮调色板复制进本帧快照：渲染器不持有 World 或运行时状态指针。
    for(auto& mesh:output.meshes)
        if(const auto* pose=environment.Skeletal().Find(mesh.entity)){mesh.skinPalette=pose->joints;mesh.staticGeometry=false;}
}
void BuildFrameData(const World& world,const UiTree* ui,const PhysicsWorld* physics,FrameData& output,RenderDefaults defaults)
{
    FrameData next;
    for(auto entity:world.Entities())
    {
        const auto* record=world.Get(entity);bool enabled=true,rigidHierarchy=true,staticHierarchy=true;
        const auto activeOn=[&](EntityHandle target,ComponentTypeId type){
            const auto* component=world.GetComponent(world.FindComponent(target,type));
            return component&&component->Active();
        };
        for(auto p=entity;p;){const auto* r=world.Get(p);if(!r||!r->enabled){enabled=false;break;}
            const auto* transform=world.GetTransform(p);
            if(!transform||!HasUnitScale(transform->scale))rigidHierarchy=false;
            if((r->staticFlags&EntityStaticFlags::Explicit)!=0)
            {
                if((r->staticFlags&EntityStaticFlags::Batching)==0)staticHierarchy=false;
            }
            else if(activeOn(p,ComponentTypes::Script)||activeOn(p,CharacterSettingsComponent::TypeId)
                ||activeOn(p,ActorSettingsComponent::TypeId)||activeOn(p,NavigationAgentComponent::TypeId))
                staticHierarchy=false;
            else if(activeOn(p,PhysicsBodyComponent::TypeId))
            {
                if(std::get<float>(FeatureValue(world,p,"body.mass"))>0.f)staticHierarchy=false;
            }
            p=r->parent;}
        if(!enabled)continue;
        const auto matrix=world.GetWorldMatrix(entity);
        const auto active=[&](ComponentTypeId type){return activeOn(entity,type);};
        if(const auto* mesh=world.GetMesh(entity);mesh&&active(ComponentTypes::Mesh))
            next.meshes.push_back({record->stableId,matrix,*mesh,{},staticHierarchy,world.EntitySpatialRevision(entity)});
        if(const auto* camera=world.GetCamera(entity);camera&&active(ComponentTypes::Camera))next.cameras.push_back({record->stableId,matrix,*camera,rigidHierarchy});
        if(const auto* light=world.GetLight(entity);light&&active(ComponentTypes::Light))next.lights.push_back({record->stableId,matrix,*light});
        if(active(RectangleLightComponent::TypeId))
        {
            const auto color=std::get<Vec3>(FeatureValue(world,entity,"rectangle.color"));
            const auto radiance=std::get<float>(FeatureValue(world,entity,"rectangle.radiance"));
            const auto width=std::get<float>(FeatureValue(world,entity,"rectangle.width"));
            const auto height=std::get<float>(FeatureValue(world,entity,"rectangle.height"));
            next.rectangleLights.push_back({record->stableId,{{matrix(0,3),matrix(1,3),matrix(2,3)},
                {matrix(0,0)*width*.5f,matrix(1,0)*width*.5f,matrix(2,0)*width*.5f},
                {matrix(0,1)*height*.5f,matrix(1,1)*height*.5f,matrix(2,1)*height*.5f},color*radiance}});
        }
    }
    if(ui)next.ui=ui->Visuals();
    if(physics)next.debugLines=physics->DebugLines();
    ApplyFeatureFrameData(world,next,defaults);
    output=std::move(next);
}
}
