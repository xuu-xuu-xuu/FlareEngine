# 托管宿主与原生 ABI

本文说明 .NET 托管层（`FlareEngine.Bootstrap` + `FlareEngine.Managed`）与 C++ 原生引擎（`EngineNative.dll`）之间的 `flare_*` / `FLARE_*` C ABI 契约，包括异常边界、结构体二进制对齐、零运行时封送 P/Invoke 以及物理射线首帧同步、光标上升沿解锁保护等关键跨边界细节。

## 从进程入口到跨语言边界

```text
FlareEngine.Bootstrap (Program.cs)
  │  解析 --editor / --project / --scene / --bindings / --window / --verify-runtime
  ▼
EngineSession (FlareEngine.Managed.dll)
  │  1. flare_get_abi_info() 核对 ABI 版本、Transform 字节数与 64 位指针
  │  2. flare_session_create() 获取 64 位代数句柄，包装进 NativeSessionHandle (SafeHandle)
  │  3. 分配弱 GCHandle 作为回调 context，注册 6 类 [UnmanagedCallersOnly(Cdecl)] 回调
  ▼
[LibraryImport("EngineNative")]  ◀── DisableRuntimeMarshalling=true（编译期源生成）
  ═══════════════════════════ C ABI 边界 (extern "C" noexcept) ═══════════════════════════
EngineNative.dll (EngineNative.h / EngineNative.cpp / Native*Exports.inl)
  │  Protect([&]{ lock_guard(sessionsMutex); Find(handle, checkThread); ... })
  │  回调托管前用 RAII Unlock 临时解锁，防止回调重入死锁
  ▼
flare::Environment / World / PhysicsWorld / AudioWorld / UiTree / EditorSession
```

