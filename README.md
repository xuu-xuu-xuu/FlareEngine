<p align="center">
  <img src="Resources/Branding/FlareMark-README.png" alt="Flare Engine 图标" width="160">
</p>

<h1 align="center">Flare Engine</h1>

<p align="center">
  基于 <strong>C++20</strong>、<strong>DirectX 12</strong> 与 <strong>.NET 10</strong> 构建的现代自研 3D 游戏引擎
</p>

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-20-1f2328?style=flat-square&logo=cplusplus&logoColor=white" alt="C++20">
  <img src="https://img.shields.io/badge/DirectX-12%20(SM6.5%20%2F%20DXR%201.1)-1f2328?style=flat-square" alt="DirectX 12">
  <img src="https://img.shields.io/badge/.NET-10%20C%23-1f2328?style=flat-square&logo=dotnet&logoColor=white" alt=".NET 10">
  <img src="https://img.shields.io/badge/Platform-Windows%20x64-1f2328?style=flat-square" alt="Windows x64">
</p>

<p align="center">
  <a href="https://xuu-xuu-xuu.github.io/FlareEngine/"><strong>开发者文档</strong></a> ·
  <a href="https://xuu-xuu-xuu.github.io/FlareEngine/user/"><strong>使用者文档</strong></a> ·
  <a href="#核心特性"><strong>核心特性</strong></a> ·
  <a href="#c-脚本速览"><strong>C# 示例</strong></a> ·
  <a href="#自定义-shader-与-ase-连线"><strong>Shader / ASE</strong></a> ·
  <a href="#快速开始"><strong>快速开始</strong></a>
</p>

---

**Flare Engine** 是一款面向 Windows 的现代 3D 游戏引擎与一体化创作环境。引擎底层采用 C++20 与 DirectX 12 实现，编辑器基于零第三方 IMGUI 依赖的自研 UI 单栈构建，原生支持 .NET 10 C# 脚本、Unity 风格多 Pass 表面着色器（`.fshader.hlsl`）与 138 节点 ASE 可视化连线图（`.fshader.json`）的零重启热重载——覆盖从场景搭建、材质特效创作、实时预览到独立游戏包导出的完整开发流程。

## 文档入口

