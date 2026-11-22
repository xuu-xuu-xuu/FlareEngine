# 资产导入服务（FBX / glTF / 贴图 / 音频）

本文说明外部资源如何安全进入项目的 `Content/` 目录并注册到 [`AssetStore`](models.md#assetstore)。Flare 采用**两阶段事务导入**：先在只读阶段展开完整依赖闭包并校验合法性与冲突，确认后再原子复制、写入 `.asset.json` 并重新扫描索引；任何一步失败都会自动回滚本次新建的文件和空目录。

## 完整导入数据流

```text
编辑器拖拽文件 / 导入对话框 (EditorToolkitSession.cpp)
       │
       ▼
  AssetImportService::Plan(AssetImportRequest)
  ├─ 校验扩展名与目录沙箱 (asset_io::Within)
  ├─ 模型试解析 (ImportStaticModel) ──▶ 展开外部贴图/缓冲依赖闭包
  ├─ 预解码校验图片/音频 + 计算源文件 SHA-256 指纹
  ├─ 项目内源原地登记 (copyRequired=false)，项目外源规划复制目标
  └─ 跨输入组目标冲突检测（不同源争抢同一路径时双双拒绝）
       │
       ▼  生成 AssetImportPlan (valid / files / conflicts / diagnostics)
  用户确认导入 ──▶ 后台线程执行 AssetImportService::ExecuteRegistered(plan, assets, imported)
  ├─ 二次校验计划合法性与源 SHA-256（防确认期间文件被篡改）
  ├─ ImportTransaction 原子写入目标文件 (WriteAtomic, replaceExisting=false 绝不覆盖)
  ├─ 对拷贝到目标位置的模型再次试导入，确认相对依赖完整解析
  ├─ 为主输入与图像生成 32 位 AssetId 与 .asset.json sidecar，调用 AssetStore::RegisterSource()
  └─ AssetStore::Scan() 刷新索引 ──▶ 返回 imported AssetId 列表
       │
  任意步骤失败 ──▶ ImportTransaction::Rollback() 仅清理本次创建文件与空目录 + 恢复旧 AssetStore 快照
```

编辑器调用入口位于 [`OpenToolkitImportConfirm()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorToolkitSession.cpp#L376) 与 [`BrowseAndImportToolkitAsset()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorToolkitSession.cpp#L411)。

## 支持的输入格式

[`AssetImportService::SupportedExtension()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/AssetImportService.cpp#L301) 决定哪些主输入文件可以被接受：

| 资产类别 | 支持扩展名 | 是否登记 `.asset.json` | 预检动作 |
| :--- | :--- | :---: | :--- |
| 3D 模型 | `.fbx`、`.gltf`、`.glb` | 是 | 调用 `ImportStaticModel()` 验证并收集贴图/缓冲依赖 |
| 纹理贴图 | `.png`、`.jpg`、`.jpeg` | 是（含模型附带贴图） | 调用 `DecodeImage()` 预解码验证 |
| 音频剪辑 | `.wav`、`.ogg` | 是 | 调用 `DecodeAudioSource()` 预解码验证 |
| 材质 / 预制体 | `.material.json`、`.prefab.json` | 是 | 校验路径沙箱与 SHA-256 |
| 用户着色器产物 | `.sebin` | 是 | 校验路径沙箱与 SHA-256（容器内容由渲染器验证） |
| 模型外部二进制缓冲 | 如 glTF 附带 `.bin` | 否（`registerAsset=false`） | 随主模型一并复制到相对目录，不生成多余 sidecar |

## AssetImportService

[`flare::AssetImportService`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/AssetImportService.h#L35) 是无状态静态服务，核心请求与计划数据结构定义在 [`AssetImportService.h#L10-L33`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/AssetImportService.h#L10)：

```cpp
// Source/ToolLayer/Editor/AssetImportService.h#L10-L33
struct AssetImportRequest
{
    std::vector<std::filesystem::path> sources;
    std::filesystem::path contentRoot;
    std::filesystem::path targetDirectory; // 相对 contentRoot 的目录，空表示根。
};

struct AssetImportFile
{
    std::filesystem::path source, target;
    bool primary = false;         // 用户选择的主输入；共享依赖只保留一份。
    bool copyRequired = true;     // 项目内重导入只登记，不移动原文件。
    bool registerAsset = false;   // 主输入和可登记图像；缓冲依赖不生成 sidecar。
    std::string sourceDigest;     // 计划批准时的 SHA-256，执行时重新验证。
    std::string sidecarDigest;    // 仅项目内已有 sidecar；空表示目标不得存在 sidecar。
};

struct AssetImportPlan
{
    bool valid = false;
    std::filesystem::path contentRoot;
    std::vector<AssetImportFile> files;
    std::vector<std::string> conflicts, unsupported;
    std::vector<AssetDiagnostic> diagnostics; // 每个被拒绝的主输入组及其原因。
    std::string errorKey, detail;
};
```

### Plan

[`AssetImportPlan Plan(const AssetImportRequest& request)`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/AssetImportService.cpp#L307) 逐组规划输入文件，不修改磁盘上的任何内容：

1. **去重与原地重导入**：对 `request.sources` 规范化路径去重；如果源文件已经位于 `contentRoot` 内，直接原地登记（`copyRequired = false`），忽略 `targetDirectory`。
2. **依赖闭包解析**：对模型调用 `ImportStaticModel()` 并要求所有依赖满足 `allowedRoot = source.parent_path()`，防止外部相对路径（如 `../../secret.png`）越界逃逸。
3. **公平冲突仲裁**：若批量选择的两个不同源文件会写入同一个目标路径（或同一 `.asset.json`），`Plan()` 会同时拒绝这两个输入组，不让列表顺序决定谁覆盖谁（[`AssetImportService.cpp#L328-L350`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/AssetImportService.cpp#L328)）。

::: tip 批量导入排查要点
只要至少有一个输入组通过校验，`plan.valid` 即为 `true`，但其余被拒绝的组仍会写入 `plan.conflicts`、`plan.unsupported` 与 `plan.diagnostics`。遇到“选了 5 个文件，为什么只进来了 3 个”时，应同时检查 `valid` 和 `diagnostics`。
:::

### ExecuteRegistered

[`bool ExecuteRegistered(const AssetImportPlan& plan, AssetStore& assets, std::vector<AssetId>& imported, std::string& errorKey, std::string& detail)`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/AssetImportService.cpp#L385) 是编辑器正式使用的事务提交入口（底层仅复制不登记版本为 [`Execute()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/AssetImportService.cpp#L371)）：

- **绝不静默覆盖**：[`ImportTransaction::Add()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/AssetImportService.cpp#L208) 调用 `asset_io::WriteAtomic(path, bytes, /*replaceExisting=*/false)`，若目标在规划后突然出现同名文件，立刻失败并回滚。
- **落盘后再验证**：文件复制完成后，`VerifyCopied()` 会对每个主模型在**最终目标路径**下再次执行 `ImportStaticModel()`，确认复制后的相对贴图与 `.bin` 依赖真实完整。
- **精确回滚**：回滚时逐个核对文件 SHA-256 与本次写入记录一致后才删除，并且只删除仍为空的自建目录（[`ImportTransaction::Rollback()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/AssetImportService.cpp#L216)）；最后若 `assets.Scan()` 失败，直接恢复事务前的 `AssetStore` 内存快照，确保绝不残留脏 ID。

## 常见导入诊断键速查

| 错误键 | 常见原因 | 排查方向 |
| :--- | :--- | :--- |
| `Assets.ImportNoFiles` / `Assets.ImportNothingSupported` | 未传文件，或所有输入扩展名不受支持/均被拒绝 | 检查 `plan.diagnostics` 与 `SupportedExtension()` 列表 |
| `Assets.ImportConflict` | 目标目录已存在同名文件或 `.asset.json`，或同批次多文件撞名 | 更换 `targetDirectory` 或重命名源文件 |
| `Assets.ImportDependencyMissing` | FBX / glTF 引用的外部贴图或 `.bin` 缓冲不存在 | 确保贴图与模型处于同级或子目录且未缺失 |
| `Assets.ReferenceOutsideRoot` | 模型内部引用了 `..` 越出源目录的路径，或目标超出 `contentRoot` | 重新导出模型并改为同目录相对贴图路径或嵌入贴图（GLB） |
| `Assets.ImportSourceChanged` | 确认导入期间源文件或已有 sidecar 被外部程序改写 | 重新触发导入以重新计算 SHA-256 指纹 |
| `Assets.ImportInvalidSource` | 模型、图像或音频解码失败（损坏文件或不支持的特性） | 查看 `detail` 尾部附带的底层解码器错误键 |