编辑器工作进程无论项目是否已有 `.cs` 脚本，都统一由 [`FlareEngine.Bootstrap/Program.cs`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Bootstrap/Program.cs#L26) 拉起（初始无脚本项目传入 `--project ""`），从而在同一个进程内完成后续新建脚本与热重载。

## `flare_*` C ABI 异常边界与线程契约

所有导出函数声明在 [`EngineNative.h`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/NativeBridge/EngineNative.h#L17)，遵循三条硬约束：
1. **绝不让 C++ 异常跨越 C 边界**：所有导出签名带 `FLARE_NOEXCEPT` 并返回 `int32_t` 错误码（[`FLARE_Result`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/NativeBridge/EngineNative.h#L23)），真实实现全部包在 [`Protect(...)`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/NativeBridge/EngineNative.cpp#L198) 内，错误文本键写入 `thread_local lastError` 供 `flare_get_last_error()` 读出。
2. **绝不跨边界释放内存**：所有缓冲区均为调用方借用（caller-owned）。变长字符串与事件数组采用统一的两段式协议——先传 `capacity = 0` 查询所需字节数或条数（返回 `FLARE_BUFFER_TOO_SMALL = 3`），分配精确缓冲后再调用一次拷贝（见 [`EngineSession.ReadString`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/EngineSession.cs#L341)）。
3. **会话线程亲和 + 64 位代数句柄**：除 `flare_session_close`（允许 GC 终结器关闭空闲会话）和 `flare_event_publish`（工作线程弱发布端）外，[`Find(handle)`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/NativeBridge/EngineNative.cpp#L204) 校验 `(generation << 32) | (index + 1)` 与创建线程 ID，错线程立即返回 `FLARE_WRONG_THREAD = 6`。

```cpp
// Source/FunctionLayer/Scripting/NativeBridge/EngineNative.h#L23-L27
enum FLARE_Result {
    FLARE_OK = 0, FLARE_INVALID_ARGUMENT = 1, FLARE_INVALID_HANDLE = 2,
    FLARE_BUFFER_TOO_SMALL = 3, FLARE_BUSY = 4, FLARE_CALLBACK_FAILED = 5,
    FLARE_WRONG_THREAD = 6, FLARE_OUT_OF_MEMORY = 7, FLARE_INTERNAL_ERROR = 8
};

// Source/FunctionLayer/Scripting/NativeBridge/EngineNative.cpp#L198-L203
template<class Function> int32_t Protect(Function&& function) noexcept
{
    try { std::lock_guard lock(sessionsMutex); Error(FLARE_OK, "Bridge.Success"); return function(); }
    catch (const std::bad_alloc&) { return Error(FLARE_OUT_OF_MEMORY, "Bridge.OutOfMemory"); }
    catch (...) { return Error(FLARE_INTERNAL_ERROR, "Bridge.NativeException"); }
}
```

## 56 字节 `FLARE_ContactEvent` 与双向布局对齐

所有跨边界结构体均只使用定宽数字与顺序布局，显式补齐尾部 `reserved` 填充字段，并在 C++ 编译期用 `static_assert` 锁死字节大小（[`EngineNative.cpp#L602-L611`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/NativeBridge/EngineNative.cpp#L602)），同时在 C# 会话启动时校验关键结构体大小（[`EngineSession.cs#L88-L100`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/EngineSession.cs#L88)）。

以 56 字节的物理碰撞接触事件为例，C 端与 C# 端字段逐字节一一对应：

```c
// C ABI: Source/FunctionLayer/Scripting/NativeBridge/EngineNative.h#L54-L60
typedef struct FLARE_ContactEvent {
    uint64_t entity_a, entity_b;   // offset  0,  8 (16 B)
    int32_t began, ended;          // offset 16, 20 ( 8 B)
    FLARE_Vector3 point, normal;   // offset 24, 36 (24 B)
    float impulse;                 // offset 48     ( 4 B)
    uint32_t reserved;             // offset 52     ( 4 B) ── 总计 56 B（8 字节对齐）
} FLARE_ContactEvent;
```

```csharp
// C# 镜像: Source/FunctionLayer/Scripting/Managed/Physics.cs#L69-L77
[StructLayout(LayoutKind.Sequential)]
internal struct NativeContact
{
    internal ulong EntityA, EntityB; // offset  0,  8
    internal int Began, Ended;       // offset 16, 20
    internal Vector3 Point, Normal;  // offset 24, 36
    internal float Impulse;          // offset 48
    internal uint Reserved;          // offset 52 ── 总计 56 B
}
```

关键 ABI 结构体尺寸速查（均在 `EngineNative.cpp#L602-L611` 通过 `static_assert` 校验）：

| C 结构体 | 字节数 | C# 对应结构体 | 说明 |
| :--- | :---: | :--- | :--- |
| `FLARE_AbiInfo` | 16 | `NativeMethods.AbiInfo` | ABI 版本、Transform 字节数、指针宽度 |
| `FLARE_Transform` | 40 | `Transform` / `F_Transform` | `Vector3 + Quaternion + Vector3` |
| `FLARE_BodyState` | 72 | `RigidBodyState` / `F_RigidBodyState` | 位姿 + 线/角速度 + 质量 + 休眠标志 |
| `FLARE_RayHit` | 48 | `NativeRayHit` | 刚体句柄 + 实体 ID + 交点 + 法线 + 距离 |
| `FLARE_ContactEvent` | 56 | `NativeContact` | 碰撞双方 + 起止边沿 + 流形点/法线/冲量 |
| `FLARE_TriggerEvent` | 24 | `NativeTrigger` | 触发器实体 + 对方实体 + 进入/退出标志 |
| `FLARE_AudioPlayDesc` | 40 | `AudioPlayDescription` | 3D 位置、音量、音高、衰减距离、循环、优先级 |
| `FLARE_EventHeader` / `FLARE_EventStats` | 88 / 104 | `EventHeader` / `EventStatistics` | 事件总线头与统计结构 |

## 关键跨边界行为实现

### `flare_physics_raycast` 首帧同步

如果脚本在 `F_Awake()` / `F_Start()`（或刚改动实体变换但尚未步进物理模拟）时发射射线，原生 `PhysicsWorld` 里可能还未从 `World` 组件同步出最新碰撞体。[`flare_physics_raycast`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/NativeBridge/NativePhysicsExports.inl#L203) 在执行查询前主动调用 `scenePhysics.Synchronize(...)`，确保**首帧与同帧变换修改后立即可命中**：

```cpp
// Source/FunctionLayer/Scripting/NativeBridge/NativePhysicsExports.inl#L203-L212
int32_t FLARE_CALL flare_physics_raycast(
    FLARE_Session handle, FLARE_Vector3 origin, FLARE_Vector3 direction,
    float distance, uint32_t mask, FLARE_RayHit* output, int32_t* found) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle); if (!s) return MissingSession();
        if (!output || !found || !std::isfinite(distance) || distance <= 0)
            return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        // 首帧/变换变更后同步场景刚体到物理世界，保证射线即时命中
        if (!s->scenePhysics.Synchronize(s->WorldRef(), s->physics))
            return Error(FLARE_INTERNAL_ERROR, "Bridge.PhysicsFailed");
        *output = {}; *found = 0;
        const auto hit = s->physics.Raycast(Vector(origin), Vector(direction), distance, mask);
        if (hit) { *found = 1; *output = { /* ... */ }; }
        return FLARE_OK;
    });
}
```

### `F_Cursor.Locked` 上升沿解锁保护 {#cursor-lock}

在第一人称游戏中，脚本常在 `F_Update()` 里写 `F_Cursor.Locked = true;`。为了让玩家在编辑器 Play Mode 按 `Esc` 释放鼠标后不被脚本下一帧强行抢回，[`flare_input_set_cursor_mode`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/NativeBridge/NativeInputExports.inl#L80) 只在 **`false → true` 的上升沿**（或玩家主动点击游戏视口，见 [`CaptureInput`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/NativeBridge/EngineNative.cpp#L386)）清除 `cursorLockPausedByEsc`：

```cpp
// Source/FunctionLayer/Scripting/NativeBridge/NativeInputExports.inl#L80-L108
int32_t FLARE_CALL flare_input_set_cursor_mode(FLARE_Session handle, int32_t locked, int32_t visible) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle); if (!s) return MissingSession();
        const bool nextLocked = locked != 0, nextVisible = visible != 0;
        const bool wasLocked = s->cursorLocked;
        if (!nextLocked && wasLocked) ReleaseSessionCursor(*s);
        s->cursorLocked = nextLocked;
        s->cursorVisible = nextVisible;
        if (nextLocked && !wasLocked)       // 仅上升沿清除 Esc 临时解锁，防止每帧重锁对抗玩家
            s->cursorLockPausedByEsc = false;
        if (!nextLocked && nextVisible) ReleaseSessionCursor(*s);
        return FLARE_OK;
    });
}

int32_t FLARE_CALL flare_input_get_cursor_mode(FLARE_Session handle, int32_t* locked, int32_t* visible) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle); if (!s) return MissingSession();
        if (locked) *locked = (s->cursorLocked && !s->cursorLockPausedByEsc) ? 1 : 0;
        if (visible) *visible = s->cursorVisible ? 1 : 0;
        return FLARE_OK;
    });
}
```

此外，窗口失焦或退出运行态时，`CaptureInput` 与 `~Session()` 会通过 RAII 自动调用 `ClipCursor(nullptr)` 与 `ShowCursor(TRUE)`（[`EngineNative.cpp#L79-L82`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/NativeBridge/EngineNative.cpp#L79)），绝不把鼠标卡死在系统桌面。

### `[LibraryImport]` 零开销 P/Invoke

`Engine.Managed.csproj` 开启了 `<DisableRuntimeMarshalling>true</DisableRuntimeMarshalling>`（[`Engine.Managed.csproj#L3`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Engine.Managed.csproj#L3)），[`NativeMethods`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/NativeMethods.cs#L7) 全部采用 .NET 编译期源生成的 `[LibraryImport]`，且所有 `blittable` 结构体直接按值或 `in/out` 指针传递，无运行时封送 stub 开销：

```csharp
// Source/FunctionLayer/Scripting/Managed/NativeMethods.cs#L17-L26
[LibraryImport(Library, EntryPoint = "flare_get_abi_info")]
internal static partial int Abi(out AbiInfo info);

[LibraryImport(Library, EntryPoint = "flare_session_create")]
internal static partial int Create(out ulong handle);

[LibraryImport(Library, EntryPoint = "flare_session_set_update")]
internal static partial int SetUpdate(
    ulong handle, delegate* unmanaged[Cdecl]<ulong, double, nint, int> callback, nint context);
```

## 托管子系统转发速查

| 子系统 | 托管入口 | 原生导出分片（`Source/FunctionLayer/Scripting/NativeBridge/`） |
| :--- | :--- | :--- |
| 会话、窗口、抓帧与编辑桥 | `EngineSession` | `NativeSessionExports.inl`、`NativeRenderExports.inl` |
| 实体、变换、克隆与场景服务 | `F_Entity`、`F_Scene`、`SceneService` | `NativeWorldExports.inl` |
| 刚体、射线、触发器与角色控制 | `Physics`、`F_RigidBody`、`F_CharacterController` | `NativePhysicsExports.inl` |
| 键鼠输入、动作表与光标模式 | `F_Input`、`F_InputActions`、`F_Cursor` | `NativeInputExports.inl` |
| 3D 音频 / 粒子 / 导航 | `F_Audio`、`F_Particles`、`F_Navigation` | `NativeAudioExports.inl`、`NativeParticlesExports.inl`、`NativeNavigationExports.inl` |
| 游戏界面树 | `F_GameUi`、`F_UiElement` | `NativeUiExports.inl` |
| 追踪式逻辑对象堆 | `LogicState` | `NativeLogicExports.inl` |

## 运行诊断与任务依赖

- **启动与运行诊断**：`BootstrapDiagnostics` 记录托管启动与会话阶段检查点；`RuntimePreflight.Verify()`（`--verify-runtime`）执行端到端管道与程序集加载自检；独立预览目录由 `PreviewLease` 持有独占文件锁隔离并发会话；`EngineSession.SetWindowProfile()`（[`WindowProfiling.cs#L9`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/WindowProfiling.cs#L9)）配置帧耗时与 GPU 采集输出。
- **跨组件调度依赖**：通过 [`ScriptSystem.SetTickPrerequisites()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptSystem.cs#L447) 传入 [`TickDependency`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/EnvironmentScheduling.cs#L7)，交由原生世界的[任务调度器](../runtime/scheduling.md#tickscheduler)按拓扑顺序执行。

::: warning 新增 C ABI 接口时的检查清单
1. 函数名统一 `flare_<子系统>_<动作>`，签名加 `FLARE_API int32_t FLARE_CALL ... FLARE_NOEXCEPT`，实现体套 `Protect([&]{ ... })`。
2. 新增结构体只使用定宽类型并在尾部显式补齐对齐填充，同步在 `EngineNative.cpp` 追加 `static_assert(sizeof(...) == ...)`。
3. C# 端在 `NativeMethods` 用 `[LibraryImport(Library, EntryPoint = "flare_...")]` 声明，`bool` 统一传 `int`（`0/1`），字符串显式指定 `StringMarshalling = StringMarshalling.Utf8` 并附带 UTF-8 字节长度。
:::
