#include "MaterialSurfacePipelines.h"

namespace se
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
}
