# 编写与挂载 C# 脚本（零重启热重载）

本页带你走完从新建脚本、外部 IDE 编写代码（自带中文智能提示）、保存后**切回编辑器零重启热重载**，到在属性面板拖拽绑定实体与滑块调参的全过程，并附上可直接复制运行的统一 `F_` API 实战脚本。

## 1. 一键新建并挂载脚本

1. 在 **场景层级** 点击选中 `Cube`（或任意实体）。
2. 在右侧 **属性** 面板拉到最底部，点击 **+ 添加组件**，弹出搜索浮层。
3. 在搜索框直接输入新脚本名字，例如 `PlayerController`：列表最上方会出现 **＋ 新建脚本并挂载: PlayerController.cs**，按 `Enter`（或点击它）。
4. 编辑器会在后台一口气帮你做好 4 件事：
   - 在项目 `Scripts/` 下生成 `PlayerController.cs` 与脚本工程 `FlareGame.Scripts.csproj`。
   - 在项目根目录自动同步 **`<项目名>.sln`** 与 **`FlareEngine.Managed.xml`** 中文文档注释桥接（悬停任意 `F_` 类和方法都能直接看到中文说明）。
   - 自动识别并打开你电脑里装好的代码编辑器（支持 **VS Code / Cursor / Antigravity / Windsurf / Visual Studio / JetBrains Rider**），直接以完整项目/解决方案模式打开，开箱就有代码补全和跳转定义。
   - 后台静默编译并把 `PlayerController (脚本)` 卡片挂载到当前实体。

::: tip 搬移过引擎目录，或外部 IDE 找不到 `FlareEngine` 提示怎么办？
打开菜单 **编辑 → 偏好设置**（快捷键 `Ctrl + Shift + P`），点击底部的 **同步 C# 解决方案** 按钮，编辑器会立刻重新生成 `<项目名>.sln` 与本地 SDK 路径桥接并执行依赖还原；也可以在偏好设置里手动指定你偏好的外部代码编辑器路径。
:::

## 2. 修改代码后切回窗口，自动零重启热重载

整个开发循环只需要两步：

```text
  在 VS Code / Rider / VS 改完代码按 Ctrl+S 保存
                      │
                      ▼
  直接切回 Flare 编辑器窗口（或按 Ctrl+B 点「编译脚本」）
                      │
                      ▼
  后台自动增量编译 ──▶ 成功：原地无闪烁热重载，属性面板实时刷新暴露字段
                 ──▶ 报错：状态栏与控制台标红，双击报错行直接跳回代码对应行列
```

::: info 控制台双击直达出错代码
不管是编译报错，还是运行时用 `F_Log.Info / Warn / Error` 打出的日志，都会自带 `文件名.cs(行,列)`。在左下角 **控制台** 面板**双击该行**，会直接把外部代码编辑器光标定位到那一行代码。
:::

::: tip 为什么热重载不是“边打字边编译”？
Flare 的 C# 热重载**默认就是自动开启的，不需要写任何配置代码**。引擎刻意在「切回编辑器窗口」或「点击运行预览」时才触发增量编译，而不是你每敲一个字就编译——这样可以避免外部编辑器开启自动保存（Auto-Save）时把还没写完的代码反复编译刷屏报错。如果你是双屏并排写代码（窗口焦点没切换），随时按 **`Ctrl + B`** 点一下顶栏 **编译脚本** 即可立刻刷新。另外，**运行预览（Play Mode）进行中会暂停代码级热重载**，但支持在属性面板直接热调数值；停止预览后新代码会自动生效。
:::

## 3. 3 分钟看懂一份 Flare 脚本的语法结构

如果你刚接触 C# 或 Flare 脚本，下面这份 20 行的 `PlayerMover.cs` 就是最标准的脚本骨架——它实现“按 `WASD` 移动、转头看目标、按空格跳跃并打印日志”：

