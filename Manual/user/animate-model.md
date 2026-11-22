# 让模型播放骨骼动画

本页带你导入带骨架与动作的 `.glb` 角色模型，在场景视口里直接挑动作预览，并学会用几行 C# 脚本在游戏运行期间按事件切换动作、读取手部等骨骼关节坐标。

::: info 模型导出格式要求
- 请将网格、骨架、蒙皮和动作一起导出为单个 **`.glb`**（最省心），或保留完整依赖文件的 `.gltf`。
- Flare 的 FBX 导入器目前只用于静态模型，带骨骼动画请优先用 `.glb`。
:::

## 1. 导入带动作的 GLB 模型

1. 打开包含摄像机的 Flare 项目。
2. 在 **资源** 面板点击 **导入**（或按 `Ctrl + I`），选中你的 `.glb` 角色文件并打开。
3. 等待状态栏提示 **「资源导入成功」**。点击资源列表里的新模型，右侧 **属性** 面板会显示它的顶点数、三角面数与 **动画剪辑数**。

## 2. 一键放进场景并在视口里预览动作

1. 在 **资源** 面板选中刚导入的 `.glb` 模型，直接拖入 **场景视口**（或在右侧属性面板点 **将模型加入场景**）。
2. 编辑器会自动帮你挂载 **网格** 与 **动画角色** 两个组件，并自动绑定同一个模型资产：
   - 如果模型里有名为 `Idle` 的片段，默认选中 `Idle`；否则默认选中第一个动作片段。
3. 在右侧 **属性** 面板展开 **动画角色** 卡片：
   - 点击 **动画片段名** 下拉选择你想看的动作（例如 `Walk`、`Run`、`BendTouch`）。
   - 点击 **预览所选动作**，角色会直接在 **场景视口** 中单独循环播放，不用开整场游戏预览。
   - 点击 **暂停动作预览** 可定格在当前姿态；在 **预览位置（秒）** 输入具体秒数按 `Enter` 可以逐帧检查动作细节。
4. 想连同场景灯光和脚本一起看整体效果，按 **`F5`**（或点顶栏 **`▶`**）运行预览即可。

## 3. 给已有物体手动挂接骨骼动画

如果你想给场景里已经建好的空物体配置骨骼角色：

1. 在 **场景层级** 选中目标实体。
2. 在右侧 **属性** 面板点 **+ 添加组件**，添加 **动画角色**（如果该物体还没挂 **网格** 组件，编辑器会自动补齐前置的 **网格** 依赖）。
3. 按下表填好关键项（选资产可直接点槽位右侧的 **`⊙`** 搜索选择，不用手动复制 ID）：

| 组件与字段 | 怎么配置 |
| :--- | :--- |
| **网格 → 网格资源** | 选择刚导入的 `.glb` 角色模型。 |
| **动画角色 → 动画资产** | 选择**同一个 `.glb` 模型**，让骨骼姿态与网格对应。 |
| **动画角色 → 动画片段名** | 从下拉菜单选择模型里实际存在的片段（如 `Walk`）；如果下拉为空，请检查导出 `.glb` 时是否勾选了导出动画。 |
| **动画角色 → 播放速度** | `1` 为原速，`0.5` 为半速慢放，`0` 停在当前姿态。 |
| **动画角色 → 正在播放 / 循环播放** | 都需要开启；如果动作只播一次停在最后一帧，取消勾选 **循环播放** 即可。 |

::: tip 换模型后片段名不匹配？
如果替换了模型导致原来的片段名不存在，**动画角色** 卡片内会自动出现黄色提示条，点击右侧 **[选用首个片段]** 即可一键修复。
:::

## 4. 用 C# 脚本按按键切换动作 & 追踪关节位置

如果你想让角色在移动时播 `Walk`、静止时播 `Idle`，或者在角色指尖/手腕挂特效，只需给角色实体挂一个 `F_Behaviour` 脚本：

```csharp
using System.Numerics;
using FlareEngine;

public class HeroAnimator : F_Behaviour
{
    [F_Expose] public string IdleClip = "Idle";
    [F_Expose] public string WalkClip = "Walk";
    [F_Expose] public string TrackedJoint = "IndexTip.R";
    [F_Expose] public F_Entity? HandSpark;

    private bool _wasMoving;

    public override void F_Start()
    {
        Entity.PlayAnimation(IdleClip, speed: 1f, loop: true);
    }

    public override void F_Update(float delta)
    {
        bool moving = F_Input.MoveVector.LengthSquared() > 0.01f;
        if (moving != _wasMoving)
        {
            _wasMoving = moving;
            // loop: true 循环切换动作；需要单次挥手/攻击时传 loop: false
            Entity.PlayAnimation(moving ? WalkClip : IdleClip, speed: 1f, loop: true);
        }
    }

    public override void F_FixedUpdate(float fixedDelta)
    {
        // 读取指定关节的世界坐标（请把 TrackedJoint 改成你模型里的实际骨骼名）
        if (HandSpark is { IsValid: true } && Entity.JointPosition(TrackedJoint) is { } jointWorldPos)
        {
            HandSpark.WorldPosition = jointWorldPos;
        }
    }
}
```

## 5. 在导出的独立游戏里检查结果

1. 按 **`Ctrl + S`** 保存场景。
2. 按 [导出游戏](record-video.md#export-game) 的步骤生成独立游戏包，双击包里的 **`Play.exe`** 查看角色是否按预期循环运动。
3. 每次在编辑器修改完动作或场景后，记得**先保存再重新导出**，旧游戏包里保留的是上次导出时的场景快照。

::: warning 进游戏发现角色静止不动，或完全看不到？
- **只看到静止模型**：依次检查「网格资源」与「动画资产」是否指向同一个 `.glb`、片段名是否存在、实体与组件是否启用、**正在播放**是否勾选且 **播放速度** 大于 `0`。
- **连模型都看不到**：检查 `Main Camera` 是否对准了角色，以及模型的缩放是不是过小或过大（部分软件导出的模型需要调整 `Transform` 缩放，如 `0.01` 或 `100`）。
:::

想把这段角色动作录成高清视频，继续看 [连续抓帧并导出 MP4](record-video.md#capture)；底层蒙皮细节见开发者文档 [骨骼动画与 GPU 蒙皮](../runtime/skeletal-animation.md#playanimation)。
