#pragma once
#include "ResourceLayer/Assets/DiffuseProbeAsset.h"

namespace se
{
class World;
class AssetStore;
struct PreparedDiffuseProbeScene
{
    std::vector<DiffuseBakeTriangle> triangles;
    std::vector<DiffuseBakePointLight> lights;
    std::vector<std::uint64_t> sourceEntityIds;
    std::vector<DiffuseProbeSource> sources;
    std::string contentHash;
    DiffuseProbeScene View(const FloatImage* environment=nullptr) const
    {return {triangles,lights,environment};}
};
// 只在 World/AssetStore 的所属线程准备；成功后所有几何、灯和来源身份均为自有 CPU 值。
// 所有有效静态面都参与遮挡（不做相机可见剔除）；unsupported 来源使整次准备失败并保留旧 output。
// uniform Opaque 的 Lambert albedo=baseColor.rgb*(1-metallic)，不含 Fresnel/镜面。
// builtin 要求显式 uniform 材质覆盖，避免不同产品场景的默认 checker 差异。
bool PrepareDiffuseProbeScene(const World& world,AssetStore& assets,PreparedDiffuseProbeScene& output,
    std::vector<AssetDiagnostic>& diagnostics);
}
