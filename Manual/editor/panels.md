# 面板模型、默认布局与界面合成

## 默认 2×3 布局与共享选择状态

编辑器默认采用符合直觉的 2×3 停靠布局：左侧上下分置 **场景视口**（与 **UI 设计** 同组）和 **摄像机视口**（与 **控制台** 同组），中部上下分置 **场景层级** 与 **资源**，右侧通栏为 **属性**。`EditorContext` 将当前项目、文档、选中实体/资产与运行预览状态同步给各面板；`EditorHierarchyModel` 按树形缩进与类型徽标展开实体行，支持直接将实体拖入属性引用槽或拖入资源面板生成 `.prefab.json`。新场景由 `CreateDefaultScene()` 创建默认主相机、方向光、平面与立方体。

## 摄像机视口与控制台面板

- **摄像机视口**：顶栏提供 **窗口预览** 与 **全屏预览** 模式切换（`previewFullscreen`）。点击运行预览时，游戏画面直接在摄像机视口运行（或按全屏模式铺满编辑区，按 `Esc` 释放捕获/退出全屏预览），不会劫持编辑用场景视口镜头。
- **控制台面板**：顶栏集成 **清空**、**折叠重复**、**运行清空** 开关以及右侧 **信息 / 警告 / 错误** 实时计数过滤按钮；同时接收引擎诊断、异步 `dotnet build` 编译结果与 C# `F_Log` 输出，双击含 `File.cs(line,col)` 的日志行可直接调用外部代码编辑器定位到对应行列。

## 属性代理层、五层 GameObject 契约与双向视口闭环

`EditorProperty` 作为面板 UI 与底层 `World` / 外部资产文档之间的序列化代理与语义适配层：

- **实体边界与共享资产隔离**：`SceneInspector` 将选中实体（`id != 0`）严格限定为实体头（`Active`、`Name`、`#StableId`、`Tag`、`Layer 0..31`、`EntityStaticFlags` 位掩码）、预制体实例顶栏（`◆ Prefab [定位/还原/应用]`，支持单属性与单组件级覆盖还原/回写）、空间锚点 `Transform`（`Local | World` 坐标分解与 `CachedEulerDegrees` 连续欧拉角缓存）、按槽位顺序排列的组件栈以及底部默认折叠的只读 `Runtime & Derived Diagnostics`；全局 `SceneEnvironment` 与 `TOD` 仅在 `id == 0` 时展示。检视 `.material.json` 时统计场景引用计数并提供一键克隆独立材质（`asset.mat.duplicateAssign`）。
- **组件栈原语、依赖守卫与内联自愈**：全部内置与扩展组件及 C# 脚本统一支持启停复选框、`Reset` 语义重置、`MoveUp/MoveDown` 持久排序、`Copy/Paste Values/Paste As New` 组件剪贴板以及 `⌖` 空间手柄聚焦；`add.actor` 自动补齐 `Mesh`，`remove.mesh` 在存在 `SkeletalActor` 时前置拦截；组件参数携带物理单位（`unit`）、色彩空间（`colorSpace`）、条件显隐与内联告警 + `fix.*` 一键修复按钮。
- **跨面板桥接、事务合并与运行期保留**：属性页签提供 `🔒/🔓` 检视锁定（`SetInspectorLocked`），引用槽集成点击 `Ping` 高亮闪烁与 `⊙` 作用域搜索弹窗（`OpenScopedPickerPopup`）；运行预览支持按实体勾选 `play.keepEntity`，在 `StopPlay` 时捕获运行态组件与脚本字段并合并为单条撤销写回编辑世界。
- **全局多选、差异比对与视口双向手柄**：`EditorSession::Selection()` 与 `AppendMultiSceneInspector` 自动提取共有组件并将差异字段标记为 `mixedValue`（`—`）；连续拖动滑块、标签 Scrub、拾色器或 3D 视口组件控制柄仅在松手时生成 1 条撤销记录，`Esc` 完整回滚。

## 资源面板与统一保存

`EditorAssetsModel` 支持多级目录树浏览、面包屑导航、`+` 新建文件夹、关键词即时过滤，并为场景、材质、模型、纹理、预制体、音频、导航网格与 C# 脚本绘制专属彩色类型徽标。右上角布局菜单除内置预设外，支持将自定义分屏树持久化至 `.editor-ui.custom.json`。

`EditorChrome` 组织顶部菜单、播放控制与状态栏，界面配色统一取自黑曜金 `FlarePalette`。按下 `Ctrl+S`（`SaveAll`）原子保存当前场景、C# 脚本绑定（`.bindings.json`）以及材质/UI 等 `EditorExternalDocuments` 外部文档；崩溃恢复快照由 `EditorSessionStore::WriteRecoveryGroup()` 维护。
