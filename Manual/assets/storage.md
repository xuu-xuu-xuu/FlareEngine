# 文件解码、路径引用安全与派生缓存

本文聚焦资产系统的底层基础设施：模型/图像/环境贴图在 CPU 端如何解码、路径引用如何防范目录逃逸、磁盘落盘怎样做到断电/并发安全，以及属性值 JSON 序列化与场景原子提交机制。

## I/O、解码与派生缓存分层图

```text
磁盘源文件 (.fbx / .gltf / .glb / .png / .hdr / .scene.json)
       │
       ▼  flare::asset_io (AssetIO.h / AssetIO.cpp)
  ├─ Read(path, bytes[, stop_token])   (上限 256 MB；支持 OVERLAPPED 可取消异步读)
  ├─ ResolveReference / ResolveRelative (URI 百分号解码 + Within() 根目录沙箱校验)
  ├─ NewId() / Hash()                   (BCryptGenRandom 128-bit ID + SHA-256)
  └─ WriteAtomic(path, bytes, replace)  (.tmp-<id> + FlushFileBuffers + MoveFileExW)
       │
       ▼  CPU 解码与离线烘焙
  ├─ ImportFbx (ufbx-0.17.1) / ImportGltf (cgltf-1.15) ──▶ StaticModel
  ├─ DecodeImage (WIC, 上限 256 MB) / PrepareModelImages ──▶ RGBA8 + ImageContentKey
  ├─ CompressBc1/Bc4/Bc5MipChain / CompressBc7         ──▶ .sebc 块压缩缓存
  └─ BakeEnvironment / SaveEnvironment / CookEnvironment ──▶ IBL (Irradiance + Radiance + BRDF LUT)
       │
       ▼  持久化与运行态提交
  ├─ WritePropertyJson / ReadPropertyJson (类型化属性 ↔ JSON)
  └─ World::CommitScene()                 (预留容量 + 冲突检查 → 零分配原子切换场景)
```

## 导入器、路径安全与原子写入

### 统一模型导入（ufbx + cgltf）

