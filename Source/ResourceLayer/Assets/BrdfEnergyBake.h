#pragma once
#include "HdrImage.h"

namespace se
{
inline constexpr const char* BrdfEnergyCookerVersion="SelfEngine.BrdfEnergy.1/ggx-correlated-schlick-vndf2018";
inline constexpr float BrdfEnergyMinimumRoughness=.045f,BrdfEnergyMinimumNv=.0001f;
struct BrdfEnergySettings
{
    std::uint32_t width=256,height=128,samples=32768;
    bool operator==(const BrdfEnergySettings&) const=default;
};
struct BrdfEnergyLut
{
    BrdfEnergySettings settings;
    std::vector<Vec2> directional; // x=Nv，y=感知 roughness；每项 A/B，E=A+B。
    std::vector<Vec2> average; // 每个 roughness 行的精确 cosine-weighted 平均 A/B。
    std::string bakeKey;
};
bool ValidateBrdfEnergySettings(const BrdfEnergySettings& settings);
std::string BrdfEnergyBakeKey(const BrdfEnergySettings& settings);
// Nv 的 LUT 坐标为 (sqrt(Nv)-.01)/.99；roughness 坐标为 (r-.045)/.955。
// 该函数返回含半 texel 的 2D normalized UV；输入必须在生产域内，失败保留 output。
bool BrdfEnergyUv(const BrdfEnergySettings& settings,float nv,float roughness,Vec2& output);
bool SampleBrdfEnergy(const BrdfEnergyLut& lut,float nv,float roughness,Vec2& directional,Vec2& average);
// 单点参考同生产 VNDF、有界 G2/G1 权重。失败不改 output。
bool EstimateBrdfEnergy(float nv,float roughness,std::uint32_t samples,Vec2& output);
// 每个 texel 按固定顺序归约，threads 不改变内容身份或输出位值。
bool BakeBrdfEnergy(const BrdfEnergySettings& settings,BrdfEnergyLut& output,std::vector<AssetDiagnostic>& diagnostics,std::uint32_t threads=1);
bool ValidateBrdfEnergyLut(const BrdfEnergyLut& lut);
bool EncodeBrdfEnergy(const BrdfEnergyLut& lut,std::vector<std::uint8_t>& output,std::vector<AssetDiagnostic>& diagnostics);
bool DecodeBrdfEnergy(std::span<const std::uint8_t> bytes,BrdfEnergyLut& output,std::vector<AssetDiagnostic>& diagnostics);
bool SaveBrdfEnergy(const std::filesystem::path& path,const BrdfEnergyLut& lut,std::vector<AssetDiagnostic>& diagnostics);
bool LoadBrdfEnergy(const std::filesystem::path& path,BrdfEnergyLut& output,std::vector<AssetDiagnostic>& diagnostics);
bool CookBrdfEnergy(const std::filesystem::path& destination,const BrdfEnergySettings& settings,bool& cacheHit,
    std::vector<AssetDiagnostic>& diagnostics,std::uint32_t threads=1);
}
