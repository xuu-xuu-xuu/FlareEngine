#pragma once
#include "CoreLayer/QualityProfiles.h"
#include <filesystem>
#include <string_view>

namespace flare
{
struct FrameData;
enum class PlayerQuality { Project,Performance,Balanced,High };
// 只覆盖玩家可以选择的几项；每帧从场景原始快照开始，恢复项目设置不需要逆向修改世界。
void ApplyPlayerQuality(PlayerQuality level,FrameData& frame,const QualityProfiles& profiles=DefaultQualityProfiles);
bool SupportsPlayerQuality(const FrameData& frame);
class PlayerQualityPreference
{
public:
    explicit PlayerQualityPreference(std::filesystem::path file);
    PlayerQuality Level()const noexcept{return level_;}
    bool Select(PlayerQuality);
private:
    std::filesystem::path file_;
    PlayerQuality level_=PlayerQuality::Project;
};
}
