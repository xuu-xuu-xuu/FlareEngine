#include "PlayerQuality.h"
#include "FrameData.h"
#include <Windows.h>
#include <fstream>
#include <string>

namespace flare
{
bool SupportsPlayerQuality(const FrameData& frame)
{
    return !frame.instancing&&(!frame.sceneRenderSettings||frame.post.has_value());
}
void ApplyPlayerQuality(PlayerQuality level,FrameData& frame,const QualityProfiles& profiles)
{
    const auto index=static_cast<size_t>(level);
    if(index==0||index>profiles.size()||!SupportsPlayerQuality(frame))return;
    if(!frame.sceneRenderSettings){
        frame.sceneRenderSettings=true;
        frame.post.emplace();frame.post->bloom=false;
    }
    const auto& profile=profiles[index-1];
    if(!profile.allowShadows)frame.shadow.enabled=false;
    auto& post=*frame.post;
    if(profile.sampling!=QualitySampling::Scene){
        post.fxaa=profile.sampling==QualitySampling::Fxaa||profile.sampling==QualitySampling::Spatial4x;
        post.taa=profile.sampling==QualitySampling::Taa;post.supersample4x=profile.sampling==QualitySampling::Spatial4x;}
    if(!profile.allowBloom)post.bloom=false;
}
PlayerQualityPreference::PlayerQualityPreference(std::filesystem::path file):file_(std::move(file))
{
    std::error_code error;
    if(file_.empty()||std::filesystem::file_size(file_,error)>64||error)return;
    std::ifstream stream(file_);std::string id;
    if(std::getline(stream,id))for(size_t i=0;i<PlayerQualityIds.size();++i)
        if(id==PlayerQualityIds[i]){level_=static_cast<PlayerQuality>(i);break;}
}
bool PlayerQualityPreference::Select(PlayerQuality level)
{
    const auto index=static_cast<size_t>(level);
    if(index>=PlayerQualityIds.size()||file_.empty())return false;
    std::error_code error;std::filesystem::create_directories(file_.parent_path(),error);if(error)return false;
    auto temp=file_;temp+=L"."+std::to_wstring(GetCurrentProcessId())+L".tmp";
    {std::ofstream stream(temp,std::ios::trunc);stream<<PlayerQualityIds[index]<<'\n';stream.close();if(!stream)return false;}
    if(!MoveFileExW(temp.c_str(),file_.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
    {std::filesystem::remove(temp,error);return false;}
    level_=level;return true;
}
}