[`ImportStaticModel()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/ModelImporter.cpp#L1205) 根据扩展名分派两条解码路径，输出完全同构的 `StaticModel`（保留节点父子层级、网格、PBR 材质、贴图引用及 glTF 骨骼动画）：

- **FBX**：[`ImportFbx()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/ModelImporter.cpp#L1089) 使用 `ufbx-0.17.1` 解码并归一坐标与单位到右手 Y-up 米制。
- **glTF / GLB**：[`ImportGltf()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/ModelImporter.cpp#L767) 使用 `cgltf-1.15` 读取独立 `.gltf`（含外部 `.bin` / 贴图）或自包含 `.glb`。

### `asset_io` 安全读、路径沙箱与原子落盘

[`flare::asset_io`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/AssetIO.h#L7) 强制所有资产读写遵守三条安全底线（文件读取硬上限 `MaxFileBytes = 256 MiB`）：

1. **可取消异步文件读**：[`asset_io::Read(path, bytes, std::stop_token)`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/AssetIO.cpp#L12) 使用 Win32 `FILE_FLAG_OVERLAPPED` 逐 1 MiB 块读取，收到停止信号时调用 `CancelIoEx` 并等待内核确认 `GetOverlappedResult(..., TRUE)` 后再释放缓冲区，避免悬空内核 DMA 写。
2. **禁止路径穿越**：[`asset_io::ResolveReference()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/AssetIO.cpp#L136) 先对 glTF URI 做 `%XX` 解码并拒绝 `\0` 和 `:`（盘符/协议头），再通过 [`ResolveRelative()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/AssetIO.cpp#L161) 与 [`Within()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/AssetIO.cpp#L59) 校验 `weakly_canonical` 后的路径严格落在 `allowedRoot` 之内。
3. **原子落盘**：[`asset_io::WriteAtomic()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/AssetIO.cpp#L106) 先写同目录随机后缀临时文件 `.tmp-<id>`，调用 `FlushFileBuffers` 落盘后再用 `MoveFileExW(..., MOVEFILE_WRITE_THROUGH)` 原子替换（`replaceExisting = false` 时目标已存在会直接失败，用于导入事务防覆盖）。

```cpp
// Source/ResourceLayer/Assets/AssetIO.cpp#L113-L133 (核心流程)
auto temporary = path;
temporary += ".tmp-" + id;
HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
// ...分块 WriteFile...
if (succeeded && !FlushFileBuffers(file)) succeeded = false;
if (!CloseHandle(file)) succeeded = false;
if (succeeded)
    succeeded = MoveFileExW(temporary.c_str(), path.c_str(),
        MOVEFILE_WRITE_THROUGH | (replaceExisting ? MOVEFILE_REPLACE_EXISTING : 0)) != FALSE;
if (!succeeded) DeleteFileW(temporary.c_str());
```

## 解码工作与派生缓存

### 图像解码与异步任务

- [`DecodeImage()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/AssetStore.h#L37) 支持文件路径或内存切片（嵌入图片），输出线性字节流 RGBA8（最大 256 MiB 预算），**不擅自假定颜色空间**（是否按 sRGB 采样由渲染材质槽决定，避免破坏法线/粗糙度贴图）。
- [`PrepareModelImages()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/AssetTasks.cpp#L7) 批量解码模型材质引用的全部贴图，严格扣减共享 `maximumRgbaBytes` 预算，并为每张可用图像计算 `ImageContentKey`；缺图仅标记 `unavailable[i] = true`，由渲染端按[默认纹理回退](models.md#staticmodel)处理。
- 后台任务辅助函数 [`ImportModelAsync()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/AssetTasks.cpp#L40) 与 [`DecodeImageAsync()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/AssetTasks.cpp#L54) 投递到 `TaskQueue`，完成回调仅在所有者线程 `queue.Pump()` 中执行。

### 模型与贴图派生缓存键

- **模型缓存**：`AssetStore` 计算派生缓存键时，会把 `StaticModelImporterVersion`（含解析器与材质扩展版本，见 [`StaticModel.h#L137`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/StaticModel.h#L137)）、`ImportOptions`（`scale` / `generateTangents`）、主文件 SHA-256 及所有外部依赖的 SHA-256（缺失记为 `"missing"`）拼接后求总 SHA-256（[`AssetStore.cpp#L52-L75`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/AssetStore.cpp#L52)）。只要源码或任一贴图/缓冲变化，缓存自动失效重建。
- **纹理压缩链**：[`CompressBc1MipChain()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/TextureCompression.cpp#L136)、[`CompressBc5MipChain()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/TextureCompression.cpp#L189)、[`CompressBc4MipChain()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/TextureCompression.cpp#L272) 与 [`CompressBc7()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/Bc7Compression.cpp#L141) 负责把 RGBA8 压块并交给 `TextureBakeCache` 存盘。

### IBL 环境资产

[`EnvironmentAsset`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/Environment.h#L20) 保存离线烘焙的三张半精度浮点（RGBA binary16）IBL 纹理：`irradiance` 立方体、`prefiltered radiance`（含多级 mip）与 `BRDF LUT`：
- [`CookEnvironment()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/Environment.h#L36) 结合 `sourceHash` 与 `EnvironmentSettings` 生成 `cookKey`，命中已有资产时直接返回 `cacheHit = true`。
- [`SaveEnvironment()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/Environment.h#L33) 写出带 `format: "FlareEngine.Environment"` 的容器，[`DecodeEnvironment()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/Environment.h#L34) 在读回时严格校验版本、尺寸和字节长度。

## 属性值持久化与场景原子提交

### `WritePropertyJson` / `ReadPropertyJson`

[`WritePropertyJson()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/PropertyJson.h#L10) 与 [`ReadPropertyJson()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/PropertyJson.h#L20) 负责所有原生反射组件与材质属性在强类型 `PropertyValue` 与 JSON 之间的无损往返：

| `PropertyType` | JSON 表示 | 校验规则 |
| :--- | :--- | :--- |
| `Boolean` / `Number` | `true/false` / 数值 | 数值必须有限且不超出 `float` 范围，并通过 `PropertyDescriptor` 上下界校验 |
| `Vector3` / `Vector4` / `Quaternion` | `[x, y, z]` / `[x, y, z, w]` | 严格检查数组长度与各分量有限性 |
| `Text` / `AssetId` / `AssetList` | 字符串 / 字符串数组 | `AssetList` 上限 1,024 项 |
| `EntityReference` | 64 位无符号十进制字符串或 `null` | 使用 `std::from_chars` 精确解析，杜绝 JSON 浮点精度丢失 |

### `World::CommitScene()`

场景在后台解析并实例化到临时暂存世界 `candidate`（`staging_ == true`）后，在主线程安全点通过 [`World::CommitScene()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/WorldSceneCommit.cpp#L7) 一次性提交进活动世界：

1. **只读预检**：拒绝跨场景父子引用（`Scene.CrossSceneParent`）、`StableEntityId` 冲突（`Scene.StableIdConflict`）与脚本控制键重复。
2. **先分配再切换**：预先为目标 `slots_`、`componentSlots_` 及全部组件池 `ReserveAdditional()`；如果中途内存不足抛出异常，原世界毫发无损。
3. **零分配移交**：调用 `beforeRetire()` 通知物理/脚本/粒子/导航清理退役实体，按子先父后深度顺序释放旧组件，再通过 `TransferMapped()` 零分配把新实体与组件槽位并入活动世界。

::: tip 延伸阅读
- 外部文件进入项目的复制与登记事务见[资产导入服务](import.md)。
- 异步加载状态机、分帧 GPU 上传与纹理显存预算见[异步加载、上传与纹理驻留](loading.md)。
:::
