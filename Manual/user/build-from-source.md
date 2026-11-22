# 从源码启动编辑器

本页带你从 Flare 源码仓库编译出 Release 版 `FlareEditor.exe` 并打开启动器。编译完成后，下次就可以直接双击生成的 `FlareEditor.exe` 使用，无需反复敲命令。

## 准备开发环境

请先在 Windows x64 电脑上装好以下两套工具：

1. **Visual Studio 2022**：在 Visual Studio Installer 中勾选 **「使用 C++ 的桌面开发」** 工作负载（自带 MSVC 编译器、Windows SDK、CMake 3.25+ 与 Ninja）。
2. **.NET SDK `10.0.402`**：项目根目录 `global.json` 固定了该版本用于编译脚本与托管宿主。

::: tip 如何确认 .NET 版本已装好？
打开任意 PowerShell 窗口运行 `dotnet --list-sdks`，列表里能看到 `10.0.402` 即可继续。
:::

## 五条命令编译并启动

在**源码仓库根目录**打开 PowerShell，按顺序逐条执行下面命令：

```powershell
# 1. 把 MSVC x64 编译环境加载到当前终端
. .\Scripts\Import-MsvcEnvironment.ps1
Import-EngineMsvcEnvironment

# 2. 配置并编译 Release 版原生引擎与编辑器
cmake --preset windows-x64-release
cmake --build --preset windows-x64-release

# 3. 编译托管脚本启动器
dotnet build .\Source\FunctionLayer\Scripting\Bootstrap\Bootstrap.csproj -c Release

# 4. 启动 Flare 编辑器启动器
.\Scripts\Start-Editor.ps1
```

## 看到什么算成功

命令执行完成后会弹出 **Flare 引擎** 启动器窗口：

- 首次使用可以点 **新建项目**（或 **打开示例**）开始创作，具体步骤见 [新建项目](create-project.md)。
- 如果你已经有 Flare 项目，点 **打开项目…** 选择项目目录里的 `project.json`。
- 编译好的可执行文件位于 `out/build/windows-x64-release/FlareEditor.exe`，之后也可以直接双击它启动。

::: info 启动脚本常用参数
`.\Scripts\Start-Editor.ps1` 还支持直接指定打开项目、切换语言或进入安全模式（跳过上次会话恢复）：

```powershell
# 直接打开指定项目
.\Scripts\Start-Editor.ps1 -Project "D:\Flare_Project\MyFirstGame"

# 切换英文界面，或在异常退出后以安全模式启动
.\Scripts\Start-Editor.ps1 -Language en-US -SafeMode
```
:::

::: warning 常见编译/启动排查
- **提示找不到 `cl` 或 `ninja`**：请确认第 1 步的两行命令已在当前 PowerShell 窗口执行成功，且 Visual Studio 已安装 C++ 桌面开发组件。
- **显卡不支持或创建 D3D12 设备失败**：Flare 默认优先选用支持 Shader Model 6+ 的非 UMA 独立硬件显卡，请先更新显卡驱动。
:::