```csharp
using System.Numerics;                       // ① 引入命名空间（提供 Vector3 / Vector2）
using FlareEngine;                           //    引入引擎全部 F_ 游戏 API

public class PlayerMover : F_Behaviour       // ② 定义脚本类：继承 F_Behaviour 才能挂到物体上
{
    // ③ 成员变量：加 [F_Expose] 会显示在编辑器右侧「属性」面板
    [F_Header("移动参数")]
    [F_Expose, F_Range(1f, 20f)] public float MoveSpeed = 6f;
    [F_Expose] public F_Entity? LookTarget;

    // ④ 生命周期方法：游戏运行时由引擎在固定时机自动调用
    public override void F_Update(float delta)
    {
        // A. 读取 WASD 移动物体
        var dir = new Vector3(F_Input.Horizontal, 0f, F_Input.Vertical);
        Translate(dir * (MoveSpeed * F_Time.DeltaTime), localSpace: false);

        // B. 自动转头看向目标物体
        if (LookTarget is { IsValid: true })
            LookAt(LookTarget.WorldPosition);

        // C. 按空格键给刚体施加向上冲量，并打印起跳坐标
        if (F_Input.KeyDown(F_Key.Space) && TryGetComponent<F_Rigidbody>(out var body))
        {
            body.AddImpulse(Vector3.UnitY * 5f);
            F_Log.Info($"Jump at {Entity.WorldPosition}");
        }
    }
}
```

### 结构与常见 C# 语法拆解

1. **命名空间（`using ...;`）**
   - 写在文件最顶部，相当于告诉编译器“我要用哪些工具箱”。写 Flare 脚本基本固定带两行：`using System.Numerics;`（三维向量 `Vector3` 等数学类型）和 `using FlareEngine;`（引擎的 `F_` 开头全部 API）。
2. **类声明（`public class PlayerMover : F_Behaviour`）**
   - `public class 类名` 定义一个公开类，**建议与 `.cs` 文件名保持一致**（例如 `PlayerMover.cs` 里写 `class PlayerMover`）。
   - 冒号后的 `: F_Behaviour` 表示“继承自引擎行为基类”，只有继承它才能作为组件挂到场景物体上，**不需要**额外手写 `[F_Script]` 或外层 `namespace`。
3. **特性与字段（`[...]`、`public`、`float`、`?`）**
   - **方括号 `[...]`（特性 Attribute）**：给编辑器看的标记，用来控制面板显示方式（例如 `[F_Expose]` 暴露到面板、`[F_Range(1f, 20f)]` 变成滑条）。**不加 `[F_Expose]` 的字段只供代码内部使用，不会出现在属性面板**。
   - **`public` 与 `private`**：需要暴露给编辑器或其他脚本读写的字段写 `public`；只在当前脚本内部缓存的临时变量（如 `_body`、`_yaw`）写 `private`。
   - **小数后缀 `f`**：C# 里写 `float` 必须加后缀 `f`（如 `6f`、`0.25f`），不写 `f` 会被当成 `double` 导致类型报错。
   - **类型后面的 `?`（可空类型）**：如 `F_Entity?`、`F_Rigidbody?` 表示“这个引用可能是空的”（比如面板里还没拖物体进去，或目标刚被销毁）。使用前写 `if (LookTarget is { IsValid: true })` 就能同时判空并确认物体仍然存活，不会崩。
4. **生命周期钩子（`public override void F_...()`）**
   - `override` 表示重写基类 `F_Behaviour` 预留的回调。你只需要写你关心的那几个，没写的不占任何开销：
     | 回调方法 | 什么时候触发 | 常见用途 |
     | :--- | :--- | :--- |
     | `F_Awake()` | 实体激活瞬间最早执行一次 | 初始化本脚本内部变量 |
     | `F_Start()` | 首帧更新前执行一次 | 用 `GetComponent<...>()` 获取同物体上的刚体、角色控制器等组件 |
     | `F_Update()` 或 `F_Update(float delta)` | 每一渲染帧执行一次（无参、带参两种写法都支持） | 读取键鼠输入、旋转移动、镜头控制、射线检测 |
     | `F_FixedUpdate()` 或 `F_FixedUpdate(float fixedDelta)` | 按固定物理步（默认 60 Hz）稳定执行 | 调用 `AddForce`、`F_CharacterController.Move` 等物理控制 |
     | `F_CollisionEnter(in F_Contact contact)` | 两个碰撞体刚撞上时触发 | 扣血、播音效、读取真实撞击点 `contact.Point` 与冲量 `contact.Impulse` |
     | `F_Destroy()` | 物体销毁或停止运行预览时触发 | 解锁鼠标（`F_Cursor.Locked = false`）、清理资源 |
