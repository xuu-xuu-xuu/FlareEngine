#pragma once
#include "FunctionLayer/Runtime/Camera.h"
#include "ResourceLayer/Assets/AssetStore.h"
#include "CoreLayer/Math/Bounds.h"
namespace se
{
// 编辑镜头不存入 World，不参与 Play 的相机选择。
class EditorViewportCamera
{
public:
    FreeCameraState state{{0,2,6},0,-0.2f};
    std::optional<CameraSnapshot> Snapshot(CameraViewport viewport,std::uint64_t frame) const;
    bool Update(const FreeCameraInput& input,double seconds,const FreeCameraSettings& settings={}){return UpdateFreeCamera(state,input,seconds,settings);}
};
// 用包围盒排除无关实例，再按静态网格的实际三角形和相机裁剪范围选择最近表面。
StableEntityId PickEditorBounds(const World& world,AssetStore& assets,const CameraSnapshot& camera,float x,float y);
std::optional<Aabb> EditorEntityBounds(const World& world,AssetStore& assets,EntityHandle entity);
}
