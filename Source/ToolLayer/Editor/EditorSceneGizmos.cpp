#include "EditorSceneGizmos.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace se
{
namespace
{
constexpr ui::UiColor CameraColor{.36f, .8f, 1, 1};
constexpr ui::UiColor LightColor{1, .78f, .23f, 1};
constexpr float IconOffset = 22.0f;

bool Active(const World& world, EntityHandle entity, ComponentTypeId type)
{
    const auto* component = world.GetComponent(world.FindComponent(entity, type));
    return component && component->Active();
}

void SceneLine(std::vector<rhi::SceneLine>& lines, Vec3 a, Vec3 b, ui::UiColor color)
{
    if (IsFinite(a) && IsFinite(b))
        lines.push_back({a, b, {color.r, color.g, color.b, color.a}, 1.6f, .22f});
}

void IconRing(ui::UiDisplayList& overlay, Vec2 center, float radius, ui::UiColor color, float width)
{
    constexpr int Segments = 24;
    for (int i = 0; i < Segments; ++i)
    {
        const float a = 2 * Pi * i / Segments;
        const float b = 2 * Pi * (i + 1) / Segments;
        overlay.Line(center.x + std::cos(a) * radius, center.y + std::sin(a) * radius,
                     center.x + std::cos(b) * radius, center.y + std::sin(b) * radius, width, color);
    }
}

void CameraIcon(ui::UiDisplayList& overlay, Vec2 center, ui::UiColor color)
{
    const auto line = [&](float x0, float y0, float x1, float y1) {
        overlay.Line(center.x + x0, center.y + y0, center.x + x1, center.y + y1, 1.7f, color);
    };
    // 摄像机机身、镜头和顶部取景器。
    line(-9, -5, 4, -5); line(4, -5, 4, 5);
    line(4, 5, -9, 5); line(-9, 5, -9, -5);
    line(4, -3, 10, -7); line(10, -7, 10, 7); line(10, 7, 4, 3);
    line(-6, -5, -4, -8); line(-4, -8, 0, -8); line(0, -8, 2, -5);
}

void LightIcon(ui::UiDisplayList& overlay, Vec2 center, ui::UiColor color, bool badge)
{
    const float scale = badge ? .45f : 1.0f;
    overlay.CanvasCircle(center.x, center.y, 3.5f * scale, color);
    for (int i = 0; i < 8; ++i)
    {
        const float angle = i * Pi / 4;
        const Vec2 ray{std::cos(angle) * scale, std::sin(angle) * scale};
        overlay.Line(center.x + ray.x * 6, center.y + ray.y * 6,
                     center.x + ray.x * 10, center.y + ray.y * 10, 1.6f * scale, color);
    }
}

void DrawCameraFrustum(const World& world, EntityHandle entity, const CameraSnapshot& editor,
                       std::vector<rhi::SceneLine>& lines)
{
    CameraInstance instance{world.Get(entity)->stableId, world.GetWorldMatrix(entity), *world.GetCamera(entity)};
    for (auto ancestor = entity; ancestor; ancestor = world.Get(ancestor)->parent)
    {
        const auto* transform = world.GetTransform(ancestor);
        if (!transform || !HasUnitScale(transform->scale)) instance.rigidHierarchy = false;
    }
    CameraSnapshot camera;
    CameraError error;
    if (!BuildCameraSnapshot(instance, editor.viewport, editor.frameIndex, camera, error)) return;

    std::array<Vec3, 8> corners;
    // 直接按真实裁剪距离求角点，避免对 NDC far=1 反投影时的浮点量化。
    for (int plane = 0; plane < 2; ++plane)
    {
        const float depth = plane ? camera.lens.farPlane : camera.lens.nearPlane;
        const float halfWidth = depth / camera.projection(0, 0);
        const float halfHeight = depth / camera.projection(1, 1);
        const Vec3 local[]{{-halfWidth, -halfHeight, -depth}, {halfWidth, -halfHeight, -depth},
                           {halfWidth, halfHeight, -depth}, {-halfWidth, halfHeight, -depth}};
        for (int corner = 0; corner < 4; ++corner)
            corners[plane * 4 + corner] = TransformPoint(camera.inverseView, local[corner]);
    }
    for (int corner = 0; corner < 4; ++corner)
    {
        const int next = (corner + 1) % 4;
        SceneLine(lines, corners[corner], corners[next], CameraColor);
        SceneLine(lines, corners[corner + 4], corners[next + 4], CameraColor);
        SceneLine(lines, corners[corner], corners[corner + 4], CameraColor);
    }
}

void Arrow(std::vector<rhi::SceneLine>& lines, Vec3 origin, Vec3 direction, Vec3 side, Vec3 up, float length)
{
    const auto tip = origin + direction * length;
    const auto neck = tip - direction * length * .2f;
    const float head = length * .09f;
    SceneLine(lines, origin, tip, LightColor);
    SceneLine(lines, tip, neck + side * head, LightColor);
    SceneLine(lines, tip, neck - side * head, LightColor);
    SceneLine(lines, tip, neck + up * head, LightColor);
    SceneLine(lines, tip, neck - up * head, LightColor);
}

void DrawLight(const World& world, EntityHandle entity, const CameraSnapshot& editor,
               std::vector<rhi::SceneLine>& lines)
{
    const auto& light = *world.GetLight(entity);
    const auto matrix = world.GetWorldMatrix(entity);
    const auto origin = TransformPoint(matrix, {});
    if (!IsFinite(origin)) return;
    if (light.type == LightType::Point)
    {
        // 点光源的世界空间范围与渲染器一致，不受实体缩放影响。
        const float range = std::isfinite(light.range) ? std::clamp(light.range, .05f, 10000.0f) : .05f;
        constexpr Vec3 first[]{{1, 0, 0}, {1, 0, 0}, {0, 1, 0}};
        constexpr Vec3 second[]{{0, 1, 0}, {0, 0, 1}, {0, 0, 1}};
        constexpr int Segments = 64;
        for (int plane = 0; plane < 3; ++plane)
            for (int i = 0; i < Segments; ++i)
            {
                const float a = i * 2 * Pi / Segments;
                const float b = (i + 1) * 2 * Pi / Segments;
                SceneLine(lines, origin + (first[plane] * std::cos(a) + second[plane] * std::sin(a)) * range,
                          origin + (first[plane] * std::cos(b) + second[plane] * std::sin(b)) * range, LightColor);
            }
        return;
    }
    // 渲染器的 +Z 是指向光源方向，箭头表示实际向场景传播的光线方向。
    const auto toLight = TransformVector(matrix, {0, 0, 1});
    const double magnitude = std::sqrt(double(toLight.x) * toLight.x + double(toLight.y) * toLight.y +
                                       double(toLight.z) * toLight.z);
    if (!IsFinite(toLight) || !std::isfinite(magnitude) || magnitude < 1e-8) return;
    const Vec3 direction{-float(toLight.x / magnitude), -float(toLight.y / magnitude), -float(toLight.z / magnitude)};
    const auto side = Normalize(Cross(direction, std::abs(direction.y) < .9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0}));
    const auto up = Cross(direction, side);
    if(light.type==LightType::Spot)
    {
        const float range=std::clamp(light.range,.05f,10000.f);
        constexpr int segments=48;
        for(const float angle:{light.outerAngle,light.innerAngle})
        {
            const auto center=origin+direction*(range*std::cos(angle));
            const float radius=range*std::sin(angle);
            const auto rim=[&](int i){const float phase=2*Pi*i/segments;
                return center+(side*std::cos(phase)+up*std::sin(phase))*radius;};
            for(int i=0;i<segments;++i)
            {
                SceneLine(lines,rim(i),rim(i+1),LightColor);
                if(i%(segments/4)==0&&angle==light.outerAngle)SceneLine(lines,origin,rim(i),LightColor);
            }
        }
        return;
    }
    const auto view = editor.view * Vec4{origin.x, origin.y, origin.z, 1};
    const float depth = std::max(std::abs(view.z), editor.lens.nearPlane);
    const float length = 2 * depth * 84 / (editor.projection(1, 1) * editor.viewport.height);
    if (!IsFinite(length) || length <= 0) return;
    Arrow(lines, origin, direction, side, up, length);
    Arrow(lines, origin + side * length * .22f, direction, side, up, length * .8f);
    Arrow(lines, origin - side * length * .22f, direction, side, up, length * .8f);
}
}

