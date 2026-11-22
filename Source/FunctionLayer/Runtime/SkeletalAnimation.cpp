#include "SkeletalAnimation.h"
#include "FeatureComponents.h"
#include "ComponentMetadata.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace flare
{
namespace
{
constexpr std::string_view ActorAssetKey="actor.asset";
constexpr std::string_view ActorClipKey="actor.clip";
constexpr std::string_view ActorSpeedKey="actor.speed";
constexpr std::string_view ActorLoopKey="actor.loop";
constexpr std::string_view ActorPlayingKey="actor.playing";

Mat4 FromArray(const std::array<float,16>& values)
{
    Mat4 result;std::copy(values.begin(),values.end(),result.values.begin());return result;
}
Mat4 Compose(const Vec3& translation,const Quat& rotation,const Vec3& scale)
{
    return Translation(translation)*Rotation(rotation)*Scaling(scale);
}
Vec3 VectorAt(const std::array<float,4>& value){return {value[0],value[1],value[2]};}
Quat QuaternionAt(const std::array<float,4>& value){return {value[0],value[1],value[2],value[3]};}
// 夹取采样：找到不晚于 time 的最后一帧；LINEAR 在相邻两帧之间插值，STEP 保持前帧。
struct Sampled {std::size_t first=0,second=0;float blend=0;};
Sampled Locate(const std::vector<float>& times,double time)
{
    if(times.empty())return {};
    if(times.size()==1||time<=times.front())return {0,0,0};
    if(time>=times.back())return {times.size()-1,times.size()-1,0};
    const auto upper=std::upper_bound(times.begin(),times.end(),time);
    const std::size_t second=static_cast<std::size_t>(upper-times.begin());
    const std::size_t first=second-1;
    const float span=times[second]-times[first];
    const float blend=span>1e-9f?static_cast<float>((time-times[first])/span):0.f;
    return {first,second,blend};
}
Quat SlerpShortest(Quat a,Quat b,float t)
{
    a=Normalize(a);b=Normalize(b);
    float dot=a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w;
    if(dot<0){b={-b.x,-b.y,-b.z,-b.w};dot=-dot;}
    const float blend=std::clamp(t,0.f,1.f);
    float first=1.f-blend,second=blend;
    if(dot<0.9995f)
    {
        const float angle=std::acos(dot),denominator=std::sin(angle);
        first=std::sin(first*angle)/denominator;
        second=std::sin(second*angle)/denominator;
    }
    return Normalize(Quat{first*a.x+second*b.x,first*a.y+second*b.y,first*a.z+second*b.z,first*a.w+second*b.w});
}
Vec3 Lerp(Vec3 a,Vec3 b,float t){return {a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,a.z+(b.z-a.z)*t};}
EntityHandle FindByStableId(const World& world,StableEntityId stable)
{
    for(auto candidate:world.Entities())
    {
        const auto* record=world.Get(candidate);
        if(record&&record->stableId==stable)return candidate;
    }
    return {};
}
}

void SkeletalAnimationSystem::SetResolver(Resolver resolver)
{
    resolver_=std::move(resolver);
}
void SkeletalAnimationSystem::Reset()
{
    states_.clear();pendingPlays_.clear();version_=0;
}
void SkeletalAnimationSystem::RequestPlay(StableEntityId entity,std::string clip,float speed,bool loop)
{
    pendingPlays_[entity]=PendingPlay{std::move(clip),speed,loop};
}
bool SkeletalAnimationSystem::Play(World& world,StableEntityId entity,std::string_view clip,float speed,bool loop,std::string& error)
{
    const auto target=FindByStableId(world,entity);
    if(!target){error="Actor.MissingEntity";return false;}
    if(!world.FindComponent(target,ComponentTypeId{0x464C0005})){error="Actor.MissingComponent";return false;}
    if(!WriteProperty(world,target,ActorClipKey,PropertyValue{std::string(clip)},error))return false;
    if(!WriteProperty(world,target,ActorSpeedKey,PropertyValue{speed},error))return false;
    if(!WriteProperty(world,target,ActorLoopKey,PropertyValue{loop},error))return false;
    return WriteProperty(world,target,ActorPlayingKey,PropertyValue{true},error);
}
void SkeletalAnimationSystem::Sample(const StaticModel& model,const ModelAnimationClip& clip,const std::vector<ChannelIndex>& channels,double time,std::vector<Mat4>& locals)
{
    for(std::size_t index=0;index<locals.size();++index)
    {
        // 先取资产 rest TRS；只有存在通道的分量才被 clip 覆盖（契约 §4）。
        const auto& joint=model.joints[index];
        Vec3 translation{joint.restTranslation[0],joint.restTranslation[1],joint.restTranslation[2]};
        Quat rotation{joint.restRotation[0],joint.restRotation[1],joint.restRotation[2],joint.restRotation[3]};
        Vec3 scale{joint.restScale[0],joint.restScale[1],joint.restScale[2]};
        const auto& binding=channels[index];
        const auto apply=[&](std::int32_t channelIndex)
        {
            if(channelIndex<0||static_cast<std::size_t>(channelIndex)>=clip.channels.size())return;
            const auto& channel=clip.channels[static_cast<std::size_t>(channelIndex)];
            if(channel.times.empty()||channel.values.empty())return;
            const auto sampled=Locate(channel.times,time);
            const auto& a=channel.values[std::min(sampled.first,channel.values.size()-1)];
            const auto& b=channel.values[std::min(sampled.second,channel.values.size()-1)];
            if(channel.path==ModelAnimationPath::Rotation)
                rotation=channel.interpolation==ModelAnimationInterpolation::Step?QuaternionAt(a):SlerpShortest(QuaternionAt(a),QuaternionAt(b),sampled.blend);
            else if(channel.path==ModelAnimationPath::Translation)
                translation=channel.interpolation==ModelAnimationInterpolation::Step?VectorAt(a):Lerp(VectorAt(a),VectorAt(b),sampled.blend);
            else
                scale=channel.interpolation==ModelAnimationInterpolation::Step?VectorAt(a):Lerp(VectorAt(a),VectorAt(b),sampled.blend);
        };
        apply(binding.channel[0]);
        apply(binding.channel[1]);
        apply(binding.channel[2]);
        locals[index]=Compose(translation,rotation,scale);
    }
}
bool SkeletalAnimationSystem::Advance(World& world,double seconds)
{

    if(!resolver_)return true;
    if(!std::isfinite(seconds)||seconds<0)return false;
    std::unordered_map<StableEntityId,State> next;
    next.reserve(states_.size());
    for(auto entity:world.Entities())
    {
        const auto* record=world.Get(entity);
        if(!record||!world.IsEnabled(entity))continue;
        const auto component=world.FindComponent(entity,ComponentTypeId{0x464C0005});
        const auto* instance=world.GetComponent(component);
        if(!instance||!instance->Active())continue;
        const auto stable=record->stableId;
        State state;
        if(const auto previous=states_.find(stable);previous!=states_.end())state=std::move(previous->second);
        const auto read=[&](std::string_view key){return FeatureValue(world,entity,key);};
        const auto asset=std::get<std::string>(read(ActorAssetKey));
        const auto componentClip=std::get<std::string>(read(ActorClipKey));
        auto clipName=componentClip;
        const auto componentSpeed=std::get<float>(read(ActorSpeedKey));
        const auto componentLoop=std::get<bool>(read(ActorLoopKey));
        state.playing=std::get<bool>(read(ActorPlayingKey));
        // 运行时请求必须持续生效：场景里的 clip 只是初始值，脚本播放过的 clip 不能被
        // 每帧重新读回的初始值覆盖。只有场景/编辑器把组件 clip 改成新值时才让出控制权。
        if(state.hasOverride&&componentClip==state.componentClip)
        {
            clipName=state.overrideClip;state.speed=state.overrideSpeed;state.loop=state.overrideLoop;
        }
        else
        {
            state.hasOverride=false;state.componentClip=componentClip;
            state.speed=componentSpeed;state.loop=componentLoop;
        }
        if(const auto pending=pendingPlays_.find(stable);pending!=pendingPlays_.end())
        {
            clipName=pending->second.clip;
            // 单次动作可以显式重播；循环动作可逐步调速并保留当前相位。
            if(!pending->second.loop)state.time=0;
            state.speed=pending->second.speed;
            state.loop=pending->second.loop;
            state.playing=true;
            state.hasOverride=true;state.componentClip=componentClip;
            state.overrideClip=pending->second.clip;state.overrideSpeed=pending->second.speed;state.overrideLoop=pending->second.loop;
            pendingPlays_.erase(pending);
        }
        if(state.asset!=asset||!state.model)
        {
            state.asset=asset;state.clip.clear();state.clipIndex=UINT32_MAX;
            state.model=asset.empty()?nullptr:resolver_(asset);
            state.channels.clear();
        }
        if(!state.model){next.emplace(stable,std::move(state));continue;}
        // 诊断用伪 clip：不采样任何通道，只用资产 rest TRS 走蒙皮路径，用来区分
        // “rest/绑定空间错误”和“clip 数据错误”。
        if(clipName=="__rest")
        {
            auto& pose=state.pose;
            std::vector<Mat4> locals(state.model->joints.size());
            for(std::size_t index=0;index<locals.size();++index)
            {
                const auto& joint=state.model->joints[index];
                locals[index]=Compose({joint.restTranslation[0],joint.restTranslation[1],joint.restTranslation[2]},
                    {joint.restRotation[0],joint.restRotation[1],joint.restRotation[2],joint.restRotation[3]},
                    {joint.restScale[0],joint.restScale[1],joint.restScale[2]});
            }
            pose.joints.assign(state.model->joints.size(),Mat4::Identity());
            pose.jointWorld.assign(state.model->joints.size(),Mat4::Identity());
            pose.clip=0;pose.time=0;
            for(std::size_t index=0;index<locals.size();++index)
            {
                const auto parent=state.model->joints[index].parent;
                pose.jointWorld[index]=parent>=0&&static_cast<std::size_t>(parent)<index
                    ?pose.jointWorld[static_cast<std::size_t>(parent)]*locals[index]:locals[index];
                pose.joints[index]=pose.jointWorld[index]*FromArray(state.model->joints[index].inverseBind);
            }
            pose.version=++version_;
            next.emplace(stable,std::move(state));
            continue;
        }
        if(state.clip!=clipName||state.clipIndex==UINT32_MAX)
        {
            state.clip=clipName;state.clipIndex=UINT32_MAX;state.time=0;
            for(std::size_t index=0;index<state.model->animations.size();++index)
                if(state.model->animations[index].name==clipName){state.clipIndex=static_cast<std::uint32_t>(index);break;}
            state.channels.assign(state.model->joints.size(),ChannelIndex{});
            if(state.clipIndex!=UINT32_MAX)
                for(std::size_t index=0;index<state.model->animations[state.clipIndex].channels.size();++index)
                {
                    const auto& channel=state.model->animations[state.clipIndex].channels[index];
                    if(channel.joint>=state.channels.size())continue;
                    state.channels[channel.joint].channel[static_cast<std::size_t>(channel.path)]=static_cast<std::int32_t>(index);
                }
        }
        // 无 clip 时保持绑定姿态：不产生调色板，渲染走普通静态网格路径。
        if(state.clipIndex==UINT32_MAX){next.emplace(stable,std::move(state));continue;}
        const auto& model=*state.model;const auto& clip=model.animations[state.clipIndex];
        if(state.playing&&clip.duration>1e-6f)
        {
            state.time+=seconds*static_cast<double>(state.speed);
            state.time=state.loop?std::fmod(state.time,static_cast<double>(clip.duration))
                :std::min(state.time,static_cast<double>(clip.duration));
        }
        std::vector<Mat4> locals(model.joints.size());
        Sample(model,clip,state.channels,state.time,locals);
        auto& pose=state.pose;
        pose.joints.assign(model.joints.size(),Mat4::Identity());
        pose.jointWorld.assign(model.joints.size(),Mat4::Identity());
        pose.clip=state.clipIndex;pose.time=state.time;
        for(std::size_t index=0;index<model.joints.size();++index)
        {
            const auto parent=model.joints[index].parent;
            pose.jointWorld[index]=parent>=0&&static_cast<std::size_t>(parent)<index
                ?pose.jointWorld[static_cast<std::size_t>(parent)]*locals[index]:locals[index];
            pose.joints[index]=pose.jointWorld[index]*FromArray(model.joints[index].inverseBind);
        }
        pose.version=++version_;
        next.emplace(stable,std::move(state));
    }
    states_=std::move(next);
    return true;
}
const SkeletalPose* SkeletalAnimationSystem::Find(StableEntityId entity)const
{
    const auto found=states_.find(entity);
    return found==states_.end()||found->second.clipIndex==UINT32_MAX?nullptr:&found->second.pose;
}
bool SkeletalAnimationSystem::JointIndex(StableEntityId entity,std::string_view joint,std::uint32_t& index)const
{
    const auto found=states_.find(entity);
    if(found==states_.end()||!found->second.model)return false;
    const auto& joints=found->second.model->joints;
    for(std::size_t candidate=0;candidate<joints.size();++candidate)
        if(joints[candidate].name==joint){index=static_cast<std::uint32_t>(candidate);return true;}
    return false;
}
bool SkeletalAnimationSystem::QueryJointWorld(const World& world,StableEntityId entity,std::string_view joint,Mat4& output)const
{
    std::uint32_t index=0;
    if(!JointIndex(entity,joint,index))return false;
    const auto pose=Find(entity);
    if(!pose||index>=pose->jointWorld.size())return false;
    const auto target=FindByStableId(world,entity);
    if(!target)return false;
    output=world.GetWorldMatrix(target)*pose->jointWorld[index];
    return true;
}
bool SkeletalAnimationSystem::QueryJointPosition(const World& world,StableEntityId entity,std::string_view joint,Vec3& output)const
{
    Mat4 matrix;
    if(!QueryJointWorld(world,entity,joint,matrix))return false;
    output=TransformPoint(matrix,{});
    return true;
}
}
