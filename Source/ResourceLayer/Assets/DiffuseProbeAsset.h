#pragma once
#include "DiffuseProbeBake.h"
#include <optional>

namespace flare
{
inline constexpr const char* DiffuseProbeCookerVersion="FlareEngine.DiffuseProbe.1";
struct DiffuseProbeSource
{
    std::string path,sha256; // 逻辑根下的相对 UTF-8 路径；绝不保存机器绝对路径。
};
struct DiffuseProbeAsset
{
    Sh9Rgb globalDiffuse; // 已做 cosine 卷积的 E/pi，白环境为 1，允许有符号 SH 系数。
    std::optional<DiffuseProbeGrid> local;
    DiffuseProbeBakeSettings bakeSettings;
    std::string environmentSourceHash,localSourceHash;
    std::vector<DiffuseProbeSource> sources;
};
bool ValidateDiffuseProbeAsset(const DiffuseProbeAsset& asset);
bool EncodeDiffuseProbeAsset(const DiffuseProbeAsset& asset,std::vector<std::uint8_t>& output,std::vector<AssetDiagnostic>& diagnostics);
bool DecodeDiffuseProbeAsset(std::span<const std::uint8_t> bytes,DiffuseProbeAsset& output,std::vector<AssetDiagnostic>& diagnostics);
bool SaveDiffuseProbeAsset(const std::filesystem::path& path,const DiffuseProbeAsset& asset,std::vector<AssetDiagnostic>& diagnostics);
bool LoadDiffuseProbeAsset(const std::filesystem::path& path,DiffuseProbeAsset& output,std::vector<AssetDiagnostic>& diagnostics);
}
