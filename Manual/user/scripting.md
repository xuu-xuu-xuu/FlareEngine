# 编写与挂载 C# 脚本（零重启热重载）

在 Flare 中新建、挂载或修改 C# 脚本**全程无需重启编辑器窗口**。下面给示例场景中的 `Cube` 挂载一个可键盘移动、自动旋转并朝向目标的脚本。

## 1. 搜索框一键新建并挂载脚本

1. 在中部 **场景层级** 点击选中 `Cube`。
2. 点击右侧 **属性** 面板底部的 **+ 添加组件** 按钮，弹出搜索浮层。
3. 直接输入 `SpinCube`：列表顶部会出现 **＋ 新建脚本并挂载: SpinCube.cs**，按回车（或点击该项）。
4. 编辑器会自动在项目 `Scripts/SpinCube.cs` 生成模板代码、在项目根目录同步 `<项目名>.sln` 与引擎 API 智能提示桥接（含 `FlareEngine.Managed.xml` 中文悬停注释）、在后台静默增量编译，并将 `SpinCube (脚本)` 卡片挂载到 `Cube` 上，同时以完整项目工作区/解决方案模式打开你的代码编辑器（自动识别 VS Code / Cursor / Antigravity / Visual Studio / Rider；若搬移过引擎目录，也可在 **偏好设置 → 同步 C# 解决方案** 一键刷新）。

## 2. 编写代码并切回自动热重载

在代码编辑器（VS Code / Rider / Visual Studio）中将 `SpinCube.cs` 替换为以下内容并按 `Ctrl+S` 保存：

```csharp
using System.Numerics;
using FlareEngine;

public class SpinCube : F_Behaviour
{
    [F_Header("旋转与移动")]
    [F_Expose, F_Range(0f, 360f)] public float TurnSpeed = 120f;
    [F_Expose, F_Range(0f, 20f)] public float MoveSpeed = 4f;

    [F_Header("观察目标")]
    [F_Expose] public EntityReference LookTarget;

    public override void F_Start()
    {
        F_Log.Info($"SpinCube 已启动，初始位置: {Entity.Position}");
    }

    public override void F_Update(float delta)
    {
        var input = new Vector3(F_Input.Horizontal, 0f, F_Input.Vertical);
        Translate(input * (MoveSpeed * F_Time.DeltaTime), localSpace: false);
        Rotate(new Vector3(0f, TurnSpeed * F_Time.DeltaTime, 0f));

        if (LookTarget.Resolve(Entity.Session) is { } target)
            LookAt(target.Position);
    }
}
```

保存后**直接切回 Flare 编辑器窗口**（也可随时点顶部工具栏的 **编译脚本**）：底部状态栏与控制台会提示后台增量编译并自动完成热重载，全程同一窗口零闪烁。

## 3. 滑块调参、实体拖拽引用与统一保存

切回编辑器选中 `Cube`，你会看到 `SpinCube (脚本)` 卡片已自动更新：

- **声明式特性与丰富字段类型**：除 `float`/`int`/`bool`/`string`/`Vector3`/`EntityReference`/`AssetReference` 外，原生反射支持 `Vector2`、`F_Color`（HSV 拾色器）与自定义 `enum` 下拉框，并支持 `[F_Header]` 分组、`[F_Range(min, max)]` 滑块、`[F_Tooltip("...")]` 悬停提示气泡、`[F_Space]` 留白与 `[F_ReadOnly]` 只读检视。
- **拖拽绑定实体**：从 **场景层级** 按住 `Main Camera`（或其他物体），直接拖进属性栏的 `LookTarget` 槽松手，槽位即显示 `◈ Main Camera (#ID)`（点右侧 `×` 可清除引用）。
- **统一保存**：按 `Ctrl+S`（或点左上角 **保存**），场景与脚本绑定配置会一同保存，无需额外点击单独的绑定保存按钮。
- **管理脚本组件**：点击脚本卡片右上角的 **`⋮`** 菜单，可随时选择 **编辑 C# 脚本** 或 **移除组件**。

## 4. 常用 `FlareEngine` 高频 API 速查

| 场景需求 | 推荐写法 |
| :--- | :--- |
| **生命周期与物理回调** | 重写 `F_Awake()`、`F_Enable()`、`F_Start()`、`F_Update(float delta)`、`F_FixedUpdate(float fixedDelta)`、`F_LateUpdate(float delta)`、`F_CollisionEnter(in F_Contact)`、`F_TriggerEnter(F_Entity)`、`F_Destroy()` |
| **读写位置、旋转、朝向** | `Entity.Position`、`Entity.WorldPosition`、`Entity.Rotation`、`Entity.WorldRotation`、`Entity.EulerAngles`、`Entity.LocalScale`、`Entity.Forward / Right / Up`、`Entity.SetParent(parent)`，以及 `Translate(...)`、`Rotate(...)`、`LookAt(...)`（对刚体/角色控制器自动同步物理位姿） |
| **获取组件与场景查找** | `GetComponent<RigidBody>()`、`TryGetComponent<OtherScript>(out var s)`、`F_Scene.Find("Player")`、`F_Scene.FindObjectOfType<GameManager>()`、`F_Scene.Reload()` |
| **深度克隆与延时销毁** | `var bullet = Instantiate(prefabOrEntity, pos, rot);`（完整复制子物体层级、网格材质、碰撞体与脚本引用） · `Destroy(bullet, 3.0f);` |
| **帧时间与慢放** | `F_Time.DeltaTime`、`F_Time.FixedDeltaTime`、`F_Time.UnscaledDeltaTime`、`F_Time.Time`、`F_Time.TimeScale` |
| **键鼠、光标锁定与射线** | `F_Input.Key(F_Key.W)`、`F_Input.KeyDown(F_Key.Space)`、`F_Input.MouseDown()`、`F_Input.MouseDelta`、`F_Input.MoveVector` · `F_Cursor.Locked = true;` · `F_Camera.ScreenPointToRay(F_Input.MousePosition)` |
| **常用数学与四元数** | `F_Math.Clamp / Clamp01 / Lerp / MoveTowards / SmoothDamp / PingPong` · `F_Quat.Euler / ToEuler / LookRotation / Slerp / RotateTowards` |
| **刚体速度与施力** | `body.Velocity`、`body.AngularVelocity`、`body.AddForce(force)`、`body.AddImpulse(impulse)` |
| **无线程异步时序** | `await F_Await.Seconds(1.0f);` · `await F_Await.NextFrame();` · `await F_Await.FixedUpdate();`（停止预览自动安全取消） |
| **输出日志到控制台** | `F_Log.Info(...)`、`F_Log.Warn(...)`、`F_Log.Error(...)`（双击控制台日志行直达源码行） |
