# 灯光组件

场景实体可以携带方向光、点光或聚光灯。修改一盏灯的强度，从 [World::GetLight()](world.md#getlight) 读取参数，修改副本，再通过 [World::SetLight()](world.md#setlight) 写回。

## LightRuntimeComponent

[`se::LightRuntimeComponent`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/RuntimeComponents.h#L31) 直接继承 [ComponentBase](component-base.md#componentbase)，声明为 `final`。基类记录组件属于哪个实体。派生类通过成员 `value` 保存一份 [LightComponent](#lightcomponent)，由 [World](world.md#world) 的灯光组件池管理。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/RuntimeComponents.h#L31)

## LightComponent

`se::LightComponent` 定义在 `Source/FunctionLayer/Runtime/ComponentValues.h`，是传给 `World::SetLight()` 的参数结构体。新建时默认得到强度为 1 的白色方向光。[World 的灯光写入接口](world.md#setlight)

| 字段 | 默认值 | 含义 |
| --- | --- | --- |
| `type` | `Directional` | 光源类型，可选 `Directional`、`Point`、`Spot` |
| `color` | `{1, 1, 1}` | RGB 光色，各分量均为非负有限数 |
| `intensity` | `1` | 光照强度，允许设为零 |
| `range` | `10` | 光源范围，写入时必须为正的有限数 |
| `castsShadow` | `false` | 点光或聚光投射阴影；编辑器“新建点光源/聚光灯”操作会将它置为 `true` |
| `innerAngle`、`outerAngle` | `π/9`、`π/6` | 聚光半角，沿本地 −Z 发射，内部为弧度，属性面板显示角度。外角在 0.01 至 1.55 弧度内，内角介于零和外角之间 |

从实体上移除灯光用 [World::RemoveLight()](world.md#removelight)。

灯光写入后，下一次提取帧数据时会计算渲染用的光色和方向。继续读[前向管线与帧图](../render/pipeline.md)或[方向光阴影](../render/shadows.md)。

## 场景格式与编辑器

灯光组件仍使用类型 ID 4。组件版本 2 在 `properties.kind` 保存 `directional`、`point` 或 `spot`。可选字段 `castsShadow`、`innerAngle`、`outerAngle` 使用上表默认值。读取旧组件时，版本 1 的 `type` 布尔值映射为 `directional` 或 `point`。读取原始版本 1 场景时仍接受当时的字符串灯型。旧的运行时属性 `light.point` 可以读写方向光与点光，新调用方使用 `light.kind` 表示三种灯型。

例如，下面是一盏朝本地 −Z 照射的暖色聚光灯，世界中的位置与方向由实体变换决定。

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
