# 脚本反射、特性元数据、程序集卸载与 IDE 智能提示

本文解释三个底层机制：一、C# 字段与特性怎样被反射进编辑器 Inspector 并安全序列化；二、用户脚本 DLL 如何借助可回收 `AssemblyLoadContext` 实现零文件锁卸载与 GC；三、编辑器如何自动同步外部 IDE 的 `.sln` 工程并提供 `FlareEngine.Managed.xml` 中文智能提示。

## 整体链路图

```text
【字段反射与零锁 Schema 检查】
  用户脚本 .cs ──(dotnet build)──▶ bin-<seq>/FlareGame.Scripts.dll
                                         │
                             LoadFromStream (内存流，无文件锁)
                                         ▼
                       临时可回收 ProjectContext (ALC)
                         ├─ ScriptFields.Describe(instance)
                         │    └─ [F_Expose] + Header/Range/Tooltip/Space/ReadOnly
                         └─ finally: context.Unload()  (仅留 WeakReference 观察 GC)
                                         │
                                         ▼
                       ScriptBindings 缓存纯 JSON Schema ──▶ EditorInspector

【外部 IDE 工作区自动同步与防冲突隔离】
  打开项目 / 双击脚本 ──▶ EditorScriptTools::SyncIdeWorkspace()
       ├─ 生成 <Project>.sln                 (确定性 GUID，VS 2022 格式)
       ├─ 生成 out/Editor/FlareEngine.Ide.props (锁定本机 FlareEngineRoot + 全局 using)
       ├─ 维护 Scripts/Directory.Build.props (DefaultItemExcludes 屏蔽 IDE obj/bin)
       ├─ 生成 .vscode/settings.json         (绑定默认解决方案)
       └─ 同步 FlareEngine.Managed.dll + FlareEngine.Managed.xml (中文 XML 智能提示)
```

## `[F_Expose]` 反射与 Inspector 特性

