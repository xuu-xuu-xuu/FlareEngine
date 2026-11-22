#pragma once
#include "HdrImage.h"

namespace flare
{
// 六 Gaussian 归一化球面扩散；仅预积分 diffuse，不含厚度、透射、屏幕模糊或跨对象采样。
inline constexpr const char* SkinDiffuseCookerVersion="FlareEngine.SkinDiffuse.1/normalized-six-gaussian-sphere/exponential-cdf";
inline constexpr float SkinDiffuseMaximumCurvature=8;
struct SkinDiffuseSettings
{
    // 奇数 width 使 NoL=0 落在节点上，q=0 行双线性插值仍精确退化 Lambert。
    std::uint32_t width=257,height=128,samples=4096;
    bool operator==(const SkinDiffuseSettings&) const=default;
};
struct SkinDiffuseLut
{
    SkinDiffuseSettings settings;
    std::vector<Vec3> pixels; // x=NoL [-1,1]，y=sqrt(q/8)，q=米制 radius × 曲率(1/m)。
    std::string bakeKey;
};
bool ValidateSkinDiffuseSettings(const SkinDiffuseSettings& settings);
std::string SkinDiffuseBakeKey(const SkinDiffuseSettings& settings);
bool SkinDiffuseUv(const SkinDiffuseSettings& settings,float signedNoL,float scaledCurvature,Vec2& output);
bool SampleSkinDiffuse(const SkinDiffuseLut& lut,float signedNoL,float scaledCurvature,Vec3& output);
// 单点生产积分入口（与烘焙共用算法，不是独立 oracle）；输出是替代 NoL 的 RGB 因子。
bool EstimateSkinDiffuse(float signedNoL,float scaledCurvature,std::uint32_t samples,Vec3& output);
bool BakeSkinDiffuse(const SkinDiffuseSettings& settings,SkinDiffuseLut& output,std::vector<AssetDiagnostic>& diagnostics,std::uint32_t threads=1);
bool ValidateSkinDiffuseLut(const SkinDiffuseLut& lut);
bool EncodeSkinDiffuse(const SkinDiffuseLut& lut,std::vector<std::uint8_t>& output,std::vector<AssetDiagnostic>& diagnostics);
bool DecodeSkinDiffuse(std::span<const std::uint8_t> bytes,SkinDiffuseLut& output,std::vector<AssetDiagnostic>& diagnostics);
bool SaveSkinDiffuse(const std::filesystem::path& path,const SkinDiffuseLut& lut,std::vector<AssetDiagnostic>& diagnostics);
bool LoadSkinDiffuse(const std::filesystem::path& path,SkinDiffuseLut& output,std::vector<AssetDiagnostic>& diagnostics);
bool CookSkinDiffuse(const std::filesystem::path& path,const SkinDiffuseSettings& settings,bool& cacheHit,std::vector<AssetDiagnostic>& diagnostics,std::uint32_t threads=1);
}
