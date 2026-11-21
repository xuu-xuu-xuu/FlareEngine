# 资产操作、卡片式属性面板与项目文档

## 重命名、移动和依赖

[导入服务](../assets/import.md#assetimportservice) 把外部模型、贴图与音频带入项目。`EditorAssetActions` 支持新建目录，并在移动或删除资产前生成引用影响计划，扫描场景、材质、预制体、UI 和脚本绑定引用；`Execute()` 原子更新磁盘文件与项目清单，`ValidatePlayAssets()` 在进入运行预览前校验依赖完整性。

## 卡片式 Inspector、搜索添加组件与跨面板拖拽

`EditorInspector` 将每个原生组件与 C# 脚本渲染为带缩进圆角背景、彩色类型徽标和折叠开关的独立卡片，标题右侧 `⋮` 更多菜单收纳「编辑 C# 脚本」与「移除组件」等操作：

- **富属性控件与算式求值**：根据反射元数据自动渲染 RGBX 轴向数值框、`[F_Header]` 分组小标题、`[F_Range(min, max)]` 填充滑条、HSV/RGB 选色浮层、可读实体槽（`◈ 名称 (#ID)` + `×` 清除）和资产槽。所有数值框支持标签 `↔` 左右拖拽精调，并由 `TryEvaluateMathExpression()` 原生解析加减乘除、括号及 `pi` 常数表达式（如 `360/8`、`2*pi`）。
- **搜索式 `+ 添加组件` 浮层**：点击底部按钮弹出即时模糊过滤列表（C# 脚本置顶）；输入尚不存在的类名回车即可调用 `EditorScriptTools::CreateBehaviour()` 生成 `.cs` 源文件，并在后台热重载完成后自动挂载到当前实体。
- **跨面板直觉拖拽**：从 **场景层级** 拖实体到 Inspector 的 `EntityReference` 槽完成对象绑定；从 **资源** 拖资产到材质/网格槽或直接拖到场景视口模型上赋材质；从 **场景层级** 拖实体回 **资源** 面板直接生成 `.prefab.json`。

专用资产方面，`MaterialEditSession` 与 `RenderMaterialThumbnail()` 负责材质编辑事务和球体预览，`UICanvasEditor` 负责画布控件框选、缩放与锚点换算。

## 项目、脚本与设置

`ProjectSettingsService` 读取和合并项目清单与画质/构建配置；`EditorSettingsDocument` 跟踪草稿摘要与脏状态。`PrepareOpenSource()` 支持按可配置的外部 IDE 命令模板打开 `.cs` 文件并精确跳转到报错或日志对应的行列号。脚本字段与特性的反射机制见[脚本反射与程序集卸载](../scripts/lifetime.md)。
