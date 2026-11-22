#pragma once
#include "FunctionLayer/Application/FrameData.h"
#include "PlatformLayer/Graphics/Rhi.h"
#include "ResourceLayer/Assets/StaticModel.h"
#include "FunctionLayer/Rendering/FrameGraph.h"

namespace flare
{
// 只消费一帧自有快照，稳定选择灯光并生成显式用途/顺序；不接触原生屏障。
rhi::ForwardLighting PrepareForwardLighting(std::span<const LightInstance> lights,float exposure,Vec3 sky,
    std::vector<AssetDiagnostic>& diagnostics,bool clustered=false);
rhi::Result RecordForwardFrame(rhi::CommandContext& commands,const rhi::ForwardLighting& lighting,
    rhi::PipelineHandle output,rhi::PipelineHandle sky,std::span<const rhi::DrawIndexed> draws,bool cacheBindings=false,
    std::span<const rhi::DrawIndexed> transparent={},const rhi::DepthNormalSettings* geometry=nullptr,const rhi::ScreenSpaceOcclusionSettings* occlusion=nullptr);
rhi::Result RecordShadowedFrame(rhi::CommandContext& commands,const rhi::ForwardLighting& lighting,
    const rhi::ShadowSettings& shadows,rhi::PipelineHandle depth,rhi::PipelineHandle output,rhi::PipelineHandle sky,
    std::span<const rhi::DrawIndexed> visible,std::span<const rhi::DrawIndexed> casters,const rhi::EnvironmentBindings* environment=nullptr,
    const rhi::PostProcessSettings* post=nullptr,const rhi::PostProcessPipelines* postPipelines=nullptr,std::span<const rhi::OverlayQuad> overlays={},bool cacheBindings=false,
    std::span<const rhi::DrawIndexed> transparent={},const rhi::DepthNormalSettings* geometry=nullptr,const rhi::ScreenSpaceOcclusionSettings* occlusion=nullptr);
// G13：同一组 Pass 先经 Frame Graph 声明/编译，再按编译顺序录制；屏障仍由 RHI 生成。
// 图只在本帧有效，调用方保活到函数返回；编译失败返回 InvalidArgument/CapacityExceeded。
rhi::Result RecordForwardFrameGraph(rhi::CommandContext& commands,const rhi::ForwardLighting& lighting,
    rhi::PipelineHandle output,rhi::PipelineHandle sky,std::span<const rhi::DrawIndexed> draws,bool cacheBindings,
    FrameGraph& graph,std::uint32_t width,std::uint32_t height,std::span<const rhi::DrawIndexed> transparent={},
    const rhi::DepthNormalSettings* geometry=nullptr,const rhi::ScreenSpaceOcclusionSettings* occlusion=nullptr);
rhi::Result RecordShadowedFrameGraph(rhi::CommandContext& commands,const rhi::ForwardLighting& lighting,
    const rhi::ShadowSettings& shadows,rhi::PipelineHandle depth,rhi::PipelineHandle output,rhi::PipelineHandle sky,
    std::span<const rhi::DrawIndexed> visible,std::span<const rhi::DrawIndexed> casters,const rhi::EnvironmentBindings* environment,
    const rhi::PostProcessSettings* post,const rhi::PostProcessPipelines* postPipelines,std::span<const rhi::OverlayQuad> overlays,
    bool cacheBindings,FrameGraph& graph,std::uint32_t width,std::uint32_t height,std::span<const rhi::DrawIndexed> transparent={},
    const rhi::DepthNormalSettings* geometry=nullptr,const rhi::ScreenSpaceOcclusionSettings* occlusion=nullptr);
}
