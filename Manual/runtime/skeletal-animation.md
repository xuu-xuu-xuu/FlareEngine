# 骨骼动画与 GPU 蒙皮

这篇文档讲清楚 Flare 骨骼动画从 glTF/GLB 导入、`SkeletalAnimationSystem` 固定步采样、GPU 3×4 蒙皮调色板打包，到 C# 行为脚本动态切动作与查询关节世界坐标的完整实现链路。如果只需在编辑器里给模型配动作，直接看[让模型播放骨骼动画](../user/animate-model.md)。

## 一句话定位

同一个实体上，`MeshComponent` 负责画什么网格，`ActorSettingsComponent`（`TypeId = 0x464C0005`）负责用哪个模型骨架和片段算姿态；`SkeletalAnimationSystem` 在每个固定步的 `FixedPreTick` 之前算出蒙皮矩阵与关节世界位姿，渲染快照与脚本逻辑共享同一固定步结果，零帧延迟。

## 端到端数据流

```text
  glTF / GLB 模型 (1st skin + JOINTS_0/WEIGHTS_0 + LINEAR/STEP 轨道)
        │  ModelImporter (读取关节拓扑、逆绑定矩阵与动画通道)
        ▼
  StaticModel (只读资产，共享于全部同模型实体)
        │  Environment::SetSkeletalResolver()
        ▼
  SkeletalAnimationSystem::Advance(world, dt)   [在 FixedPreTick 之前执行]
    ├─ 仲裁播放源: 脚本 pendingPlays_/override 优先于场景初始 actor.clip
    ├─ Sample(): rest TRS 兜底 + LINEAR(Lerp/SlerpShortest) / STEP 通道覆盖
    └─ 父序累乘: jointWorld[i] = jointWorld[parent] * local[i]
                 joints[i]     = jointWorld[i] * inverseBind[i]
        │
        ├──▶ 玩法逻辑: QueryJointPosition() ──▶ entityWorld * pose.jointWorld[i]
        │                                       (C# F_Entity.JointPosition("IndexTip.R"))
        └──▶ 渲染管线: BuildFrameData(env) 复制 pose.joints 到 mesh.skinPalette
                          └─▶ SandboxRenderer::BuildSkinPalette()
                              每关节打包 48 字节 (行主序 3×4 float) ──▶ Shaders/Skinning.hlsli
```

## 核心源码拆解

### 1. glTF / GLB 骨架与动画导入约束

