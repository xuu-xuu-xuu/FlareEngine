#pragma once
#include "FunctionLayer/Runtime/Camera.h"
#include "FunctionLayer/Rendering/UiPaint.h"
#include "PlatformLayer/Graphics/Rhi.h"

namespace se
{
inline constexpr float EditorSceneIconRadius = 13.0f;

// 绘制与拾取共用同一逻辑像素锚点，图标放在实体原点上方以避开变换手柄。
std::optional<Vec2> EditorSceneIconCenter(const World& world, EntityHandle entity,
                                        const CameraSnapshot& editorCamera);

// 调用方控制视口裁剪。范围线使用世界坐标，由现有场景辅助线通道做裁剪与遮挡。
void AppendEditorSceneGizmos(const World& world, StableEntityId selected,
                            const CameraSnapshot& editorCamera, ui::UiDisplayList& overlay,
                            std::vector<rhi::SceneLine>& lines);

// 向场景辅助线通道提交视口拖放包围盒预览，不直接接触 D3D12。
void AppendDropPreviewGizmo(const Aabb& worldBounds, bool valid, std::vector<rhi::SceneLine>& lines);
}
