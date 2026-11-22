#include "MaterialSurfacePipelines.h"

namespace flare
{
namespace
{
std::string ShaderName(rhi::PipelineKind kind,bool skin)
{
    using K=rhi::PipelineKind;
    switch(kind)
    {
    case K::PbrMesh:return "Pbr.hlsl";
    case K::ForwardMesh:return skin?"":"Forward.hlsl";
    case K::ShadowedMesh:return skin?"SkinnedForward.hlsl":"ShadowedForward.hlsl";
    case K::EnvironmentMesh:return skin?"SkinnedEnvironmentForward.hlsl":"EnvironmentForward.hlsl";
    case K::BasicLitMesh:return skin?"":"BasicLit.hlsl";
    case K::CascadedShadowedMesh:return skin?"CascadedSkinnedForward.hlsl":"CascadedShadowedForward.hlsl";
    case K::CascadedEnvironmentMesh:return skin?"CascadedSkinnedEnvironmentForward.hlsl":"CascadedEnvironmentForward.hlsl";
    case K::CascadedBasicLitMesh:return skin?"":"CascadedBasicLit.hlsl";
    case K::ClusteredForwardMesh:return skin?"ClusteredSkinnedForward.hlsl":"ClusteredForward.hlsl";
    case K::ClusteredShadowedMesh:return skin?"ClusteredSkinnedShadowedForward.hlsl":"ClusteredShadowedForward.hlsl";
    case K::ClusteredEnvironmentMesh:return skin?"ClusteredSkinnedEnvironmentForward.hlsl":"ClusteredEnvironmentForward.hlsl";
    case K::ClusteredBasicLitMesh:return skin?"":"ClusteredBasicLit.hlsl";
    case K::ClusteredCascadedShadowedMesh:return skin?"ClusteredCascadedSkinnedForward.hlsl":"ClusteredCascadedShadowedForward.hlsl";
    case K::ClusteredCascadedEnvironmentMesh:return skin?"ClusteredCascadedSkinnedEnvironmentForward.hlsl":"ClusteredCascadedEnvironmentForward.hlsl";
    case K::ClusteredCascadedBasicLitMesh:return skin?"":"ClusteredCascadedBasicLit.hlsl";
    case K::ShadowDepth:return skin?"SkinnedShadowDepth.hlsl":"ShadowDepth.hlsl";
    default:return {};
    }
}
}
rhi::Pipeline MaterialSurfacePipelineCache::Load(rhi::Device& device,const std::filesystem::path& shaders,
    rhi::PipelineKind kind,MaterialAlphaMode mode,bool skinned,bool filtered,bool compressed,bool normalBc5,bool shading)
{
    if(!IsValidMaterialAlphaMode(mode)||(mode==MaterialAlphaMode::Blend&&!rhi::IsForward(kind)))return {};
    const Key key{shaders,kind,mode,skinned,filtered,compressed,normalBc5,shading};
    if(const auto found=pipelines_.find(key);found!=pipelines_.end())return found->second;
    auto file=ShaderName(kind,skinned);if(file.empty())return {};
    if(filtered){if(!rhi::IsCascaded(kind)||!rhi::IsShadowed(kind))return {};file="Pcss"+file;}
    const auto layout=filtered?rhi::ShadowFilterBindingLayout(kind,skinned):skinned?
        (rhi::IsClustered(kind)?rhi::SkinnedClusteredLayoutVersion:rhi::IsCascaded(kind)?rhi::SkinnedCascadedLayoutVersion:rhi::SkinnedLayoutVersion):rhi::BindingLayout(kind);
    rhi::PipelineDescription description{kind,shaders/file,layout};
    description.materialLayoutVersion=mode!=MaterialAlphaMode::Opaque?rhi::MaterialSurfaceLayoutVersion:0;description.alphaMode=mode;
    description.shadingLayoutVersion=shading?rhi::ShadingExtensionLayoutVersion:0;
    description.colorFormat=rhi::IsForward(kind)?rhi::ColorFormat::Rgba16Float:rhi::ColorFormat::Rgba8Unorm;
    description.vertexLayoutVersion=skinned?rhi::VertexLayoutVersionSkinned:compressed?rhi::VertexLayoutVersionCompressed:rhi::VertexLayoutVersionLegacy;
    if(skinned)description.macros.push_back({"SE_SKINNED","1"});
    if(normalBc5)description.macros.push_back({"SE_NORMAL_BC5","1"});
    if(mode==MaterialAlphaMode::Blend){description.depth=rhi::DepthMode::ReadOnlyLessEqual;description.blend=rhi::BlendMode::Alpha;}
    rhi::Pipeline created;if(!device.CreatePipeline(description,created))return {};
    pipelines_.emplace(key,created);return created;
}
rhi::Pipeline MaterialSurfacePipelineCache::LoadUserSurface(rhi::Device& device,const std::filesystem::path& builtinShaders,
    const std::filesystem::path& cacheRoot,const UserSurfaceShaderSource& userShader,rhi::PipelineKind kind,
    MaterialAlphaMode mode,bool skinned,bool filtered,bool compressed,bool normalBc5,bool shading,
    std::vector<std::string>& outDiagnostics,const SurfaceShaderMetadata** outFallbackMetadata,
    std::size_t passIndex,rhi::BlendMode blendOverride,rhi::DepthMode depthOverride)
{
    if(outFallbackMetadata)*outFallbackMetadata=nullptr;
    if(!IsValidMaterialAlphaMode(mode)||(mode==MaterialAlphaMode::Blend&&!rhi::IsForward(kind)))return {};
    const rhi::BlendMode resolvedBlend=(mode==MaterialAlphaMode::Blend)
        ?(blendOverride==rhi::BlendMode::Additive?rhi::BlendMode::Additive:rhi::BlendMode::Alpha)
        :rhi::BlendMode::Opaque;
    const rhi::DepthMode resolvedDepth=(mode==MaterialAlphaMode::Blend)
        ?((depthOverride==rhi::DepthMode::ReadOnlyGreater||depthOverride==rhi::DepthMode::Disabled)?depthOverride:rhi::DepthMode::ReadOnlyLessEqual)
        :rhi::DepthMode::Default;

    const UserKey ukey{userShader.content_sha256,passIndex,kind,mode,resolvedBlend,resolvedDepth,skinned,filtered,compressed,normalBc5,shading};
    if(const auto found=userPipelines_.find(ukey);found!=userPipelines_.end())return found->second;

    const SourceFallbackKey fbKey{userShader.source,passIndex,kind,mode,resolvedBlend,resolvedDepth,skinned,filtered,compressed,normalBc5,shading};
    const auto fallback=[&]()->rhi::Pipeline
    {
        if(const auto prev=lastGoodUserPipelines_.find(fbKey);prev!=lastGoodUserPipelines_.end())
        {
            if(outFallbackMetadata)*outFallbackMetadata=&prev->second.metadata;
            return prev->second.pipeline;
        }
        return {};
    };
    if(failedUserKeys_.contains(ukey))return fallback();

    auto file=ShaderName(kind,skinned);
    if(file.empty()){failedUserKeys_.insert(ukey);return fallback();}
    if(filtered)
    {
        if(!rhi::IsCascaded(kind)||!rhi::IsShadowed(kind)){failedUserKeys_.insert(ukey);return fallback();}
        file="Pcss"+file;
    }

    std::filesystem::path stagedHlsl;
    std::string stagedKey,stageError;
    const std::filesystem::path effectiveCache=cacheRoot.empty()?(builtinShaders.parent_path()/".shader-cache"):(cacheRoot/"user-shaders");
    if(!StageSurfaceShaderVariant(builtinShaders,effectiveCache,userShader.source,userShader.metadata,file,stagedHlsl,stagedKey,stageError,passIndex))
    {
        outDiagnostics.push_back(stageError);
        failedUserKeys_.insert(ukey);
        return fallback();
    }

    const auto layout=filtered?rhi::ShadowFilterBindingLayout(kind,skinned):skinned?
        (rhi::IsClustered(kind)?rhi::SkinnedClusteredLayoutVersion:rhi::IsCascaded(kind)?rhi::SkinnedCascadedLayoutVersion:rhi::SkinnedLayoutVersion):rhi::BindingLayout(kind);
    rhi::PipelineDescription description{kind,stagedHlsl,layout};
    description.materialLayoutVersion=rhi::UserSurfaceMaterialLayoutVersion;
    description.alphaMode=mode;
    description.shadingLayoutVersion=shading?rhi::ShadingExtensionLayoutVersion:0;
    description.colorFormat=rhi::IsForward(kind)?rhi::ColorFormat::Rgba16Float:rhi::ColorFormat::Rgba8Unorm;
    description.vertexLayoutVersion=skinned?rhi::VertexLayoutVersionSkinned:compressed?rhi::VertexLayoutVersionCompressed:rhi::VertexLayoutVersionLegacy;
    if(skinned)description.macros.push_back({"SE_SKINNED","1"});
    if(normalBc5)description.macros.push_back({"SE_NORMAL_BC5","1"});
    description.blend=resolvedBlend;
    description.depth=resolvedDepth;

    (void)device.TakeShaderDiagnostics();
    rhi::Pipeline created;
    if(!device.CreatePipeline(description,created))
    {
        auto d3dDiags=device.TakeShaderDiagnostics();
        for(auto& d:d3dDiags)outDiagnostics.push_back(std::move(d));
        failedUserKeys_.insert(ukey);
        return fallback();
    }
    userPipelines_.emplace(ukey,created);
    lastGoodUserPipelines_[fbKey]=FallbackUserPipeline{created,userShader.metadata};
    return created;
}
}
