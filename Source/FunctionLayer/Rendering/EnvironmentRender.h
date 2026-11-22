#pragma once
#include "ResourceLayer/Assets/AssetStore.h"
#include "FunctionLayer/Runtime/Camera.h"
#include "PlatformLayer/Graphics/Rhi.h"

namespace flare
{
struct RenderEnvironment
{
    std::shared_ptr<const EnvironmentAsset> source;
    std::array<rhi::Texture,3> textures;
};
// 仅保留当前有效版本；命令包负责使被替换的 GPU 版本活到自身 Fence。
class EnvironmentRenderCache
{
public:
    std::shared_ptr<const RenderEnvironment> Load(AssetStore& assets,const AssetId& id,rhi::Device& device,std::vector<AssetDiagnostic>& diagnostics);
    std::shared_ptr<const RenderEnvironment> EngineDefault(const std::filesystem::path& resources,rhi::Device& device,std::vector<AssetDiagnostic>& diagnostics);
    bool Upload(std::shared_ptr<const EnvironmentAsset> source,rhi::Device& device,std::shared_ptr<const RenderEnvironment>& output,std::vector<AssetDiagnostic>& diagnostics);
    std::shared_ptr<const RenderEnvironment> Fallback(rhi::Device& device,std::vector<AssetDiagnostic>& diagnostics);
    void Clear(){active_.reset();fallback_.reset();engineDefault_.reset();engineDefaultAttempted_=false;}
private:
    std::shared_ptr<const RenderEnvironment> active_,fallback_,engineDefault_;
    bool engineDefaultAttempted_=false;
};
rhi::EnvironmentBindings PrepareEnvironmentBindings(const RenderEnvironment& environment,const CameraSnapshot& camera,float intensity,float yaw,
    std::vector<AssetDiagnostic>& diagnostics);
}