5. **上面示例里 4 个最常用的语法写法**
   - **`var` 自动推断类型**：`var dir = new Vector3(...)` 等价于 `Vector3 dir = new Vector3(...)`，少敲重复类型名。
   - **命名参数（`localSpace: false`）**：显式写清布尔参数的含义。在 `Translate(..., localSpace: ...)` 里，`false` 表示按**世界固定方向**移动，`true` 表示按**物体自身脸朝向**移动。
   - **安全取组件（`TryGetComponent<T>(out var body)`）**：`<>` 尖括号里填组件类型，`out var body` 在找到组件时顺便把结果放进新变量 `body` 并返回 `true`；如果这个物体没挂刚体则返回 `false`，直接跳过跳跃逻辑而不报错。
   - **字符串插值（`$"Jump at {Entity.WorldPosition}"`）**：引号前加 `$` 后，大括号 `{...}` 里的表达式会自动转成文字拼进日志里，非常适合配合 `F_Log.Info(...)` 调试。

## 4. 属性面板暴露字段与拖拽直连实体

在脚本字段上加上 `[F_Expose]` 特性，保存并热重载后，字段就会自动出现在右侧 **属性** 面板里，并随 `Ctrl + S` 与场景一起保存。

- **支持直接暴露的字段类型**：`int`、`float`、`double`、`bool`、`string`、`Vector2`、`Vector3`、`F_Color`（自带 HSV 拾色器）、自定义 `enum`（下拉框），以及 **`F_Entity?`**（或 `F_EntityReference`）实体引用槽。
- **把场景实体直接拖给 `F_Entity?` 字段**：声明 `[F_Expose] public F_Entity? Target;` 后，直接从 **场景层级** 把任意物体拖进属性栏的 `Target` 槽位（或点槽位右侧的 **`⊙`** 搜索选择），运行期即可直接读写 `Target.Position`、`Target.GetComponent<...>()`，**无需手动 `.Resolve()`**！
- **声明式排版特性**：
  - `[F_Header("分组标题")]`：在字段上方画一个小标题分组。
  - `[F_Range(0f, 100f)]`：把数值框变成可拖拽滑块并限制范围。
  - `[F_Tooltip("鼠标悬停说明")]`：鼠标停在字段名上弹出提示气泡。
  - `[F_Space(10f)]`：在上方留出垂直间距。
  - `[F_ReadOnly]`：只读显示（方便实时查看运行态调试值）。

## 5. 可直接复制的统一 `F_` API 实战脚本

### 示例一：点击射线交互、物理推力、真实碰撞点与无线程异步延时

给一个挂有 **刚体与盒形碰撞** 的立方体挂上 `InteractiveBox.cs`，并把场景里的一个道具拖给 `SpawnTemplate` 槽、把目标点拖给 `LookTarget` 槽，就能一次性体验 `F_Entity?`、`F_Rigidbody`、`F_Physics.Raycast`、`F_Camera.ScreenPointToRay`、`F_Contact`、`F_Await` 和 `F_Log`：

