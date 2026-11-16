#pragma once
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <nlohmann/json.hpp>

namespace se
{
inline constexpr std::array PlayerQualityIds{"project","performance","balanced","high"};
inline constexpr std::array QualitySamplingIds{"scene","off","fxaa","taa","spatial4x"};
enum class QualitySampling { Scene,Off,Fxaa,Taa,Spatial4x };
struct QualityProfile
{
    QualitySampling sampling=QualitySampling::Fxaa;
    bool allowShadows=true,allowBloom=true;
    bool operator==(const QualityProfile&)const=default;
};
using QualityProfiles=std::array<QualityProfile,3>;
inline constexpr QualityProfiles DefaultQualityProfiles{{{QualitySampling::Fxaa,false,false},{QualitySampling::Fxaa,true,true},{QualitySampling::Spatial4x,true,true}}};
inline std::optional<QualitySampling> ParseQualitySampling(std::string_view id)
{
    for(size_t i=0;i<QualitySamplingIds.size();++i)if(id==QualitySamplingIds[i])return static_cast<QualitySampling>(i);
    return {};
}
inline bool ReadQualityProfiles(const nlohmann::json& object,QualityProfiles& output)
{
    if(!object.is_object())return false;
    auto next=DefaultQualityProfiles;
    for(size_t i=0;i<next.size();++i){
        const auto it=object.find(PlayerQualityIds[i+1]);if(it==object.end())continue;
        if(!it->is_object())return false;
        auto& p=next[i];const auto method=it->find("sampling");
        if(method!=it->end()){if(!method->is_string())return false;const auto value=ParseQualitySampling(method->get_ref<const std::string&>());if(!value)return false;p.sampling=*value;}
        for(auto [key,flag]:{std::pair{"allowShadows",&p.allowShadows},std::pair{"allowBloom",&p.allowBloom}}){
            auto field=it->find(key);if(field!=it->end()){if(!field->is_boolean())return false;*flag=field->get<bool>();}}
    }
    output=next;return true;
}
inline void WriteQualityProfiles(const QualityProfiles& profiles,nlohmann::json& object)
{
    for(size_t i=0;i<profiles.size();++i){auto& dst=object[PlayerQualityIds[i+1]];const auto& p=profiles[i];
        dst["sampling"]=QualitySamplingIds[static_cast<size_t>(p.sampling)];dst["allowShadows"]=p.allowShadows;dst["allowBloom"]=p.allowBloom;}
}
}