std::optional<Vec2> EditorSceneIconCenter(const World& world, EntityHandle entity,
                                        const CameraSnapshot& editorCamera)
{
    if (!world.IsAlive(entity) || !world.IsEnabled(entity) ||
        (!Active(world, entity, ComponentTypes::Camera) && !Active(world, entity, ComponentTypes::Light))) return {};
    const auto point = ProjectWorldPoint(editorCamera, TransformPoint(world.GetWorldMatrix(entity), {}));
    if (!point || point->z < 0 || point->z > 1) return {};
    const Vec2 center{point->x, point->y - IconOffset};
    const auto viewport = editorCamera.viewport;
    if (center.x + EditorSceneIconRadius < viewport.x || center.x - EditorSceneIconRadius > viewport.x + viewport.width ||
        center.y + EditorSceneIconRadius < viewport.y || center.y - EditorSceneIconRadius > viewport.y + viewport.height) return {};
    return center;
}

void AppendEditorSceneGizmos(const World& world, StableEntityId selected,
                            const CameraSnapshot& editorCamera, ui::UiDisplayList& overlay,
                            std::vector<rhi::SceneLine>& lines)
{
    for (const auto entity : world.Entities())
    {
        const auto center = EditorSceneIconCenter(world, entity, editorCamera);
        if (!center) continue;
        const bool camera = Active(world, entity, ComponentTypes::Camera);
        const bool light = Active(world, entity, ComponentTypes::Light);
        overlay.CanvasCircle(center->x, center->y, EditorSceneIconRadius, {.06f, .07f, .08f, .88f});
        if (world.Get(entity)->stableId == selected)
            IconRing(overlay, *center, EditorSceneIconRadius - 1, camera ? CameraColor : LightColor, 1.5f);
        if (camera) CameraIcon(overlay, *center, CameraColor);
        if (light) LightIcon(overlay, camera ? Vec2{center->x + 7, center->y - 7} : *center, LightColor, camera);
    }
    const auto entity = world.FindByStableId(selected);
    if (!entity || !world.IsEnabled(entity)) return;
    if (world.GetCamera(entity) && Active(world, entity, ComponentTypes::Camera))
        DrawCameraFrustum(world, entity, editorCamera, lines);
    if (world.GetLight(entity) && Active(world, entity, ComponentTypes::Light))
        DrawLight(world, entity, editorCamera, lines);
}

