# 预制体、拖放落场与玩法系统（零命令行指南）

本页汇总了做完整玩法最常用的 6 套系统：**拖放落场、预制体制作与同步、触发器与多端输入、3D 音频、GPU 粒子、导航寻路，以及昼夜联动与 DXR 硬件光追**。界面操作零命令行，每一块都配有可直接复制的 `F_` C# 示例脚本。

## 1. 拖放落场 & 制作同步预制体

### 模型和预制体拖进视口即落场
1. 在 **资源** 面板按住模型（`.fbx` / `.glb`）或预制体（`.prefab.json`）拖到 **场景视口**。
2. 鼠标落点会自动贴合地面或已有物体表面（按住 `Ctrl` 可网格吸附），松手即创建实体，不满意按 `Ctrl + Z` 撤销。

### 三步做好并同步复用预制体
1. 在 **场景层级** 搭好一套带父子结构、组件和 C# 脚本的物体（例如一盏带点光和粒子火花的火把）。
2. **直接把它的根节点拖进右侧「资源」面板**（或右键根节点点 **创建预制体**）：当前资产目录下会生成 `<名称>.prefab.json`，场景里的这个物体会自动亮起蓝色 **`◆`** 预制体链接标记。
3. 多次把 `.prefab.json` 拖进场景生成多个实例后：
   - 单独修改某个实例的位置、颜色或脚本参数时，改动会记录为该实例的独立覆盖，不会被母版冲掉。
   - 选中实例看属性面板顶栏 **`◆ 预制体名`**：点击 **[应用覆盖回预制体]** 可把改动一键同步给全部同源实例；点击 **[还原全部覆盖]** 则恢复母版默认；也可以只针对某一个属性或某一个组件单独应用或还原。

## 2. 触发器 & 键鼠/手柄动作输入（`F_InputActions`）

1. **放一个触发区域**：在 **场景层级** 右键 → **触发盒区域**（或给已有实体添加 **碰撞体与触发器** 组件并勾选 **是否为触发器**）。在 3D 视口直接拖动绿色控制柄即可调大或缩小触发范围。
2. **自定义键位映射**：打开 **项目 → 项目设置**（`Ctrl + ,`）可调整移动摇杆死区、跳跃按键或一键 **重置默认输入映射**，同时兼容键盘鼠标和 XInput 游戏手柄。
3. **在 C# 脚本里响应触发与动作**：

```csharp
using System.Numerics;
using FlareEngine;

public class DoorTrigger : F_Behaviour
{
    [F_Expose] public F_Entity? DoorMesh;
    private bool _playerInside;

    public override void F_TriggerEnter(F_Entity other)
    {
        if (other.CompareTag("Player"))
        {
            _playerInside = true;
            F_Log.Info("玩家进入门口区域，按 Jump 或移动键交互！");
        }
    }

    public override void F_TriggerExit(F_Entity other)
    {
        if (other.CompareTag("Player")) _playerInside = false;
    }

    public override void F_Update(float delta)
    {
        // 同时响应键鼠与 XInput 手柄的动作映射（无需硬编码键位）
        Vector2 moveAxis = Actions.GetAxis2D("Move");
        if (_playerInside && Actions.IsActionPressed("Jump") && DoorMesh is { IsValid: true })
        {
            DoorMesh.Translate(new Vector3(0f, 2.5f, 0f));
            F_Log.Info($"门已打开！（当前移动轴: {moveAxis}）");
        }
    }
}
```

## 3. 3D 空间音频（`F_Audio`）

### 界面配置 3 步
1. **导入音频**：把 `.wav` 或 `.ogg` 文件导入 **资源** 面板，选中音频后在属性面板可直接点 **试听音频 / 停止试听**。
2. **挂监听器**：选中 `Main Camera`，点击 **+ 添加组件** 添加 **音频监听器**（决定玩家的“耳朵”在哪里）。
3. **放音源**：在场景层级右键创建 **3D 音源**（或给任意实体添加 **音源** 组件），指定 **音频片段**，再按用途调 **空间混合（0 2D / 1 3D）**：
   - 填 `0`：纯 2D 背景音乐或 UI 音效，不受距离影响；
   - 填 `1`：3D 空间衰减声源，在 **最小衰减距离（米）** 内满音量，衰减至 **最大衰减距离（米）** 处归零。

