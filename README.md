<p align="center">
  <img src="Resources/Branding/FlareMark-README.png" alt="Flare 引擎图标" width="180">
</p>

<h1 align="center">Flare Engine</h1>

<p align="center">
  基于 <strong>C++20</strong>、<strong>DirectX 12 (SM 6.5 / DXR 1.1)</strong> 与 <strong>.NET 10</strong> 构建的现代自研 3D 游戏引擎与黑曜金编辑器
</p>

<p align="center">
  <a href="https://xuu-xuu-xuu.github.io/FlareEngine/"><strong>开发者文档</strong></a> ·
  <a href="https://xuu-xuu-xuu.github.io/FlareEngine/user/"><strong>使用者文档</strong></a> ·
  <a href="#快速编译与启动"><strong>快速开始</strong></a> ·
  <a href="#c-脚本与零重启热重载"><strong>C# 脚本示例</strong></a> ·
  <a href="#仓库目录导览"><strong>源码结构</strong></a>
</p>

---

## 核心特性

- **DirectX 12 现代渲染与混合硬件光追**
  - **光照与阴影**：多散射能量守恒 PBR、法线高光抗锯齿、三级级联阴影（CSM）+ PCSS 软阴影、静态级联阴影底图缓存、Forward+ 空间分簇点光（256 灯）、可投影点光源与聚光锥、LTC 矩形面光源。
  - **全局光照与遮蔽**：深度法线预绘制、半分辨率双边升采样 GTAO、方向光接触阴影、球谐漫反射探针、时域 A-Trous 降噪屏幕空间间接光（SSGI）、屏幕空间反射（SSR）与 Beer–Lambert 体积雾。
  - **DXR 1.1 硬件光线追踪**：基于 Inline RayQuery 与 GPU 持久静态网格 BLAS 缓存，提供光追方向软阴影、光追环境遮蔽（RTAO）、镜面/粗糙反射与单次反弹全局漫反射（RTGI），内置 SVGF 时空方差引导滤波；不支持硬件光追时零开销回退光栅与屏幕空间路径。
  - **GPU 驱动与后处理**：SM 6.5 硬件 Mesh Shader 与 Compute HZB 遮挡剔除双轨管线、FrameGraph 瞬态显存物理别名、ACES 电影级色调映射、自动曝光、TAA 与 4× 空间超采样。
- **黑曜金（Obsidian & Solar Gold）自研 UI 单栈编辑器**
  - **零第三方 IMGUI 依赖**：编辑器与运行时游戏 UI 共用自研 UI Toolkit 单栈，支持多窗口页签拖出、五向贴边停靠、Per-Monitor DPI 缩放与 DirectWrite 清晰字形图集。
  - **直觉化创作流**：Unity 风格 2×3 默认布局；**摄像机视口**原生支持「窗口预览 / 全屏预览」一键切换；**属性面板**采用卡片式组件布局、右侧 `⋮` 菜单、`[F_Header]` 分组、`[F_Range]` 填充滑条、`↔` 标签拖拽、四则运算表达式（如 `360/8`、`2*pi`）以及 **Play Mode 运行中实时调参**（停止自动还原）；打通 `场景层级 ↔ 属性引用槽 ↔ 资源面板 ↔ 场景视口` 全链路拖拽与多级文件夹分类图标管理。
- **.NET 10 C# 脚本与零窗口重启热重载**
  - **原位内存流热替换**：点击 `+ 添加组件` 搜索框输入新名字即可一键生成 `.cs`、后台无窗口增量编译并挂载；在外部 IDE 保存代码切回编辑器自动热重载。依托可回收 `AssemblyLoadContext` 与 `LoadFromStream` 无锁加载，全程 **0 窗口重启**且保留已调字段值。
  - **高频 `F_` 游戏 API**：内置 `F_Behaviour`（14 项生命周期与碰撞/触发回调）、`GetComponent<T>()`、`Instantiate`、延时 `Destroy`、`F_Time`、`F_Input` / `F_Key`、`F_Math` / `F_Quat`、直通编辑器控制台的 `F_Log` 以及主循环确定性异步等待 `F_Await`。
- **完整运行时与发行工具链**
  - 稳定十进制 ID 的 JSON 场景与 `.prefab.json` 预制体覆盖同步、盒形刚体/触发器与胶囊角色控制器、`NavMesh` 后台体素烘焙与 A* 漏斗寻路、glTF/GLB 骨骼动画与 GPU 蒙皮、XAudio2 3D 空间音频、确定性 SoA 粒子模拟、昼夜大气联动，以及一键独立游戏包导出与固定步 H.264 MP4 连续抓帧编码。

