#pragma once
#include "CoreLayer/MaterialAlpha.h"
#include "CoreLayer/MaterialLayers.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace se
{
using AssetId = std::string; // 128 位随机 ID，以 32 个小写十六进制字符保存。
// CPU 中间数据，右手 Y-up、米，矩阵按列存储并乘列向量；不是 GPU 顶点布局。
struct MeshVertex
{
    std::array<float, 3> position{};
    std::array<float, 3> normal{ 0.0f, 1.0f, 0.0f };
    std::array<float, 2> uv{};
    std::array<float, 4> tangent{ 1.0f, 0.0f, 0.0f, 1.0f };
};

struct TextureReference
{
    std::string relativePath; // 相对模型文件目录；不保存个人机器绝对路径。
    std::string mimeType;
    std::vector<std::uint8_t> embeddedBytes;
    bool missing = false;
};

struct MaterialData
{
    std::string name;
    std::array<float, 4> baseColor{ 1.0f, 1.0f, 1.0f, 1.0f };
    float metallic = 0.0f;
    float roughness = 1.0f;
    std::int32_t baseColorTexture = -1;
    std::int32_t normalTexture = -1;
    std::int32_t metallicRoughnessTexture = -1;
    bool doubleSided = false;
    bool needsFallbackTexture = false;
    float normalScale = 1.0f;
    // opacity 只有 baseColor[3]；覆盖率为 factor alpha × baseColor texture alpha。
    MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque;
    float alphaCutoff = 0.5f;
    ClearCoatParameters clearCoat;
    // 三者均为线性数据：weight 取 R，roughness 取 G，normal 取切线空间 RGB。
    std::int32_t clearCoatWeightTexture=-1,clearCoatRoughnessTexture=-1,clearCoatNormalTexture=-1;
    SheenParameters sheen;
    AnisotropyParameters anisotropy;
    SubsurfaceParameters subsurface;
    // sheenColor：sRGB RGB；sheenRoughness：保 alpha 的线性 A；anisotropy：线性 RG 方向、B 强度。
    std::int32_t sheenColorTexture=-1,sheenRoughnessTexture=-1,anisotropyTexture=-1;
};

// 每顶点蒙皮权重，与 MeshData::vertices 一一对应；静态网格为空。
struct MeshSkinVertex
{
    std::uint16_t joints[4]{};
    float weights[4]{};
};

struct MeshData
{
    std::string name;
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::int32_t material = -1;
    std::vector<MeshSkinVertex> skinning; // 空：静态网格；否则大小与 vertices 相同。
    bool skinned = false;
    std::uint32_t meshNode = 0; // 引用该网格的节点下标；用于定位导入时烘进的网格节点变换。
};

// 关节层级按父序排列：parent < 自身下标；矩阵列主序、列向量。
struct ModelJoint
{
    std::string name;
    std::int32_t parent = -1; // joints 下标；根关节为 -1，父关节下标小于自身。
    std::array<float, 16> inverseBind{}; // 实体（网格）局部绑定空间，列主序。
    std::array<float, 3> restTranslation{};
    std::array<float, 4> restRotation{ 0, 0, 0, 1 }; // x,y,z,w
    std::array<float, 3> restScale{ 1, 1, 1 };
};

enum class ModelAnimationPath : std::uint8_t { Translation = 0, Rotation = 1, Scale = 2 };
enum class ModelAnimationInterpolation : std::uint8_t { Linear = 0, Step = 1 };
struct ModelAnimationChannel
{
    std::uint16_t joint = 0;
    ModelAnimationPath path = ModelAnimationPath::Rotation;
    ModelAnimationInterpolation interpolation = ModelAnimationInterpolation::Linear;
    std::vector<float> times; // 秒，严格递增。
    std::vector<std::array<float, 4>> values; // rotation 为四元数 xyzw；translation/scale 取 xyz。
};
struct ModelAnimationClip
{
    std::string name;
    float duration = 0; // 秒，所有采样输入时间的最大值。
    std::vector<ModelAnimationChannel> channels;
};

struct ModelNode
{
    std::string name;
    std::int32_t parent = -1;
    std::array<float, 16> localMatrix{
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    std::vector<std::uint32_t> meshes;
};

struct StaticModel
{
    std::vector<MeshData> meshes;
    std::vector<MaterialData> materials;
    std::vector<ModelNode> nodes;
    std::vector<TextureReference> textures;
    std::vector<std::string> dependencies; // 相对模型目录的外部缓冲/贴图，包含缺失项。
    std::vector<ModelJoint> joints; // 每份资产一套骨架；静态模型为空。
    std::vector<ModelAnimationClip> animations;
};

struct AssetDiagnostic
{
    std::string key;
    std::string detail;
    bool warning = false;
};

struct ImportOptions
{
    float scale = 1.0f; // 在源单位归一到米之后，额外缩放几何及节点平移。
    bool generateTangents = true;
    std::filesystem::path allowedRoot; // 空值表示模型所在目录；外部引用不得越界。
};

inline constexpr std::uint32_t StaticModelFormatVersion = 2;
inline constexpr const char* StaticModelImporterVersion = "SelfEngine.StaticModel.2/ufbx-0.17.1/cgltf-1.15/skin1/alpha1/clearcoat1/sheen1/aniso1/skinDiffuse1";

// 所有数据由引擎独立拥有；失败不改变 output。诊断只携带稳定文本键与细节。
bool ImportStaticModel(const std::filesystem::path& path, const ImportOptions& options,
    StaticModel& output, std::vector<AssetDiagnostic>& diagnostics);
bool ValidateStaticModel(const StaticModel& model);
}
