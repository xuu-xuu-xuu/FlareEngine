#pragma once
#include "World.h"
#include "ResourceLayer/Assets/StaticModel.h"
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace se
{
// 逐实体姿态结果。joints 是实体局部空间的蒙皮矩阵（jointWorld * inverseBind），
// jointWorld 供世界挂点查询；两者都按资产的父序排列，只在本固定步内有效。
struct SkeletalPose
{
    std::uint64_t version=0;
    std::uint32_t clip=UINT32_MAX;
    double time=0;
    std::vector<Mat4> joints;
    std::vector<Mat4> jointWorld;
};
// 运行时动画采样。资产只读、每个实体一份播放状态；在固定步内推进，
// 渲染与接触查询读取同一步的姿态版本。
class SkeletalAnimationSystem
{
public:
    using Resolver=std::function<std::shared_ptr<const StaticModel>(const std::string& asset)>;
    void SetResolver(Resolver resolver);
    void Reset();
    // 在 FixedPreTick 之前调用：推进时间、采样 clip、按父序计算关节。
    bool Advance(World& world,double seconds);
    // 播放控制：写入 actor 组件的可序列化属性，播放状态由下一固定步读取。
    static bool Play(World& world,StableEntityId entity,std::string_view clip,float speed,bool loop,std::string& error);
    // 脚本在固定步内使用的播放请求：绕开组件写入的队列限制，下一固定步生效。
    void RequestPlay(StableEntityId entity,std::string clip,float speed,bool loop);
    const SkeletalPose* Find(StableEntityId entity)const;
    bool JointIndex(StableEntityId entity,std::string_view joint,std::uint32_t& index)const;
    // 世界挂点 = 实体世界矩阵 * 关节局部位姿；失败时返回 false 并保持输出不变。
    bool QueryJointWorld(const World& world,StableEntityId entity,std::string_view joint,Mat4& output)const;
    bool QueryJointPosition(const World& world,StableEntityId entity,std::string_view joint,Vec3& output)const;
    std::uint64_t Version()const noexcept{return version_;}
private:
    struct ChannelIndex {std::array<std::int32_t,3> channel{-1,-1,-1};};
    struct State
    {
        std::string asset,clip;
        std::shared_ptr<const StaticModel> model;
        std::uint32_t clipIndex=UINT32_MAX;
        double time=0;
        float speed=1;
        bool loop=true,playing=true,initialized=false;
        // 运行时播放覆盖：脚本播放的 clip 持续生效，直到组件里的 clip 被改成新值。
        bool hasOverride=false;
        std::string componentClip,overrideClip;
        float overrideSpeed=1;
        bool overrideLoop=false;
        std::vector<ChannelIndex> channels;
        SkeletalPose pose;
    };
    static void Sample(const StaticModel& model,const ModelAnimationClip& clip,const std::vector<ChannelIndex>& channels,double time,
        std::vector<Mat4>& locals);
    struct PendingPlay {std::string clip;float speed=1;bool loop=false;};
    std::unordered_map<StableEntityId,State> states_;
    std::unordered_map<StableEntityId,PendingPlay> pendingPlays_;
    Resolver resolver_;
    std::uint64_t version_=0;
};
}