| 文档 | 面向读者 | 内容概览 |
| :--- | :--- | :--- |
| [**开发者文档**](https://xuu-xuu-xuu.github.io/FlareEngine/) | 引擎与图形开发者 | 分层架构、DirectX 12 / 光追 / Mesh Shader / 多 Pass 着色管线、C ABI 桥接与编码规范 |
| [**使用者文档**](https://xuu-xuu-xuu.github.io/FlareEngine/user/) | 游戏创作者 | 项目创建、编辑器操作、资产导入、C# 脚本、自定义 Shader / ASE 连线与独立游戏导出 |

## 核心特性

- **DirectX 12 现代渲染**：多散射 PBR、PSSM + 硬件双线性 Vogel 软阴影、Forward+ 局部光、GTAO / SSGI / SSR / 体积雾、SM 6.5 Mesh Shader，以及带 SVGF 降噪与自动光栅回退的 DXR 1.1 混合硬件光追。
- **Unity 风格多 Pass 着色器与 ASE 连线图**：提供简明直观的 `.fshader.hlsl` 表面着色器（支持最多 8 Pass、卡通背面外扩描边、发光叠加、隔墙 X-Ray 透视与切回窗口零重启热编译）及内置 9 大类 138 节点的 `.fshader.json` 可视化连线面板，主视口模型与动态光源预览球双路实时联动。
- **自研一体化编辑器**：零第三方 IMGUI 依赖，支持多窗口停靠与自由布局、Per-Monitor DPI、多选编辑与预制体覆写、3D 视口控制柄、运行期热调参及原生中英文界面。
- **C# 零重启热重载**：基于 .NET 10 与统一 `F_` 游戏 API，自动同步外部 IDE 解决方案（`.sln`）及中文智能提示，保存脚本即原位热重载，无需重启编辑器。
- **完整运行时与发行工具**：刚体物理与胶囊角色控制器、NavMesh 寻路、glTF / FBX 骨骼动画与 GPU 蒙皮、XAudio2 3D 音频、粒子、昼夜大气、独立游戏包导出与固定步 MP4 录制。

## C# 脚本速览

下面是一个控制角色移动、朝向与跳跃的 C# 示例脚本——在编辑器点击 **+ 添加组件** 新建并在外部 IDE 保存后，切回 Flare 窗口即可自动编译生效（完整语法拆解见 [C# 脚本教程](https://xuu-xuu-xuu.github.io/FlareEngine/user/scripting.html)）：

```csharp
using System.Numerics;
using FlareEngine;

public class PlayerMover : F_Behaviour
{
    [F_Header("移动参数")]
    [F_Expose, F_Range(1f, 20f)] public float MoveSpeed = 6f;
    [F_Expose] public F_Entity? LookTarget;

    public override void F_Update(float delta)
    {
        var dir = new Vector3(F_Input.Horizontal, 0f, F_Input.Vertical);
        Translate(dir * (MoveSpeed * F_Time.DeltaTime), localSpace: false);

        if (LookTarget is { IsValid: true })
            LookAt(LookTarget.WorldPosition);

        if (F_Input.KeyDown(F_Key.Space) && TryGetComponent<F_Rigidbody>(out var body))
        {
            body.AddImpulse(Vector3.UnitY * 5f);
            F_Log.Info($"Jump at {Entity.WorldPosition}");
        }
    }
}
```

## 自定义 Shader 与 ASE 连线

无需手写底层根签名或阴影采样——既可在编辑器中使用 **138 节点 ASE 可视化连线面板**（`.fshader.json`）实时连线预览，也可直接编写清爽的 Unity 风格单/多 Pass `.fshader.hlsl` 着色器，保存切回窗口即零重启热编译并自动生成材质滑块（完整示例见 [自定义 Shader 与 ASE 连线教程](https://xuu-xuu-xuu.github.io/FlareEngine/user/custom-shader.html)）：

```hlsl
Shader "FX/HologramRim"
{
    Properties
    {
        _RimColor ("边缘发光色", Color)          = (0.15, 0.80, 1.00, 1.0)
        _RimPower ("菲涅尔锐度", Range(0.5, 8.0)) = 3.0
    }

    SubShader
    {
        Shading Lit
        Blend   Transparent
        Cull    Off

        HLSLPROGRAM
        void surf(Input IN, inout SurfaceOutput o)
        {
            float rim  = pow(1.0 - saturate(dot(normalize(IN.worldNormal), normalize(IN.viewDir))), _RimPower);
            o.Albedo   = tex2D(_MainTex, IN.uv).rgb * 0.25;
            o.Emission = _RimColor.rgb * rim * (1.5 + 0.5 * sin(_Time.y * 4.0));
            o.Alpha    = saturate(0.25 + rim * 0.85);
        }
        ENDHLSL
    }
}
```

## 快速开始

### 直接下载安装（推荐）

前往 [**GitHub Releases**](https://github.com/xuu-xuu-xuu/FlareEngine/releases) 下载最新的 **`FlareEngine-Setup-v0.1.0-x64.exe`** 双击即可一键向导安装（自动创建桌面/开始菜单快捷方式并注册 Windows 应用卸载项，也可下载免安装便携版 `.zip`）。

### 从源码构建环境要求

- **系统与显卡**：Windows 10 / 11 x64，支持 DirectX 12（硬件光追需 DXR 1.1）
- **C++ 工具链**：Visual Studio 2022（「使用 C++ 的桌面开发」）、CMake 3.25+、Ninja
- **.NET 工具链**：[.NET SDK 10.0.402](global.json)（本地预览文档站另需 Node.js 22+）

### 从源码编译并启动

在仓库根目录打开 PowerShell 依次执行：

```powershell
. .\Scripts\Import-MsvcEnvironment.ps1
Import-EngineMsvcEnvironment
cmake --preset windows-x64-release
cmake --build --preset windows-x64-release
dotnet build .\Source\FunctionLayer\Scripting\Bootstrap\Bootstrap.csproj -c Release
.\Scripts\Start-Editor.ps1
```

打包独立便携版编辑器与一键安装程序（输出至 `out/packages/FlareEngine` 与 `out/release`）：

```powershell
.\Scripts\Build-EditorPackage.ps1 -Configuration Release -Name FlareEngine
.\Scripts\Build-EditorInstaller.ps1 -Configuration Release -IncludePortableZip
```

本地预览开发者与使用者文档站：

```powershell
npm.cmd --prefix Manual ci
npm.cmd --prefix Manual run dev
```

## 目录结构

| 目录 | 说明 |
| :--- | :--- |
| [Source/CoreLayer](Source/CoreLayer) | 数学库、任务队列、双语本地化、哈希与诊断日志 |
| [Source/PlatformLayer](Source/PlatformLayer) | Win32 窗口输入、XAudio2 音频、DirectX 12 RHI / DXR 1.1 / Mesh Shader |
| [Source/ResourceLayer](Source/ResourceLayer) | FBX / glTF 资产与骨骼导入、多 Pass 表面着色器与 138 节点 ASE 图编译、HDR 烘焙、纹理与字体 |
| [Source/FunctionLayer](Source/FunctionLayer) | 世界与预制体、渲染管线、物理、导航、粒子、自研 UI 与 C# 脚本宿主 |
| [Source/ToolLayer](Source/ToolLayer) | `FlareEditor` 编辑器（含 ASE 着色器连线画布）、项目启动器、离线烘焙工具与 MP4 视频编码器 |
| [Resources](Resources) · [Scripts](Scripts) · [Manual](Manual) | HLSL 着色器、项目模板、构建/导出脚本与 VitePress 双手册源码 |
