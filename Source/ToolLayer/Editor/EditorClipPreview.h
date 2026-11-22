#pragma once
#include "FunctionLayer/Runtime/SkeletalAnimation.h"
#include <chrono>

namespace flare
{
class AssetStore;
class EditorSession;
struct FrameData;

// 只在编辑视口采样这个角色，不启动项目脚本。
class EditorClipPreview
{
public:
    bool Select(const EditorSession&,AssetStore&,StableEntityId,std::string& error);
    void Toggle();
    void Seek(double seconds);
    void Clear();
    void Update(const EditorSession&,FrameData&);
    bool Playing()const{return playing_;}
    StableEntityId Target()const{return target_;}
    double Duration()const{return duration_;}
    double Time()const{return position_;}
private:
    bool Matches(const EditorSession&,StableEntityId)const;
    StableEntityId target_=0,previewId_=0;
    std::string asset_,clip_;
    std::unique_ptr<World> world_;
    SkeletalAnimationSystem sampler_;
    std::chrono::steady_clock::time_point last_;
    double duration_=0,position_=0;
    bool playing_=false;
};
}