[`ScriptFields`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptFields.cs#L15) 沿继承链枚举可写实例字段（包含基类私有 `[F_Expose]` 字段，按字段名稳定排序；若继承链出现同名 `[F_Expose]` 字段会明确抛出 `Script.FieldTypeChanged`）。为避免把项目 `Type` / `FieldInfo` 长期根在宿主内存里阻断卸载，**每次操作均临时枚举，不设跨加载缓存**（[`ScriptFields.cs#L47-L62`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptFields.cs#L47)）。

### 支持的字段类型（含直接 `F_Entity?` 槽位）

| C# 字段类型 | `.bindings.json` Kind | 序列化与运行期行为 |
| :--- | :--- | :--- |
| `int` / `float` / `double` | `"i32"` / `"f32"` / `"f64"` | 拒绝 `NaN` / `Infinity`，可叠加 `[F_Range]` 渲染滑条。 |
| `bool` / `string` | `"bool"` / `"string"` | 字符串上限 65,536 字符。 |
| `Vector2` / `Vector3` | `"vec2"` / `"vec3"` | 序列化为 `{ "X", "Y" (, "Z") }`，校验有限浮点数。 |
| `F_Color` | `"color"` | 线性空间 RGBA（`R/G/B/A`，缺省 `A=1`），Inspector 渲染颜色选择器。 |
| `enum` | `"enum"` | 按枚举成员名字符串保存，Inspector 渲染下拉选择框并校验枚举定义。 |
| `F_Entity?` / `Entity?` / `F_EntityReference` / `EntityReference` | `"entity"` | **统一以 64 位 `StableId` 十进制字符串持久化**；运行时加载/实时改值时自动解析为目标实体实例或引用结构体。 |

`ScriptFields` 对直接 `public F_Entity? Target;` 槽位与显式 `F_EntityReference` 走同一套转换器（[`ScriptFields.cs#L17-L34`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptFields.cs#L17)）：

```csharp
private static bool IsEntityRefType(Type type)
    => type == typeof(EntityReference) || type == typeof(F_EntityReference) || typeof(F_Entity).IsAssignableFrom(type);

private static ulong ExtractEntityStableId(object? val) => val switch
{
    EntityReference r => r.StableId,
    F_EntityReference fr => fr.StableId,
    F_Entity ent => ent.IsValid ? ent.StableId : 0,
    _ => 0
};

private static object? ConvertEntityFieldValue(Type fieldType, ulong stableId, EngineSession? session)
{
    if (fieldType == typeof(F_EntityReference)) return new F_EntityReference(stableId);
    if (typeof(F_Entity).IsAssignableFrom(fieldType))
        return stableId == 0 || session is null ? null : session.FindEntity(stableId);
    return new EntityReference(stableId);
}
```

### Inspector 布局与修饰特性

全部特性定义于 [`Behaviour.cs#L10-L36`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Behaviour.cs#L10)，经 [`ScriptFields.Describe()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptFields.cs#L108) 提取到 `ExposedField` 后随 JSON 传给编辑器面板：

| 特性 | 签名 | Inspector 表现 |
| :--- | :--- | :--- |
| `[F_Expose]` | `[F_Expose]` | 必须标注，标记字段暴露到面板并写入 `.bindings.json`。 |
| `[F_Header]` | `[F_Header(string title)]` | 在该字段上方插入分组小标题与分隔线。 |
| `[F_Range]` | `[F_Range(float min, float max)]` | 数值字段渲染为可拖拽滑块 + 精确数值输入并钳制区间。 |
| `[F_Tooltip]` | `[F_Tooltip(string text)]` | 鼠标悬停字段标签时显示中文/英文提示说明。 |
| `[F_Space]` | `[F_Space(float pixels = 8f)]` | 在当前字段上方增加指定像素垂直留白。 |
| `[F_ReadOnly]` | `[F_ReadOnly]` | 在面板中置灰为只读态，适合运行时状态观测。 |

`ScriptFields` 还提供：
- [`Save()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptFields.cs#L166) / [`Load()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptFields.cs#L189)：加载时采用“先完整校验并构造候选列表、全部通过后再停止旧系统并挂载”的事务风格，损坏 JSON 不会清空现有脚本配置。
- [`CopyExposedFields()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptFields.cs#L125)：供 `F_Scene.Instantiate()` 深克隆暴露字段并按 `stableIdRemap` 重映射子树内实体槽位。
- [`ClearSceneReferences()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptFields.cs#L36)：当叠加场景卸载时，自动清空存活脚本里指向已退役实体的 `F_Entity?` / `F_EntityReference` 字段。

C++ 原生组件反射见[原生组件反射](../runtime/metadata.md)。

## ScriptProject：可回收程序集上下文与 GC 卸载

[`ScriptProject`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptProject.cs#L7) 内部使用继承自 `AssemblyLoadContext(isCollectible: true)` 的 `ProjectContext` 隔离项目脚本与依赖：

```csharp
// Source/FunctionLayer/Scripting/Managed/ScriptProject.cs#L9-L21
internal sealed class ProjectContext(string assemblyPath) : AssemblyLoadContext(isCollectible: true)
{
    private readonly AssemblyDependencyResolver resolver = new(assemblyPath);
    protected override Assembly? Load(AssemblyName name)
    {
        // 引擎 SDK 统一复用宿主默认上下文，确保 F_Behaviour 类型身份完全一致
        if (name.Name == typeof(F_Behaviour).Assembly.GetName().Name) return typeof(F_Behaviour).Assembly;
        var path = resolver.ResolveAssemblyToPath(name);
        if (path is null) return null;
        using var stream = File.OpenRead(path);
        return LoadFromStream(stream); // 流式加载：零文件锁，随时允许重新编译覆盖 DLL
    }
    protected override nint LoadUnmanagedDll(string name)
        => throw new InvalidOperationException("Script.ProjectNativeDependencyDenied");
}
```

保证旧程序集可被 .NET GC 完整回收的 4 条红线：
1. **编辑态绝不长期持有项目 `Assembly` / `Type`**：[`ScriptBindings.LoadSchema()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptBindings.cs#L126) 标注 `[MethodImpl(MethodImplOptions.NoInlining)]` 防止栈帧延长局部引用，提取完 JSON 后在 `finally` 中立即 `context.Unload()`。
2. **宿主仅持 `WeakReference` 观察卸载**：`ScriptProject.LastUnloadedContext`（[`ScriptProject.cs#L33`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptProject.cs#L33)）与 `ScriptBindings.LastUnloadedSchemaContext`（[`ScriptBindings.cs#L31`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptBindings.cs#L31)）均不保活上下文。
3. **停止运行时完整断边**：[`ScriptProject.Release()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptProject.cs#L191) 先解绑 `Diagnostic`、调用 `scripts.Dispose()` 清空所有组件/事件/UI 动作/协程，再 `registry.Clear()` 并调用 `unloading.Unload()`。
4. **禁止项目直接加载非托管 DLL**：`LoadUnmanagedDll` 直接拒绝，所有原生能力统一经 `FlareEngine.Managed` 托管边界访问。

## 外部 IDE `.sln` 自动同步与中文智能提示

为了让 VS Code、Visual Studio、JetBrains Rider 或 Cursor 打开项目 `.cs` 文件即可获得零报错的 API 补全与中文悬停文档，引擎在两处做了完整配合：

### 1. `FlareEngine.Managed.xml` 中文文档产物

[`Engine.Managed.csproj`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Engine.Managed.csproj#L1) 开启了 XML 文档生成，构建时会在 `FlareEngine.Managed.dll` 同目录产出包含全部中文 `<summary>` 注释的 `FlareEngine.Managed.xml`：

```xml
<!-- Source/FunctionLayer/Scripting/Managed/Engine.Managed.csproj -->
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <DisableRuntimeMarshalling>true</DisableRuntimeMarshalling>
    <AssemblyName>FlareEngine.Managed</AssemblyName>
    <RootNamespace>FlareEngine</RootNamespace>
    <GenerateDocumentationFile>true</GenerateDocumentationFile>
    <NoWarn>$(NoWarn);1591</NoWarn>
  </PropertyGroup>
</Project>
```

当编辑器从便携发行包运行（本地无引擎源码目录）时，`EditorScriptTools::SyncIdeWorkspace()` 会自动把宿主目录下的 `FlareEngine.Managed.dll` **连同 `FlareEngine.Managed.xml`** 一并同步到 `out/Editor/IdeSdk/SDK/Managed/`（[`EditorScriptTools.cpp#L515-L528`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorScriptTools.cpp#L515)），因此即便独立发行版也能享有完整中文 IntelliSense。

### 2. `SyncIdeWorkspace` 与 `DefaultItemExcludes` 隔离 IDE `obj/bin`

打开项目（`EditorScriptWorkflow` 构造）或从编辑器双击打开 `.cs`（[`EditorScriptTools::OpenSource()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorScriptTools.cpp#L762)）时，[`EditorScriptTools::SyncIdeWorkspace()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorScriptTools.cpp#L462) 自动生成并维护 4 个工作区文件：

| 文件路径 | 作用 | 是否纳入版本控制 |
| :--- | :--- | :--- |
| `<项目名>.sln` | Visual Studio 2022 格式解决方案，用 FNV-1a 生成确定性项目 GUID 避免反复 diff。 | 可选（可随时自动重建） |
| `out/Editor/FlareEngine.Ide.props` | 写入本机绝对路径 `FlareEngineRoot`、`<ImplicitUsings>enable</ImplicitUsings>`、`<Nullable>enable</Nullable>` 与全局 `<Using Include="FlareEngine" />`。 | **不要提交**（含本机路径） |
| `Scripts/Directory.Build.props` | 便携导入 `../out/Editor/FlareEngine.Ide.props`，并配置 `DefaultItemExcludes` 屏蔽 `obj/**;bin/**`。 | 建议随项目保留 |
| `.vscode/settings.json` | 首次创建时写入 `dotnet.defaultSolution` 与 `omnisharp.defaultLaunchSolution`，引导 C# Dev Kit / OmniSharp 直接加载 `.sln`。 | 可选 |

::: warning 为什么必须配置 `DefaultItemExcludes` 隔离 IDE 的 `obj/bin`？
外部 IDE 做设计时还原与分析时会在 `Scripts/obj/` 下生成 `.NETCoreApp,Version=v10.0.AssemblyAttributes.cs` 等自动代码文件；而编辑器内热重载会把 `BaseIntermediateOutputPath` 重定向到 `out/Editor/HotReload/obj/`。

如果缺少排除规则，热重载触发 MSBuild 默认的 `**/*.cs` 通配扫描时，会把 IDE 遗留在 `Scripts/obj/` 里的自动文件一并编进去，立刻报 `CS0579` 重复特性错误！因此 [`PortableDirectoryBuildProps`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorScriptTools.cpp#L132) 与热重载专用 [`HotReload.Directory.Build.props`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorScriptWorkflow.cpp#L411) 双重注入了：

```xml
<DefaultItemExcludes>$(DefaultItemExcludes);obj/**;bin/**</DefaultItemExcludes>
```

旧项目的空壳 `Directory.Build.props` 也会在 `SyncIdeWorkspace` 时经 `IsLegacyStubDirectoryBuildProps()` 自动升级。
:::

此外，[`OpenSource()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorScriptTools.cpp#L762) 会自动调用 `SyncIdeWorkspace(..., restoreDesignTimeAssets: true)` 在缺少 `Scripts/obj/project.assets.json` 时静默跑一次 `dotnet restore`，并通过 [`DetectInstalledCodeEditor()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorScriptTools.cpp#L621) 自动探测系统关联或已安装的 VS Code / Cursor / Antigravity / Windsurf / Visual Studio / Rider，结合 `{file}`、`{line}`、`{column}`、`{project}`、`{sln}` 占位符直接跳转到报错行。

## 原生逻辑对象的追踪回收

除 .NET GC 外，原生世界的 `World::Objects()` 还提供了面向逻辑图的非移动标记清扫堆 `ObjectHeap`：

- 原生对象派生自 `HeapObject`，在 `Trace(ObjectTracer&)` 中枚举强引用边；`ObjectHandle<T>` 为带堆身份、槽位与代数的弱句柄，需经 `Root(handle)` 显式保活。
- `World::CollectObjects()` 在世界更新完成的安全点回收整个不可达引用环；编辑世界与播放世界各自拥有独立堆。
- C# 侧可通过 [`EngineSession.CreateLogicState()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/LogicObjects.cs#L37) 创建 [`LogicState`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/LogicObjects.cs#L6) 弱代理，使用 `Link()` 建立有向边、`Retain()` 获取 `IDisposable` 根令牌，并在安全点调用 `CollectLogicObjects()` 触发回收（在回调内调用会被拒绝）。

实体和组件寿命由[原生世界](../runtime/entities.md)管理，GPU 资源另受[提交 Fence](../render/device.md)约束。
