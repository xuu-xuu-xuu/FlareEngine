# 骨骼动画与 GPU 蒙皮

同一个实体上，网格组件决定画什么，动画角色组件决定用哪个模型、哪个片段计算姿态。两者使用同一模型资源 ID。只想配置场景，见[让模型播放骨骼动画](../user/animate-model.md)；要让动作响应游戏事件，使用下面的脚本接口。

## SkeletalAnimationSystem

运行时 `Environment` 持有 `SkeletalAnimationSystem`。宿主通过 `SetSkeletalResolver` 将资产 ID 解析为带骨架的 `StaticModel`；固定步在脚本的固定步前置回调之前调用 `Advance`。场景初始值取自 `actor.asset`、`actor.clip`（默认 `Idle`）、`actor.speed`（默认 1）、`actor.loop` 和 `actor.playing`（均默认开启）。[类型声明](https://github.com/xuu-xuu-xuu/FlareEngine/blob/542afe82dcdf65eb987477b3862dd3b5d82b43e5/Source/FunctionLayer/Runtime/SkeletalAnimation.h#L25) · [组件字段](https://github.com/xuu-xuu-xuu/FlareEngine/blob/542afe82dcdf65eb987477b3862dd3b5d82b43e5/Source/FunctionLayer/Runtime/FeatureComponents.cpp#L58) · [固定步调用](https://github.com/xuu-xuu-xuu/FlareEngine/blob/542afe82dcdf65eb987477b3862dd3b5d82b43e5/Source/FunctionLayer/Runtime/Environment.cpp#L82)

动画输入为 glTF / GLB。导入器读取第一个 skin 的关节与逆绑定矩阵；每顶点最多四个影响（`JOINTS_0`、`WEIGHTS_0`）。关节平移、旋转、缩放支持 LINEAR 和 STEP；CUBICSPLINE、形变权重动画和带骨骼的 FBX 不在此路径内。[骨架导入](https://github.com/xuu-xuu-xuu/FlareEngine/blob/542afe82dcdf65eb987477b3862dd3b5d82b43e5/Source/ResourceLayer/Assets/ModelImporter.cpp#L643) · [动画通道校验](https://github.com/xuu-xuu-xuu/FlareEngine/blob/542afe82dcdf65eb987477b3862dd3b5d82b43e5/Source/ResourceLayer/Assets/ModelImporter.cpp#L368)

### Advance

`Advance(World&, double seconds)` 按固定步采样启用的角色，先合成父子关节变换，再乘逆绑定矩阵得到蒙皮矩阵。找不到指定片段时没有可用的动画姿态。`BuildFrameData(const Environment&, …)` 将姿态复制到本帧网格快照；自定义宿主若只传 `World`，不会提取动画姿态。渲染器将每关节写成 48 字节的 3×4 矩阵供 GPU 蒙皮使用。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/542afe82dcdf65eb987477b3862dd3b5d82b43e5/Source/FunctionLayer/Runtime/SkeletalAnimation.cpp#L122) · [帧快照](https://github.com/xuu-xuu-xuu/FlareEngine/blob/542afe82dcdf65eb987477b3862dd3b5d82b43e5/Source/FunctionLayer/Application/FrameData.cpp#L5) · [GPU 数据布局](https://github.com/xuu-xuu-xuu/FlareEngine/blob/542afe82dcdf65eb987477b3862dd3b5d82b43e5/Source/FunctionLayer/Rendering/SandboxRenderer.cpp#L42)

## Entity

C# 行为脚本通过自身的 `Entity` 控制角色。以下是绑定到角色的 `Behaviour` 中的两个回调。示例假设模型确实包含 `Walk` 和名为 `IndexTip.R` 的关节；移植时应替换为自己模型中的名称。

```csharp
public override void OnCreate() => Entity.PlayAnimation("Walk", speed: 1f, loop: true);

public override void OnFixedPostUpdate(double seconds)
{
    if (Entity.JointPosition("IndexTip.R") is not { } tip) return;
    // tip 是本次固定步的世界坐标，可用来定位手指特效。
}
```

### PlayAnimation

`void PlayAnimation(string clip, float speed = 1f, bool loop = false)` 向运行时提交请求，由下一次动画固定步处理。默认单次播放；每次请求单次动作都会从头开始，因此应在触发动作时调用一次。在同一循环片段上再次传 `loop: true` 可以保留当前进度并更新速度。切到另一片段则从头开始。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/542afe82dcdf65eb987477b3862dd3b5d82b43e5/Source/FunctionLayer/Scripting/Managed/EngineSession.cs#L328) · [请求处理](https://github.com/xuu-xuu-xuu/FlareEngine/blob/542afe82dcdf65eb987477b3862dd3b5d82b43e5/Source/FunctionLayer/Runtime/SkeletalAnimation.cpp#L157)

### JointPosition

`Vector3? JointPosition(string joint)` 按名称返回关节的世界坐标（米），包含实体及其父层级的变换。没有有效姿态或关节名不匹配时返回 `null`；可在固定步回调中用模式匹配检查，再做接触判定或附着特效。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/542afe82dcdf65eb987477b3862dd3b5d82b43e5/Source/FunctionLayer/Scripting/Managed/EngineSession.cs#L330) · [世界变换](https://github.com/xuu-xuu-xuu/FlareEngine/blob/542afe82dcdf65eb987477b3862dd3b5d82b43e5/Source/FunctionLayer/Runtime/SkeletalAnimation.cpp#L259)