```csharp
using System.Numerics;
using FlareEngine;

public enum BoxMood { Calm, Active, Boosted }

public class InteractiveBox : F_Behaviour
{
    [F_Header("移动与外观")]
    [F_Expose, F_Range(1f, 20f), F_Tooltip("刚体推力大小")]
    public float PushImpulse = 6f;

    [F_Expose] public BoxMood Mood = BoxMood.Active;
    [F_Expose] public F_Color AccentColor = F_Color.Cyan;

    [F_Space(8f)]
    [F_Header("场景对象直连引用（从场景层级直接拖进来）")]
    [F_Expose] public F_Entity? LookTarget;
    [F_Expose] public F_Entity? SpawnTemplate;

    [F_Expose, F_ReadOnly] public int HitCount = 0;

    private F_Rigidbody? _body;

    public override void F_Start()
    {
        _body = GetComponent<F_Rigidbody>();
        F_Log.Info($"InteractiveBox 已启动，初始坐标: {Entity.WorldPosition}");
    }

    public override void F_Update(float delta)
    {
        // 1. 屏幕鼠标位置转世界射线 + 物理射线检测
        if (F_Input.MouseDown(F_MouseButton.Left))
        {
            if (F_Camera.ScreenPointToRay(F_Input.MousePosition) is { } ray &&
                F_Physics.Raycast(ray.Origin, ray.Direction, 100f, out F_RaycastHit hit))
            {
                F_Log.Info($"射线命中: {hit.Entity.Name}，距离 {hit.Distance:F2}m，交点 {hit.Point}");

                // 如果点中自己，沿射线方向踢一脚，并异步生成特效副本
                if (hit.Entity.StableId == Entity.StableId && _body is not null)
                {
                    _body.AddImpulse(ray.Direction * PushImpulse);
                    SpawnSparkLater(hit.Point + hit.Normal * 0.25f);
                }
            }
        }

        // 2. 直接使用 F_Entity?，无需手动 Resolve
        if (LookTarget is { IsValid: true })
        {
            LookAt(LookTarget.WorldPosition);
        }
    }

    public override void F_FixedUpdate(float fixedDelta)
    {
        // 在固定物理步给刚体施加水平持续力
        if (_body is null) return;
        var move = F_Input.MoveVector;
        if (move.LengthSquared() > 0.001f)
        {
            _body.AddForce(new Vector3(move.X, 0f, -move.Y) * 18f);
        }
    }

    // 3. 真实物理流形碰撞回调：读取接触点、表面法线与法向冲量
    public override void F_CollisionEnter(in F_Contact contact)
    {
        HitCount++;
        F_Log.Info($"碰撞到 {contact.Other.Name} | 接触点: {contact.Point} | 法线: {contact.Normal} | 冲量: {contact.Impulse:F2}");
    }

    // 4. 主线程确定性异步延时（停止运行预览会自动安全取消，绝不跨线程崩溃）
    private async void SpawnSparkLater(Vector3 pos)
    {
        if (SpawnTemplate is not { IsValid: true }) return;
        var clone = Instantiate(SpawnTemplate, pos, F_Quat.Identity);
        await F_Await.Seconds(1.5f);
        if (clone.IsValid)
        {
            Destroy(clone);
        }
    }
}
```

### 示例二：第一人称胶囊角色控制器 + 锁定鼠标视角（含 `Esc` 释放）

给玩家实体添加 **胶囊角色控制器** 组件，并将 `Main Camera` 挂在玩家下作为子物体，再拖入下方 `CameraPivot` 槽位：

```csharp
using System.Numerics;
using FlareEngine;

public class FirstPersonMover : F_Behaviour
{
    [F_Header("镜头与控制")]
    [F_Expose] public F_Entity? CameraPivot;
    [F_Expose, F_Range(0.05f, 1.0f)] public float MouseSensitivity = 0.22f;

    private F_CharacterController? _controller;
    private float _yaw;
    private float _pitch;

    public override void F_Start()
    {
        _controller = GetComponent<F_CharacterController>();
        // 进入试玩自动锁定并隐藏光标；试玩中随时按 Esc 即可临时释放光标
        F_Cursor.Locked = true;
    }

    public override void F_Update(float delta)
    {
        if (!F_Cursor.Locked) return;

        var mouse = F_Input.MouseDelta;
        _yaw -= mouse.X * MouseSensitivity;
        _pitch = F_Math.Clamp(_pitch - mouse.Y * MouseSensitivity, -80f, 80f);

        Entity.EulerAngles = new Vector3(0f, _yaw, 0f);
        if (CameraPivot is { IsValid: true })
        {
            CameraPivot.EulerAngles = new Vector3(_pitch, 0f, 0f);
        }
    }

    public override void F_FixedUpdate(float fixedDelta)
    {
        if (_controller is null) return;

        var input = F_Input.MoveVector;
        var moveDir = Entity.Forward * input.Y + Entity.Right * input.X;
        moveDir.Y = 0f;
        if (moveDir.LengthSquared() > 1e-4f)
            moveDir = Vector3.Normalize(moveDir);

        bool jump = F_Input.KeyDown(F_Key.Space);
        var state = _controller.Move(moveDir, jump, fixedDelta);

        if (jump && state.Grounded)
        {
            F_Log.Info("起跳！");
        }
    }

    public override void F_Destroy()
    {
        F_Cursor.Locked = false;
    }
}
```

