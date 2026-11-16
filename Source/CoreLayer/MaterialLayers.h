#pragma once
#include "Math/Math.h"

namespace se
{
// glTF clear coat 的数据默认值；0 权重不启用涂层，normalScale 不借用 base normal。
struct ClearCoatParameters
{
    float weight=0,roughness=0,normalScale=1;
};
struct SheenParameters
{
    Vec3 color{0,0,0}; // 线性 RGB；零色关闭 sheen。
    float roughness=0;
};
struct AnisotropyParameters
{
    float strength=0,rotation=0; // rotation：切线到副切线方向，逆时针弧度。
};
struct SubsurfaceParameters
{
    float weight=0,radius=.001f; // weight>0 显式启用皮肤漫反射；radius 为米制扩散轮廓尺度。
    Vec3 color{1,1,1}; // 线性散射 tint；不表示透射或厚度。
};
inline bool IsValidMaterialLayerColor(Vec3 color)
{return IsFinite(color)&&color.x>=0&&color.x<=1&&color.y>=0&&color.y<=1&&color.z>=0&&color.z<=1;}
inline bool IsValidMaterialLayer(const ClearCoatParameters& p)
{return IsFinite(p.weight)&&p.weight>=0&&p.weight<=1&&IsFinite(p.roughness)&&p.roughness>=0&&p.roughness<=1&&IsFinite(p.normalScale);}
inline bool IsValidMaterialLayer(const SheenParameters& p)
{return IsValidMaterialLayerColor(p.color)&&IsFinite(p.roughness)&&p.roughness>=0&&p.roughness<=1;}
inline bool IsValidMaterialLayer(const AnisotropyParameters& p)
{return IsFinite(p.strength)&&p.strength>=0&&p.strength<=1&&IsFinite(p.rotation);}
inline bool IsValidMaterialLayer(const SubsurfaceParameters& p)
{return IsFinite(p.weight)&&p.weight>=0&&p.weight<=1&&IsFinite(p.radius)&&p.radius>0&&p.radius<=1&&IsValidMaterialLayerColor(p.color);}
}
