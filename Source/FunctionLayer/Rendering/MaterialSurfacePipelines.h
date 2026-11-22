#pragma once
#include "PlatformLayer/Graphics/Rhi.h"
#include "ResourceLayer/Assets/AssetStore.h"
#include <map>
#include <set>
#include <tuple>

namespace flare
{
// 与基底灯光/阴影布局正交的材质覆盖PSO缓存。只创建场景真正用到的组合。
class MaterialSurfacePipelineCache
{
public:
    rhi::Pipeline Load(rhi::Device& device,const std::filesystem::path& shaders,rhi::PipelineKind kind,
        MaterialAlphaMode mode,bool skinned=false,bool filtered=false,bool compressed=false,bool normalBc5=false,bool shading=false);
    rhi::Pipeline LoadUserSurface(rhi::Device& device,const std::filesystem::path& builtinShaders,
        const std::filesystem::path& cacheRoot,const UserSurfaceShaderSource& userShader,rhi::PipelineKind kind,
        MaterialAlphaMode mode,bool skinned,bool filtered,bool compressed,bool normalBc5,bool shading,
        std::vector<std::string>& outDiagnostics,const SurfaceShaderMetadata** outFallbackMetadata=nullptr,
        std::size_t passIndex=0,rhi::BlendMode blendOverride=rhi::BlendMode::Opaque,
        rhi::DepthMode depthOverride=rhi::DepthMode::Default);
    void Clear() noexcept
    {
        pipelines_.clear();
        userPipelines_.clear();
        lastGoodUserPipelines_.clear();
        failedUserKeys_.clear();
    }
    std::size_t Count() const noexcept { return pipelines_.size() + userPipelines_.size(); }
private:
    struct FallbackUserPipeline
    {
        rhi::Pipeline pipeline;
        SurfaceShaderMetadata metadata;
    };
    using Key=std::tuple<std::filesystem::path,rhi::PipelineKind,MaterialAlphaMode,bool,bool,bool,bool,bool>;
    using UserKey=std::tuple<std::string,std::size_t,rhi::PipelineKind,MaterialAlphaMode,rhi::BlendMode,rhi::DepthMode,bool,bool,bool,bool,bool>;
    using SourceFallbackKey=std::tuple<std::filesystem::path,std::size_t,rhi::PipelineKind,MaterialAlphaMode,rhi::BlendMode,rhi::DepthMode,bool,bool,bool,bool,bool>;
    std::map<Key,rhi::Pipeline> pipelines_;
    std::map<UserKey,rhi::Pipeline> userPipelines_;
    std::map<SourceFallbackKey,FallbackUserPipeline> lastGoodUserPipelines_;
    std::set<UserKey> failedUserKeys_;
};
}
