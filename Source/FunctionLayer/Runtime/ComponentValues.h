#pragma once
#include "CoreLayer/Math/Math.h"
#include <string>
#include <string_view>
#include <vector>

namespace flare
{
// 兼容的属性/快照值描述；实际运行时存储由对应的 ComponentBase 包装拥有。
struct CameraComponent {float verticalFovRadians=Pi/3;float nearPlane=.1f;float farPlane=1000;bool primary=false;};
struct MeshComponent {std::string meshAsset;std::vector<std::string> materials;bool castsShadow=true,receivesShadow=true;};
enum class LightType {Directional,Point,Spot};
inline std::string_view LightTypeName(LightType type) noexcept
{
    switch(type)
    {
    case LightType::Directional:return "directional";
    case LightType::Point:return "point";
    case LightType::Spot:return "spot";
    }
    return {};
}
inline bool ParseLightType(std::string_view name,LightType& result) noexcept
{
    for(const auto type:{LightType::Directional,LightType::Point,LightType::Spot})
        if(LightTypeName(type)==name){result=type;return true;}
    return false;
}
struct LightComponent
{
    Vec3 color{1,1,1};
    float intensity=1;
    LightType type=LightType::Directional;
    float range=10;
    bool castsShadow=false;
    // 相对本地 -Z 发射轴的半角，弧度。距离和角度均独立于灯的缩放。
    float innerAngle=Pi/9,outerAngle=Pi/6;
};
}
