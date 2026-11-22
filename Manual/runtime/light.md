# 灯光组件

这篇文档讲清实体上的灯光参数如何保存在 `World` 中、写入时会做哪些合法性校验，以及它们如何在帧提取时被送入前向与阴影渲染管线。修改一盏灯的正确方式是：先通过 [World::GetLight()](world.md#getlight) 读出副本，修改字段后再通过 [World::SetLight()](world.md#setlight) 写回。

## 数据流总览

```text
编辑器属性面板 / C# 脚本 / 场景反序列化
        │
        ▼
  World::SetLight(entity, LightComponent)   ── 校验有限数 / 范围 / 聚光半角
        │  存入 lights_ 组件池（LightRuntimeComponent）
        ▼
  BuildFrameData()                          ── 提取启用实体的世界矩阵与 LightComponent
        │  产出 FrameData::lights (LightInstance[])
        ▼
  PrepareForwardLighting()                  ── 按稳定实体 ID 排序、乘以光色与强度
        ├─▶ 方向光主槽（最多 1 盏）         ── 驱动 CSM / PCSS 阴影与接触阴影
        ├─▶ 局部灯槽 localLights（最多 8）  ── 全部聚光灯 + 开启 castsShadow 的点光
        └─▶ 普通点光（旧路径 8 / Forward+ 256）
```

## LightRuntimeComponent

[`flare::LightRuntimeComponent`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/RuntimeComponents.h#L31) 直接继承自 [ComponentBase](component-base.md#componentbase) 并标记为 `final`。基类负责维护所属实体句柄、启停状态与生命周期，派生类仅持有一份值成员 `value`（类型为 [LightComponent](#lightcomponent)），由 [World](world.md#world) 内部的 `lights_` 组件池统一管理。

```cpp
// Source/FunctionLayer/Runtime/RuntimeComponents.h
class LightRuntimeComponent final : public ComponentBase
{
public:
    static constexpr ComponentTypeId TypeId = ComponentTypes::Light; // 类型 ID = 4
    LightComponent value;
    explicit LightRuntimeComponent(const LightComponent& properties = {})
        : value(properties) {}
};
```

## LightComponent

[`flare::LightComponent`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/ComponentValues.h#L29) 是传给 [`World::SetLight()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/World.cpp#L450) 的纯值结构体，默认构造出一盏强度为 `1` 的白色方向光。

```cpp
// Source/FunctionLayer/Runtime/ComponentValues.h
enum class LightType { Directional, Point, Spot };

struct LightComponent
{
    Vec3 color{1, 1, 1};
    float intensity = 1;
    LightType type = LightType::Directional;
    float range = 10;
    bool castsShadow = false;
    // 相对本地 -Z 发射轴的半角（弧度）。照射距离与光锥角度均不受实体缩放影响。
    float innerAngle = Pi / 9; // 约 20°
    float outerAngle = Pi / 6; // 约 30°
};
```

### 字段速查表

| 字段 | 默认值 | 校验边界与含义 |
| --- | --- | --- |
| `type` | `LightType::Directional` | 光源类型：`Directional`（方向光）、`Point`（点光源）、`Spot`（聚光灯） |
| `color` | `{1, 1, 1}` | 线性 RGB 光色，三个分量必须均为非负有限数 |
| `intensity` | `1` | 光照强度，允许设为 `0`（关闭输出但保留组件），不允许负数或 NaN |
| `range` | `10` | 点光/聚光的衰减半径（米），写入时必须为严格大于 `0` 的有限数 |
| `castsShadow` | `false` | 控制点光或聚光是否进入局部阴影图；编辑器新建点光/聚光时默认置为 `true` |
| `innerAngle`、`outerAngle` | `π/9`、`π/6` | 聚光灯沿本地 `-Z` 轴的半角（内部存弧度，Inspector 显示角度）。`outerAngle ∈ [0.01, 1.55]`，`0 ≤ innerAngle ≤ outerAngle` |

### 写入校验与移除

`World::SetLight()` 在入池前执行统一边界校验，非法值直接失败并保留组件原有状态，避免 NaN 或倒置光锥流入着色器：

```cpp
// Source/FunctionLayer/Runtime/World.cpp#L450
bool World::SetLight(EntityHandle entity, const LightComponent& component)
{
    if (!IsKnown(entity)) return Fail("World.InvalidEntity");
    if (!IsFinite(component.color) || !IsFinite(component.intensity) || component.intensity < 0
        || component.color.x < 0 || component.color.y < 0 || component.color.z < 0
        || !IsFinite(component.range) || component.range <= 0
        || LightTypeName(component.type).empty()
        || !IsFinite(component.innerAngle) || !IsFinite(component.outerAngle)
        || component.innerAngle < 0 || component.innerAngle > component.outerAngle
        || component.outerAngle < .01f || component.outerAngle > 1.55f)
        return Fail("World.InvalidComponent");
    return DeferEntity(entity, [entity, component](World& world) {
        if (!world.IsAlive(entity)) return world.Fail("World.InvalidEntity");
        return world.Store(world.lights_, entity, component);
    }, lights_.Get(entity.index) == nullptr);
}
```

- 读取灯光：[`World::GetLight()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/World.cpp#L449)
- 移除灯光：[`World::RemoveLight()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/World.cpp#L459)
- 帧快照提取：[`BuildFrameData()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Application/FrameData.cpp#L49) 与 [`PrepareForwardLighting()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/ForwardRenderer.cpp#L10)

## 场景格式与属性元数据

灯光组件在场景 JSON 中的类型 ID 为 `4`，元数据表定义在 [`ComponentMetadata.cpp`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/ComponentMetadata.cpp#L25)：

| 属性键 | 类型 | 说明 |
| --- | --- | --- |
| `light.kind` | `Text` | `"directional"` / `"point"` / `"spot"`（新调用方统一使用此键） |
| `light.color` | `Vector3` | 颜色（Inspector 按拾色器显示） |
| `light.intensity` | `Number` | 强度（最小值 `0`） |
| `light.range` | `Number` | 照射范围（最小值 `0.001`） |
| `light.castsShadow` | `Boolean` | 是否投射局部阴影 |
| `light.innerAngle` / `light.outerAngle` | `Number` | 聚光内/外半角（弧度） |
| `light.point` | `Boolean` | 仅兼容旧脚本的隐藏布尔键，读写方向光与点光；新场景不写入 |

组件版本 `2` 在 `properties.kind` 保存灯型字符串；读取版本 `1` 旧资产时，[`MigrateLight()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/ComponentMetadata.cpp#L41) 自动将旧布尔字段 `type` 迁移为 `"point"` 或 `"directional"`。

下面是一盏沿本地 `-Z` 照射、开启阴影的暖色聚光灯序列化示例：

```json
{
  "type": "4",
  "version": 2,
  "enabled": true,
  "properties": {
    "kind": "spot",
    "color": [1, 0.87, 0.67],
    "intensity": 85,
    "range": 11,
    "castsShadow": true,
    "innerAngle": 0.22,
    "outerAngle": 0.49
  }
}
```

::: tip 方向约定与继续阅读
- **方向光**以实体变换后的本地 **`+Z`** 作为指向光源的向量（`directionToLight`），**聚光灯**以实体变换后的本地 **`-Z`** 作为光锥照射方向。
- 灯光进入渲染器的分槽与阴影提交流程，请继续阅读 [前向管线与帧图](../render/pipeline.md) 与 [阴影、分簇灯光与环境遮蔽](../render/shadows.md)。
:::