::: tip 关于 `F_Cursor.Locked = true` 与按 `Esc` 释放鼠标
在编辑器运行预览里开启 `F_Cursor.Locked = true` 后，不需要自己写额外的解锁逻辑：试玩途中按一下 **`Esc`**，引擎会自动暂停光标锁定并恢复系统指针，你可以直接移动鼠标去点顶栏的 **停止预览 (`■`)** 或修改属性面板；如果想继续试玩，在摄像机视口里点一下鼠标左键就会重新锁回视角。
:::

## 6. 常用 `FlareEngine` 高频 API 速查表

::: tip 需要查看每个 API 的完整参数表、结构体字段与用法示例？
前往 [**FlareEngine C# API 参考手册**](api/index.md)：[核心（实体/场景/特性）](api/core.md) · [输入/相机/数学/异步](api/input-math.md) · [物理/射线/角色](api/physics.md) · [音频/粒子/导航/UI/事件](api/systems.md)。
:::

| 需求分类 | 常用写法 |
| :--- | :--- |
| **生命周期回调** | `F_Awake()` → `F_Enable()` → `F_Start()` → `F_PreUpdate(dt)` / `F_FixedUpdate(fdt)` / `F_Update(dt)` / `F_LateUpdate(dt)` → `F_Disable()` → `F_Destroy()` |
| **碰撞与触发回调** | `F_CollisionEnter / Stay / Exit(in F_Contact contact)`（含 `Other / Point / Normal / Impulse`） · `F_TriggerEnter / Stay / Exit(F_Entity other)` |
| **空间变换与朝向** | `Entity.Position`、`Entity.WorldPosition`、`Entity.Rotation`、`Entity.WorldRotation`、`Entity.EulerAngles`、`Entity.LocalScale`、`Entity.Forward / Right / Up`、`Translate(...)`、`Rotate(...)`、`LookAt(...)` |
| **标签与层级** | `Entity.Tag`、`Entity.CompareTag("Player")`、`Entity.Layer`、`Entity.SetParent(parent)` |
| **组件与场景查询** | `GetComponent<F_Rigidbody>()`、`TryGetComponent<F_CharacterController>(out var cc)`、`F_Scene.Find("Cube")`、`F_Scene.FindWithTag("Player")`、`F_Scene.FindObjectOfType<T>()`、`F_Scene.Reload()` |
| **克隆与延时销毁** | `var go = Instantiate(sourceEntity, pos, rot);` · `Destroy(go);` · `Destroy(go, 3.0f);` |
| **键鼠、光标与射线** | `F_Input.Key / KeyDown / KeyUp(F_Key.Space)`、`F_Input.Mouse / MouseDown / MouseUp(F_MouseButton.Left)`、`F_Input.Horizontal / Vertical / MoveVector`、`F_Input.MouseDelta`、`F_Cursor.Locked`、`F_Camera.ScreenPointToRay(pos)` |
| **物理世界** | `F_Physics.Raycast(origin, dir, dist, out F_RaycastHit hit)`、`body.Velocity`、`body.AngularVelocity`、`body.AddForce(f)`、`body.AddImpulse(imp)` |
| **时间与数学** | `F_Time.DeltaTime / FixedDeltaTime / UnscaledDeltaTime / Time / TimeScale` · `F_Math.Clamp / Lerp / MoveTowards / SmoothDamp / PingPong` · `F_Quat.Euler / ToEuler / LookRotation / Slerp` |
| **主线程异步时序** | `await F_Await.Seconds(1.0f);` · `await F_Await.NextFrame();` · `await F_Await.FixedUpdate();` |
| **控制台日志** | `F_Log.Info(...)`、`F_Log.Warn(...)`、`F_Log.Error(...)` |
