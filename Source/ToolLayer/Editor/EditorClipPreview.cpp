#include "EditorClipPreview.h"
#include "EditorSession.h"
#include "FunctionLayer/Application/FrameData.h"
#include "FunctionLayer/Runtime/FeatureComponents.h"
#include "ResourceLayer/Assets/AssetStore.h"
#include <algorithm>
#include <cmath>

namespace flare
{
bool EditorClipPreview::Matches(const EditorSession& session,StableEntityId id)const
{
    if(!id||target_!=id||session.IsPlaying())return false;
    const auto& world=session.EditWorld();
    const auto entity=world.FindByStableId(id);
    if(!entity||!world.GetMesh(entity)||!world.FindComponent(entity,ActorSettingsComponent::TypeId))return false;
    return std::get<std::string>(FeatureValue(world,entity,"actor.asset"))==asset_
        &&std::get<std::string>(FeatureValue(world,entity,"actor.clip"))==clip_;
}
void EditorClipPreview::Clear()
{
    if(!target_&&!world_)return;
    target_=0;playing_=false;position_=0;duration_=0;
    world_.reset();sampler_.Reset();sampler_.SetResolver({});
}
bool EditorClipPreview::Select(const EditorSession& session,AssetStore& assets,StableEntityId id,std::string& error)
{
    if(Matches(session,id))return true;
    Clear();
    const auto& world=session.EditWorld();
    const auto entity=world.FindByStableId(id);
    if(session.IsPlaying()||!entity||!world.GetMesh(entity)||!world.FindComponent(entity,ActorSettingsComponent::TypeId)){
        error="EditorUi.PreviewRequiresActor";return false;}
    asset_=std::get<std::string>(FeatureValue(world,entity,"actor.asset"));
    clip_=std::get<std::string>(FeatureValue(world,entity,"actor.clip"));
    std::vector<AssetDiagnostic> diagnostics;
    const auto model=assets.LoadModel(asset_,diagnostics);
    if(!model){error="EditorUi.AnimationModelUnavailable";return false;}
    const auto found=std::find_if(model->animations.begin(),model->animations.end(),[&](const auto& clip){return clip.name==clip_;});
    if(found==model->animations.end()||model->joints.empty()){error="EditorUi.PreviewChooseClip";return false;}
    auto preview=std::make_unique<World>();
    const auto actor=preview->Create("Preview");
    if(!AddComponent(*preview,actor,"actor",error)||!WriteProperty(*preview,actor,"actor.asset",asset_,error)
        ||!SkeletalAnimationSystem::Play(*preview,preview->Get(actor)->stableId,clip_,1,false,error))return false;
    previewId_=preview->Get(actor)->stableId;
    world_=std::move(preview);sampler_.SetResolver([model](const auto&){return model;});
    duration_=found->duration;target_=id;Seek(0);return true;
}
void EditorClipPreview::Seek(double seconds)
{
    if(!world_||!std::isfinite(seconds))return;
    playing_=false;position_=std::clamp(seconds,0.,duration_);
    sampler_.Reset();sampler_.Advance(*world_,position_);
}
void EditorClipPreview::Toggle()
{
    if(!world_)return;
    if(!playing_&&position_>=duration_)Seek(0);
    playing_=!playing_;last_=std::chrono::steady_clock::now();
}
void EditorClipPreview::Update(const EditorSession& session,FrameData& frame)
{
    if(!target_)return;
    if(!Matches(session,session.Selected())){Clear();return;}
    if(playing_){
        const auto now=std::chrono::steady_clock::now();
        const auto& world=session.EditWorld();const auto entity=world.FindByStableId(target_);
        const double rate=std::get<float>(FeatureValue(world,entity,"actor.speed"));
        const double delta=std::chrono::duration<double>(now-last_).count()*rate;last_=now;
        const double next=position_+delta;
        if(next>=duration_&&duration_>0&&std::get<bool>(FeatureValue(world,entity,"actor.loop"))){
            Seek(std::fmod(next,duration_));playing_=true;
        }else{
            position_=std::clamp(next,0.,duration_);sampler_.Advance(*world_,std::max(0.,delta));
            if(position_>=duration_)playing_=false;
        }
    }
    if(const auto* pose=sampler_.Find(previewId_))
        for(auto& mesh:frame.meshes)if(mesh.entity==target_){mesh.skinPalette=pose->joints;break;}
}
}
