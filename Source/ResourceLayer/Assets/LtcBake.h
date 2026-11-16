#pragma once
#include "HdrImage.h"

namespace se
{
// 离线拟合器来源及原许可证：ThirdParty/LtcReference/UPSTREAM.json。
inline constexpr const char* LtcCookerVersion="SelfEngine.Ltc.2/ggx-correlated-single-scatter-vndf2018/selfshadow-31e5e96";
inline constexpr float LtcMinimumRoughness=.045f,LtcMinimumNv=.0001f;
struct LtcBakeSettings
{
    std::uint32_t size=64,sampleSide=32;
    bool operator==(const LtcBakeSettings&) const=default;
};
struct LtcLut
{
    LtcBakeSettings settings;
    // x=感知 roughness，y=2/pi*acos(sqrt((Nv-minNv)/(1-minNv)))；行主序、端点采样。
    // inverse / inverse.yy，存 (xx,zx,xz,zz)，隐含 yy=1，其余 4 项为 0。
    std::vector<Vec4> inverseMatrices;
    // x=无 Fresnel 的半球积分 E；y=Schlick (1-VoH)^5 的积分 B。
    std::vector<Vec2> magnitudeFresnel;
    std::string bakeKey;
};
struct LtcSample { Vec4 inverse;Vec2 magnitudeFresnel; };
struct RectangleEmitter
{
    Vec3 center,halfU,halfV;
    Vec3 radiance{1,1,1}; // 线性辐亮度，面积=4*|cross(U,V)|；仅 cross(U,V) 所指一侧发光。
};
bool ValidateLtcSettings(const LtcBakeSettings& settings);
std::string LtcBakeKey(const LtcBakeSettings& settings);
bool LtcUv(const LtcBakeSettings& settings,float nv,float roughness,Vec2& output);
bool SampleLtc(const LtcLut& lut,float nv,float roughness,LtcSample& output);
bool BakeLtc(const LtcBakeSettings& settings,LtcLut& output,std::vector<AssetDiagnostic>& diagnostics);
bool ValidateLtcLut(const LtcLut& lut);
bool EncodeLtc(const LtcLut& lut,std::vector<std::uint8_t>& output,std::vector<AssetDiagnostic>& diagnostics);
bool DecodeLtc(std::span<const std::uint8_t> bytes,LtcLut& output,std::vector<AssetDiagnostic>& diagnostics);
bool SaveLtc(const std::filesystem::path& path,const LtcLut& lut,std::vector<AssetDiagnostic>& diagnostics);
bool LoadLtc(const std::filesystem::path& path,LtcLut& output,std::vector<AssetDiagnostic>& diagnostics);
bool CookLtc(const std::filesystem::path& destination,const LtcBakeSettings& settings,bool& cacheHit,std::vector<AssetDiagnostic>& diagnostics);
// 单次散射 GGX 的 specular 近似：Le * [F0*E+(1-F0)*B] * 变换余弦积分。
// 不含 01 多散射项、不含遮挡、不含距离衰减额外乘项；方向均为离开 receiver 的世界单位向量。
// N、V 必须归一，F0∈[0,1]，Nv 在表域内；失败保留 output。
bool EvaluateLtcRectangle(const LtcLut& lut,const RectangleEmitter& emitter,Vec3 position,Vec3 normal,Vec3 view,float roughness,Vec3 f0,Vec3& output);
// specular 依次裁真实 N 半球、变换余弦半球；单面与顶点方向按本文件约定。
// 精确 horizon clipping 的 Lambert 几何积分（albedo=1），和 specular 近似独立。
bool EvaluateLambertRectangle(const RectangleEmitter& emitter,Vec3 position,Vec3 normal,Vec3& output);
}