导入器在 [`ModelImporter.cpp`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/ModelImporter.cpp#L803) 中于 `cgltf_validate` 之前执行结构预检，明确边界：

- 读取第 1 个 `skin`（多个 skin 时给出诊断警告并仅使用第 1 个）及其拓扑排序后的关节与 `inverseBindMatrices`；
- 每顶点最多 4 个骨骼影响（`JOINTS_0` 用 `uint16` 存储，最多支持 `65536` 根关节；`WEIGHTS_0` 要求有限且和为正，归一化到 `1`）；
- 关节平移/旋转/缩放轨道支持 **`LINEAR`**（旋转走最短弧球面插值 `SlerpShortest`）与 **`STEP`** 两种插值；**不支持 `CUBICSPLINE`、Morph Target 形变权重动画以及带骨骼的 FBX**（遇到 `CUBICSPLINE` 会在预检阶段直接报错 [ModelImporter.cpp#L541](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/ModelImporter.cpp#L541)）。

### 2. 运行时系统与组件初始字段（`SkeletalAnimationSystem`）

`Environment` 独占 [`SkeletalAnimationSystem`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/SkeletalAnimation.h#L25)，宿主通过 `SetSkeletalResolver()` 把资产 ID 映射为只读 `StaticModel`。实体上的 `ActorSettingsComponent`（[FeatureComponents.cpp#L121](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/FeatureComponents.cpp#L121)）提供 5 个可序列化初始属性：`actor.asset`、`actor.clip`（默认 `"Idle"`）、`actor.speed`（默认 `1.0`）、`actor.loop`（默认 `true`）和 `actor.playing`（默认 `true`）。

### Advance

[`SkeletalAnimationSystem::Advance(World& world, double seconds)`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/SkeletalAnimation.cpp#L122) 由 [`Environment::FixedStep()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/Environment.cpp#L82) 在 `FixedPreTick` 之前调用：

1. **脚本覆盖与组件属性的仲裁规则**：场景里的 `actor.clip` 只是初始值。一旦脚本通过 `RequestPlay()` 切过动作，覆盖状态 `state.hasOverride` 会持续生效，不会被下一帧重新读回的 `actor.clip` 冲掉——**只有当编辑器或外部把组件 `actor.clip` 改写成新的片段名时，才自动让出控制权**。
2. **采样与父序合成**：`Sample()` 先用每个关节的 `rest` TRS 兜底，仅对动画片段里实际存在通道的分量进行覆盖；然后按资产保证的父序（`parent < index`）一次循环同时算出局部关节位姿 `jointWorld` 与蒙皮矩阵 `joints`：

```cpp
// Source/FunctionLayer/Runtime/SkeletalAnimation.cpp
for (std::size_t index = 0; index < model.joints.size(); ++index)
{
    const auto parent = model.joints[index].parent;
    pose.jointWorld[index] = (parent >= 0 && static_cast<std::size_t>(parent) < index)
        ? pose.jointWorld[static_cast<std::size_t>(parent)] * locals[index]
        : locals[index];
    pose.joints[index] = pose.jointWorld[index] * FromArray(model.joints[index].inverseBind);
}
```

3. **帧快照复制与 GPU 3×4 矩阵打包**：[`BuildFrameData(const Environment&, ...)`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Application/FrameData.cpp#L15) 将本步 `pose->joints` 复制进 `mesh.skinPalette` 并标记 `mesh.staticGeometry = false`（注意：自定义宿主如果只调用传 `World` 的重载，不会提取蒙皮姿态）。随后渲染器 [`SandboxRenderer::BuildSkinPalette()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/SandboxRenderer.cpp#L79) 把每根关节的仿射矩阵压缩为 **48 字节的行主序 3×4 浮点矩阵**，与 `Shaders/Skinning.hlsli` 中的 `LoadSkinMatrix` 严格对应：

```cpp
// Source/FunctionLayer/Rendering/SandboxRenderer.cpp
skinPaletteStorage_.emplace_back(static_cast<std::size_t>(outJointCount) * 48);
auto& bytes = skinPaletteStorage_.back();
for (std::uint32_t joint = 0; joint < outJointCount; ++joint)
{
    const auto& m = palette.empty() ? flare::Mat4{} : palette[joint];
    const float rows[12]{
        m(0,0), m(0,1), m(0,2), m(0,3),
        m(1,0), m(1,1), m(1,2), m(1,3),
        m(2,0), m(2,1), m(2,2), m(2,3)
    };
    std::memcpy(bytes.data() + static_cast<std::size_t>(joint) * 48, rows, sizeof(rows));
}
```

## F_Entity

C# 行为脚本（`F_Behaviour`）直接通过自身 `Entity` 属性控制动作播放与读取关节世界坐标。以下示例假设模型包含 `"Walk"` 片段与名为 `"IndexTip.R"` 的关节（实际使用时替换为自己模型的真实名称）：

```csharp
using System.Numerics;
using FlareEngine;

public sealed class HandTrailBehaviour : F_Behaviour
{
    public override void F_Start()
        => Entity.PlayAnimation("Walk", speed: 1f, loop: true);

    public override void F_FixedUpdate(float fixedDelta)
    {
        if (Entity.JointPosition("IndexTip.R") is not { } tip) return;
        // tip 是本次固定步的世界坐标（米），可直接用于触碰检测或附着粒子特效
    }
}
```

### PlayAnimation

[`void PlayAnimation(string clip, float speed = 1f, bool loop = false)`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/EngineSession.cs#L494) 经 `RequestPlay()` 向运行时提交播放请求（绕开组件写入命令队列限制），并在下一次固定步 [`Advance()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/SkeletalAnimation.cpp#L157) 中消费：

- **单次动作（`loop: false`，默认）**：每次调用都会将播放时间 `state.time` 重置为 `0` 从头播放，因此应在触发攻击/跳跃等瞬时事件时调用一次，不要每帧重复调用；播到结尾后停在最后一帧。
- **循环动作（`loop: true`）**：在同一循环片段上再次调用（例如根据角色移速动态调整 `speed`）会**保留当前播放相位**而不回零；切换到不同片段名时则自动重置 `state.time = 0` 从头开始。

### JointPosition

[`Vector3? JointPosition(string joint)`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/EngineSession.cs#L497) 调用原生 [`QueryJointPosition()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/SkeletalAnimation.cpp#L270) 与 [`QueryJointWorld()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/SkeletalAnimation.cpp#L259)，计算 `world.GetWorldMatrix(entity) * pose.jointWorld[index]` 并返回关节在世界空间中的位置（米）。当实体没有有效动画姿态或模型中不存在该关节名时返回 `null`。

## 常用接口速查

| 端 | 接口 | 说明 | 源码位置 |
| --- | --- | --- | --- |
| C++ Runtime | `SkeletalAnimationSystem::Advance` | 固定步推进与蒙皮/世界关节位姿计算 | [SkeletalAnimation.cpp#L122](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/SkeletalAnimation.cpp#L122) |
| C++ Runtime | `SkeletalAnimationSystem::RequestPlay` / `Play` | 帧内脚本覆盖请求 / 帧外直接修改 `actor.*` 组件属性 | [SkeletalAnimation.cpp#L77](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/SkeletalAnimation.cpp#L77) |
| C++ Runtime | `SkeletalAnimationSystem::QueryJointWorld` / `QueryJointPosition` | 查询关节完整世界矩阵或世界坐标 | [SkeletalAnimation.cpp#L259](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/SkeletalAnimation.cpp#L259) |
| C# SDK | `F_Entity.PlayAnimation` / `F_Entity.JointPosition` | 托管脚本切换动作与读取骨骼挂点世界坐标 | [EngineSession.cs#L494](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/EngineSession.cs#L494) |

::: tip 内置诊断伪片段 `"__rest"`
如果导入的骨骼模型蒙皮变形异常，可以把 `actor.clip` 临时设为 `"__rest"`（[SkeletalAnimation.cpp#L178](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/SkeletalAnimation.cpp#L178)）。引擎会跳过所有动画通道，仅用关节的 `rest TRS` 走完整 GPU 蒙皮路径——若 `"__rest"` 画面正常而具体动作异常，即可定位问题在动画轨道而非骨架绑定空间。
:::

::: warning 找不到片段名时的回退行为
如果 `actor.clip` 指向的片段名在模型 `animations` 中不存在，`SkeletalAnimationSystem::Find()` 会返回 `nullptr`，该网格不会生成蒙皮调色板，渲染器会自动按普通静态绑定姿态绘制，同时 `JointPosition()` 也会返回 `null`。请检查片段名大小写是否与 glTF 导出名称完全一致。
:::
