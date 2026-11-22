#pragma once
#include "StaticModel.h"
#include <map>
#include <string_view>

namespace flare
{
// 独立 CPU 材质资产；贴图只引用 AssetId，实际 Shader/PSO/描述符由渲染器决定。
struct MaterialDocument
{
    std::string name;
    std::array<float,4> baseColor{1,1,1,1};
    float metallic=0,roughness=1;
    bool doubleSided=false;
    AssetId baseColorTexture,normalTexture,metallicRoughnessTexture;
    float normalScale=1;
    // 可选用户 Shader 资产（.sebin、.fshader.hlsl 或 .fshader.json）；空表示内置默认 PBR，旧材质缺省行为不变。
    AssetId shader;
    // 可选自定义着色器材质参数覆盖（键为 @Param 参数名，值为最多 4 分量浮点）。为空时不写入 JSON。
    std::map<std::string, Vec4> shaderParameters;
    // 沿用 baseColor[3] 为唯一因子 alpha；贴图 alpha 与该因子相乘，不复制 opacity。
    MaterialAlphaMode alphaMode=MaterialAlphaMode::Opaque;
    float alphaCutoff=0.5f;
    ClearCoatParameters clearCoat;
    AssetId clearCoatWeightTexture,clearCoatRoughnessTexture,clearCoatNormalTexture;
    SheenParameters sheen;
    AnisotropyParameters anisotropy;
    SubsurfaceParameters subsurface;
    AssetId sheenColorTexture,sheenRoughnessTexture,anisotropyTexture;
};
// 编辑器当前帧的自有 CPU 预览，不改变源文件或资源身份。
struct MaterialPreview { AssetId id;MaterialDocument document; };
bool IsMaterialDocumentPath(const std::filesystem::path& path);
bool ValidateMaterialDocument(const MaterialDocument& material);
std::vector<AssetId> MaterialDependencies(const MaterialDocument& material);
std::vector<AssetId> MaterialTextureDependencies(const MaterialDocument& material);
bool SerializeMaterialDocument(const MaterialDocument& material,std::string& output,std::vector<AssetDiagnostic>& diagnostics);
bool DeserializeMaterialDocument(std::string_view text,MaterialDocument& output,std::vector<AssetDiagnostic>& diagnostics);
bool SaveMaterialDocument(const std::filesystem::path& path,const MaterialDocument& material,std::vector<AssetDiagnostic>& diagnostics);
bool LoadMaterialDocument(const std::filesystem::path& path,MaterialDocument& output,std::vector<AssetDiagnostic>& diagnostics);
}
