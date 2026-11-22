#pragma once
#include "FunctionLayer/Application/FrameData.h"
#include "EnvironmentRender.h"
#include "ResourceLayer/Assets/DiffuseProbeAsset.h"
#include <memory>

namespace flare
{
// 只检查本帧实际引用的 CPU 材质/预览；加载失败保守要求扩展，避免旧有效层材质选到旧 PSO。
bool RequiresMaterialShading(const FrameData& frame);
class ShadingResourceCache
{
public:
    ShadingResourceCache();
    ~ShadingResourceCache();
    ShadingResourceCache(const ShadingResourceCache&)=delete;
    ShadingResourceCache& operator=(const ShadingResourceCache&)=delete;
    // 只读离线资产，不在渲染线程 Bake。可选资源失败保留其上一有效版本并清不可用 flags。
    bool Prepare(const FrameData& frame,const CameraSnapshot& camera,const RenderEnvironment* environment,rhi::Device& device,
        const std::filesystem::path& resourceDirectory,rhi::ShadingFrameBindings& output,std::vector<AssetDiagnostic>& diagnostics);
    void Clear();
    // 导出/来源检查可读取原始相对来源记录；着色不依赖 provenance 源文件仍存在。
    const std::vector<DiffuseProbeSource>& ProbeSources() const noexcept;
    const std::filesystem::path& ProbeAssetPath() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