### 脚本动态播放示例
```csharp
using FlareEngine;

public class SoundTrigger : F_Behaviour
{
    [F_Expose] public string HitClipAssetId = "";

    public override void F_CollisionEnter(in F_Contact contact)
    {
        // 1. 触发当前实体上挂好的音源组件播放
        Audio.PlaySource(Entity);

        // 2. 或者直接在碰撞接触点播一次 3D 瞬时音效
        if (!string.IsNullOrEmpty(HitClipAssetId))
        {
            var desc = F_AudioPlayDescription.Spatial(contact.Point, volume: 0.9f, minDistance: 1f, maxDistance: 25f);
            Audio.Play(HitClipAssetId, desc);
        }
    }
}
```

## 4. GPU 粒子特效（`F_Particles`）

1. 在 **场景层级** 右键创建 **粒子发射器**（或给物体添加该组件）。
2. 在右侧属性面板设置：
   - **发射形状**：点发射 (`Point`) / 球体发射 (`Sphere`) / 锥体发射 (`Cone`)；
   - **每秒发射数量**、**生命周期（秒）**、**初始/结束尺寸**、**最小/最大初速度**、**重力加速度**；
   - **初始/结束颜色** 与不透明度；勾选 **叠加混合** 可做出明亮的火花、火焰或魔法光效。
3. 点击 **`▶` 运行预览** 即可看到带深度软边缘过渡的实时粒子效果。
4. 用脚本随时迸发火花或开关发射：

```csharp
using FlareEngine;

public class SparkBurst : F_Behaviour
{
    public override void F_Update(float delta)
    {
        if (F_Input.KeyDown(F_Key.F))
        {
            uint spawned = Particles.EmitBurst(Entity, 40);
            F_Log.Info($"迸发粒子: {spawned} 颗，当前活跃总数: {Particles.ActiveCount(Entity)}");
        }
    }
}
```

## 5. 导航网格烘焙与角色自动寻路（`F_Navigation`）

1. **烘焙导航网格（后台异步，不卡界面）**：
   - 摆好地面和静态障碍物后，直接在 **资源** 面板右键选择 **烘焙场景导航网格**（或打开 **面板 → 资源与参考工具**，选 **几何烘焙 → 当前场景导航网格**，设好 **体素边长（米）** 后点 **运行**）。
   - 烘焙完成后会自动生成 `.senav` 资产并自动登记进项目资源库。
2. **给 AI 角色挂组件**：选中敌人/NPC 实体，依次添加 **胶囊角色控制器** 与 **导航代理**，在 **导航网格资产（.senav）** 槽位点 **`⊙`** 选刚才烘焙出的 `.senav` 文件，调好 **移动速度（米/秒）** 和 **停止距离（米）**。
3. **脚本指挥自动寻路避障**：

```csharp
using FlareEngine;

public class GuardPatrol : F_Behaviour
{
    [F_Expose] public F_Entity? TargetPlayer;

    public override void F_Update(float delta)
    {
        if (TargetPlayer is not { IsValid: true }) return;

        // 设定世界目标点，代理会自动沿导航网格拐点绕开障碍物
        Navigation.SetDestination(Entity, TargetPlayer.WorldPosition);

        F_NavPathStatus status = (F_NavPathStatus)Navigation.AgentStatus(Entity);
        if (status == F_NavPathStatus.Complete && Navigation.RemainingDistanceMeters(Entity) < 1.5f)
        {
            Navigation.Stop(Entity);
        }
    }
}
```

## 6. 昼夜联动、DXR 硬件光追与管线加速开关

选中场景里的 **场景渲染设置** 实体，右侧属性面板直接提供一系列高阶画质与性能开关：

| 分组与开关 | 干什么用 | 说明 |
| :--- | :--- | :--- |
| **昼夜光照联动** | 让太阳角度、颜色、天空大气与雾随 `.tod` 昼夜时间轴实时联动 | 在资源面板新建/选中 `.tod` 资产拖动预览时刻即可实时看日出到夜景变化。 |
| **动态间接光（屏幕空间）** | 开启室内实时反弹染色 | 适合室内多光源场景，自带时域降噪。 |
| **启用硬件光线追踪 (DXR)** | 在支持 DXR 1.1 的显卡上开启混合硬件光追（内置 SVGF 时空降噪） | 展开后可独立开关 **光追方向软阴影**、**光追环境光遮蔽 (RTAO)**、**光追镜面与粗糙反射** 与 **光追全局漫反射间接光 (RTGI)**；显卡不支持时自动回退至光栅/屏幕空间路径。 |
| **优先使用硬件网格着色器** | 启用 SM 6.5 Mesh Shader 几何管线加速 | 硬件不支持时自动无感回退 Compute 间接绘制，不用手动关。 |
| **瞬态显存物理别名** / **静态级联阴影缓存** / **静态网格空间合批** | 按关卡特征减少显存占用与阴影/提交开销 | 大型静态场景建议保持开启，动态物体较多的关卡可按需开关测试帧率。 |
