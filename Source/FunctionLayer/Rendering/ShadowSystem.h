#pragma once
#include "FunctionLayer/Application/FrameData.h"
#include "PlatformLayer/Graphics/Rhi.h"
#include "ResourceLayer/Assets/StaticModel.h"

namespace flare
{
// 使用 Forward 已选择的方向，不重新选择灯光；失败不替换上次有效配置。
bool PrepareDirectionalShadow(const DirectionalShadowView& view,const rhi::ForwardLighting& lighting,
    rhi::ShadowSettings& output,std::vector<AssetDiagnostic>& diagnostics);
// view/projection 与 lens 必须来自同一个相机；相机 far 与阴影范围分开处理。
bool PrepareDirectionalCascadeShadow(const DirectionalShadowView& view,const rhi::ForwardLighting& lighting,
    const CameraSnapshot& camera,rhi::ShadowSettings& output,std::vector<AssetDiagnostic>& diagnostics);
}