void AppendDropPreviewGizmo(const Aabb& worldBounds, bool valid, std::vector<rhi::SceneLine>& lines)
{
    const Vec3 minPt = worldBounds.minimum;
    const Vec3 maxPt = worldBounds.maximum;
    if (!IsFinite(minPt) || !IsFinite(maxPt)) return;
    const ui::UiColor color = valid
        ? ui::UiColor{0.26f, 0.88f, 0.65f, 1.0f}
        : ui::UiColor{0.92f, 0.34f, 0.30f, 1.0f};
    const std::array<Vec3, 8> corners{{
        {minPt.x, minPt.y, minPt.z}, {maxPt.x, minPt.y, minPt.z},
        {maxPt.x, minPt.y, maxPt.z}, {minPt.x, minPt.y, maxPt.z},
        {minPt.x, maxPt.y, minPt.z}, {maxPt.x, maxPt.y, minPt.z},
        {maxPt.x, maxPt.y, maxPt.z}, {minPt.x, maxPt.y, maxPt.z}
    }};
    for (int i = 0; i < 4; ++i)
    {
        const int next = (i + 1) % 4;
        SceneLine(lines, corners[i], corners[next], color);
        SceneLine(lines, corners[i + 4], corners[next + 4], color);
        SceneLine(lines, corners[i], corners[i + 4], color);
    }
}
}
