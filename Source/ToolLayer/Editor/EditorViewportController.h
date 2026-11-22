#pragma once
#include "EditorGizmo.h"
#include "EditorViewportCamera.h"
#include "FunctionLayer/UI/Core/UiInput.h"
#include "FunctionLayer/Rendering/UiPaint.h"
#include "PlatformLayer/Graphics/Rhi.h"
#include <array>

namespace flare
{
// 视口交互拥有者：所有坐标为逻辑像素，渲染/拾取/手柄共用同一 CameraSnapshot。
class EditorViewportController
{
public:
    explicit EditorViewportController(EditorViewportCamera& camera):camera_(camera){}
    void Prepare(EditorSession&,AssetStore&,ui::UiRect,std::uint64_t frame);
    void SetSceneViewDuringPlay(bool enabled)noexcept{sceneViewDuringPlay_=enabled;}
    [[nodiscard]] bool SceneViewDuringPlay()const noexcept{return sceneViewDuringPlay_;}
    void Update(double seconds,EditorSession&,AssetStore&);
    bool Handle(ui::UiInputEvent&,EditorSession&,AssetStore&,bool canStart);
    std::optional<Vec2> TakeContextClick();
    void Cancel(EditorSession&);
    void Blur(EditorSession&);
    bool Action(std::string_view action,EditorSession&,AssetStore&);
    bool FrameSelection(EditorSession&,AssetStore&,bool follow=false);
    const std::optional<CameraSnapshot>& Camera()const{return snapshot_;}
    const ui::UiDisplayList& Overlay()const{return overlay_;}
    const std::vector<rhi::SceneLine>& SceneLines()const{return sceneLines_;}
    bool Captured()const{return gesture_!=Gesture::None;}
    bool Flying()const{return gesture_==Gesture::Fly;}
    bool Focused()const{return focused_;}
    int Tool()const{return viewTool_?0:static_cast<int>(mode_)+1;}
    bool Local()const{return space_==GizmoSpace::Local;}
    bool Center()const{return center_;}
    bool Sky()const{return sky_;}
    bool Grid()const{return grid_;}
    bool Gizmos()const{return gizmos_;}
    float Speed()const{return speed_;}
    [[nodiscard]] std::size_t ComponentHandleCount()const noexcept{return componentHandles_.size();}
    [[nodiscard]] std::optional<Vec2> ComponentHandleScreenPos(std::string_view propertyKey)const;
    const std::string& LastError()const{return gizmo_.LastError();}
private:
    enum class Gesture {None,Fly,Pan,Orbit,Zoom,Gizmo,ComponentHandle};
    enum class PickIntent { Surface, Handle, Context };
    struct HandleSegment {Vec2 a,b;int axis;};
    struct ComponentHandleDot
    {
        Vec2 screen{};
        Vec3 worldOrigin{};
        Vec3 worldAxis{};
        std::string propertyKey;
        float initialValue = 0.0f;
        float minVal = 0.001f;
        float maxVal = 10000.0f;
        float scaleFactor = 1.0f;
        ui::UiColor color{1.0f, 0.82f, 0.28f, 1.0f};
    };
    void Draw(EditorSession&,AssetStore&);
    void AppendComponentHandles(const World&,EntityHandle,const CameraSnapshot&);
    void SelectAt(Vec2 mouse, EditorSession&, AssetStore&, PickIntent intent = PickIntent::Surface, bool toggle = false);
    void Dolly(float amount);
    const World& ActiveWorld(const EditorSession&)const noexcept;
    Vec3 Pivot(const World&,AssetStore&,EntityHandle)const;
    std::optional<Aabb> SceneBounds(const World&,AssetStore&)const;
    EditorViewportCamera& camera_;
    EditorGizmo gizmo_;
    std::optional<CameraSnapshot> snapshot_;
    ui::UiRect viewport_{};
    ui::UiDisplayList overlay_;
    std::vector<rhi::SceneLine> sceneLines_;
    std::vector<HandleSegment> handles_;
    std::vector<ComponentHandleDot> componentHandles_;
    std::optional<ComponentHandleDot> activeComponentHandle_;
    int hoverComponentHandle_ = -1;
    Transform multiPrimaryBefore_{};
    std::vector<std::pair<StableEntityId, Transform>> multiFollowersBefore_;
    std::array<bool,256> keys_{};
    Gesture gesture_=Gesture::None;
    GizmoMode mode_=GizmoMode::Translate;
    GizmoSpace space_=GizmoSpace::World;
    bool focused_=false,viewTool_=false,center_=false,sky_=true,grid_=true,gizmos_=true,initialized_=false,sceneViewDuringPlay_=false;
    int dragAxis_=-1,hoverAxis_=-1;
    std::uint8_t dragButton_=0;
    Vec2 lastMouse_{};
    Vec2 pressedMouse_{};
    bool clickPending_ = false;
    std::optional<Vec2> lastPick_, contextClick_;
    Vec3 pivot_{};
    StableEntityId follow_=0;
    Vec3 followPosition_{};
    float distance_=6,speed_=3;
};
}