## C# 脚本与零重启热重载

在编辑器选中任意实体，点击 **+ 添加组件** 输入 `SpinCube` 回车即可创建并挂载脚本；修改保存后切回窗口自动增量热重载：

```csharp
using System.Numerics;
using FlareEngine;

public class SpinCube : F_Behaviour
{
    [F_Header("运动与朝向")]
    [F_Expose, F_Range(0f, 360f)] public float TurnSpeed = 90f;
    [F_Expose] public float MoveSpeed = 4f;
    [F_Expose] public EntityReference LookTarget;

    public override void F_Update(float delta)
    {
        var move = new Vector3(F_Input.Horizontal, 0f, F_Input.Vertical);
        Translate(move * (MoveSpeed * F_Time.DeltaTime), localSpace: false);
        Rotate(new Vector3(0f, TurnSpeed * F_Time.DeltaTime, 0f));

        if (LookTarget.Resolve(Entity.Session) is { } target)
            LookAt(target.Position);

        if (F_Input.KeyDown(F_Key.Space) && TryGetComponent<RigidBody>(out var body))
        {
            body.AddImpulse(Vector3.UnitY * 5f);
            F_Log.Info($"Jump at {Entity.Position}");
        }
    }
}
```

## 快速编译与启动

### 环境要求

- **操作系统**：Windows 10 / 11 x64（支持 DirectX 12 的显卡；硬件光追需 DXR 1.1 支持）
- **原生工具链**：Visual Studio 2022（「使用 C++ 的桌面开发」工作负载、Windows SDK、CMake 3.25+、Ninja）
- **托管工具链**：[.NET SDK 10.0.402](global.json)（预览本地文档站另需 Node.js 22+）

### 构建并启动编辑器

在仓库根目录打开 PowerShell 依次执行：

```powershell
. .\Scripts\Import-MsvcEnvironment.ps1
Import-EngineMsvcEnvironment
cmake --preset windows-x64-release
cmake --build --preset windows-x64-release
dotnet build .\Source\FunctionLayer\Scripting\Bootstrap\Bootstrap.csproj -c Release
.\Scripts\Start-Editor.ps1
```

需要生成可独立分发的便携版编辑器包（输出至 `out/packages/FlareEngine`）：

```powershell
.\Scripts\Build-EditorPackage.ps1 -Configuration Release -Name FlareEngine
```

本地预览开发者与使用者双手册（VitePress）：

```powershell
npm.cmd --prefix Manual ci
npm.cmd --prefix Manual run dev
```

## 仓库目录导览

| 目录 | 职责说明 |
| :--- | :--- |
| [Source/PlatformLayer](Source/PlatformLayer) | Win32 窗口与输入、XInput 手柄、XAudio2 音频后端、D3D12 RHI / DXR 1.1 光追 / Mesh Shader / 交换链实现。 |
| [Source/CoreLayer](Source/CoreLayer) | 数学库、任务队列、非移动追踪对象堆、双语本地化、哈希与诊断日志。 |
| [Source/ResourceLayer](Source/ResourceLayer) | FBX / glTF / GLB 模型与骨骼导入、HDR 环境烘焙、BC 纹理解码、音频容器、DirectWrite 字体图集与流式驻留。 |
| [Source/FunctionLayer](Source/FunctionLayer) | 世界实体与预制体、[前向与帧图渲染器](Source/FunctionLayer/Rendering)、物理与角色控制、导航寻路、粒子、空间音频、自研 UI 单栈及 [.NET 10 C# 脚本宿主](Source/FunctionLayer/Scripting)。 |
| [Source/ToolLayer](Source/ToolLayer) | `FlareEditor` 黑曜金编辑器外壳、项目启动器、零重启脚本热重载、导航/探针/着色器离线烘焙工具与 H.264 视频编码器。 |
| [Resources](Resources) | HLSL 着色器源码、项目模板（`Basic3D`、`RenderingStudio`、`IndoorGallery` 等）、中英双语词表与品牌图标。 |
| [BuildConfig](BuildConfig) | CMake 模块化目标配置、着色器离线编译规则、.NET 属性与 NuGet 配置。 |
| [Scripts](Scripts) | MSVC 环境导入、编辑器启动、正式包构建、游戏独立导出与分层/本地化守卫脚本。 |
| [ThirdParty](ThirdParty) | 随源码分发的第三方依赖及其许可证。 |
| [Manual](Manual) | VitePress 双手册源码：[在线开发者文档](https://xuu-xuu-xuu.github.io/FlareEngine/) · [在线使用者文档](https://xuu-xuu-xuu.github.io/FlareEngine/user/) |
