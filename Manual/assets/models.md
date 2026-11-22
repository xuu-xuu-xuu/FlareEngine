# 模型、纹理与材质资产

本文梳理 `AssetStore` 资产目录、统一中间模型结构 `StaticModel`（含骨骼与动画）、`MaterialDocument` 材质与着色器变体掩码，以及顶点 16 字节量化与 BC1/BC4/BC5/BC7 纹理块压缩管线。带骨骼的 glTF / GLB 运行时姿态采样与 GPU 蒙皮详见[骨骼动画](../runtime/skeletal-animation.md#advance)。

## 从源文件到渲染资源的准备流

```text
Content/*.fbx / .gltf / .glb / .png / .material.json + *.asset.json
       │
       ▼
  AssetStore (单线程目录 + weak_ptr 共享 CPU 缓存 + 磁盘派生缓存)
       ├── LoadModel()        ──▶ StaticModel (节点 / 网格 / 材质 / 骨骼 / 动画)
       │                             ├─ BuildStaticModelLayout() (展平节点矩阵 + AABB)
       │                             └─ CompressMeshVertices()   (16B PackedVertex)
       ├── LoadTextureImage() ──▶ ImageData (RGBA8 CPU 像素)
       │                             └─ TextureBakeCache (.sebc: BC1 / BC4 / BC5 / BC7)
       └── LoadMaterial()     ──▶ MaterialDocument (PBR + ClearCoat / Sheen / Aniso / SSS)
                                     ├─ ResolveMaterialInstance() (父材质 + 局部覆盖)
                                     └─ VariantMacros(mask)       (SE_FEATURE_* 着色器宏)
```

## AssetStore

[`flare::AssetStore`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/AssetStore.h#L43) 管理项目的内容根目录（`sourceRoot_`）与派生缓存目录（`cacheRoot_`），通过扫描 `.asset.json` 建立 `AssetId → Record` 索引，并以 `std::weak_ptr` 缓存已加载的 CPU 资产（引用归零后自动回收，调用 `CollectUnused()` 清理空槽）。

```cpp
// Source/ResourceLayer/Assets/AssetStore.h#L43-L79
class AssetStore
{
public:
    AssetStore(std::filesystem::path sourceRoot, std::filesystem::path cacheRoot);
    bool Scan(std::vector<AssetDiagnostic>& diagnostics);
    bool RegisterSource(const std::filesystem::path& source, const ImportOptions& options,
        AssetId& id, std::vector<AssetDiagnostic>& diagnostics);

    std::shared_ptr<const StaticModel>      LoadModel(const AssetId& id, ...);
    std::shared_ptr<const ImageData>        LoadTextureImage(const AssetId& id, ...);
    std::shared_ptr<const MaterialDocument> LoadMaterial(const AssetId& id, ...);
    bool LoadShaderArtifact(const AssetId& id, ShaderArtifactSource& output, ...) const;
    bool LoadSurfaceShader(const AssetId& id, UserSurfaceShaderSource& output, ...) const;
    std::shared_ptr<const EnvironmentAsset> LoadEnvironment(const AssetId& id, ...);
    std::shared_ptr<const TODProfile>       LoadTODProfile(const AssetId& id, ...);
    std::shared_ptr<const AudioClip>        LoadAudioClip(const AssetId& id, ...);

    // 供后台工作线程使用的值拷贝快照（不借用 AssetStore 内部表）
    bool DescribeSource(const AssetId& id, AssetSourceSnapshot& output, ...) const;
    static std::shared_ptr<const StaticModel> LoadModelSnapshot(const AssetSourceSnapshot& source, ...);
    static std::shared_ptr<const ImageData>   LoadImageSnapshot(const AssetSourceSnapshot& source, ...);
};
```

- **单线程目录 + 跨线程快照**：`AssetStore` 本体只在所有者线程调用；后台异步加载前先通过 `DescribeSource()` 拷贝独立的 `AssetSourceSnapshot`，再在工作线程调用静态 `LoadModelSnapshot()` / `LoadImageSnapshot()`。
- **程序内置基础图元**：[`MakeUnitCube()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/Primitives.cpp#L7)（24 顶点 / 36 索引，带法线/UV/切线）与 [`MakeUnitSphere(segments, rings)`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/Primitives.cpp#L27)（细分范围 `segments ∈ [3,512], rings ∈ [2,256]`）直接生成 `MeshData`，也用于异步加载占位与编辑器基础几何体。

## StaticModel 与模型布局准备 {#staticmodel}

[`ImportStaticModel()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/StaticModel.h#L140) 将 FBX（ufbx）和 glTF / GLB（cgltf）统一解码为右手 Y-up、米制单位的 [`StaticModel`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/StaticModel.h#L111)：

| 字段 | 类型 | 说明 |
| :--- | :--- | :--- |
| `meshes` | `std::vector<MeshData>` | 网格顶点（位置/法线/UV/切线）、索引、材质下标与可选每顶点 4 关节蒙皮权重（`MeshSkinVertex`）。 |
| `materials` | `std::vector<MaterialData>` | PBR 基础色/金属度/粗糙度/法线缩放、`alphaMode`/`alphaCutoff`、清漆/丝绒/各向异性/次表面参数与贴图下标。 |
| `nodes` | `std::vector<ModelNode>` | 父子层级与列主序 4×4 局部矩阵。 |
| `textures` / `dependencies` | `TextureReference` / `string` | 相对模型目录的外部贴图路径或嵌入字节（GLB/FBX），以及完整外部依赖清单。 |
| `joints` / `animations` | `ModelJoint` / `ModelAnimationClip` | 按父序排列的骨架逆绑定矩阵与 `Linear`/`Step` 关键帧通道（静态模型为空）。 |

渲染前由 [`BuildStaticModelLayout()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/StaticModelPreparation.cpp#L18) 展平节点层级矩阵、校验所有矩阵为可逆仿射变换并计算整体包围盒。

::: info 蒙皮网格与缺省贴图保护
- **蒙皮网格不重复乘节点矩阵**：`BuildStaticModelLayout()` 检测到 `mesh.skinned == true` 时将 `part.modelFromMesh` 保持为单位阵（[`StaticModelPreparation.cpp#L43-L46`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/StaticModelPreparation.cpp#L43)），因为顶点与逆绑定矩阵导入时已烘入实体局部空间，运行期形变完全来自 `skin = jointWorld * inverseBind`。
- **缺图自动回退**：[`SelectModelMaterialImages()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/StaticModelPreparation.cpp#L87) 在贴图槽为空或解码失败时，自动代入 [`DefaultTextureImage()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/StaticModelPreparation.cpp#L68) 提供的 1×1 白图、1×1 平面法线 `(128,128,255)` 或 2×2 品红棋盘格 Fallback。
:::

## 材质与着色器变体

- **可序列化材质资产**：[`MaterialDocument`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/MaterialDocument.h#L8) 保存所有贴图 `AssetId`（BaseColor、Normal、MetallicRoughness 及多层扩展贴图）、可选自定义着色器引用（`shader` 字段，同时支持离线 `.sebin`、热编译单/多 Pass `.fshader.hlsl` 与 ASE 节点图 `.fshader.json`）、自定义表面参数覆盖表（`shaderParameters`，打包至 `b4` `flareUserParams[8]`）、透明度模式（`Opaque` / `Mask` / `Blend`）与四套进阶材质层（`ClearCoat`、`Sheen`、`Anisotropy`、`Subsurface`）。多 Pass 着色器若任一 Pass 使用 `Cutout` / `Transparent`（`usesAlphaCoverage`），基础色贴图自动走保留 Alpha 的 `TextureRole::CoverageColor`。属性读写走统一的元数据通道 [`WriteMaterialProperty()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/MaterialMetadata.cpp#L75)。
- **CPU 材质实例**：[`ResolveMaterialInstance()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/MaterialInstance.h#L32) 基于父 `PbrRenderMaterial` 叠加局部标量/贴图覆盖与 `addFeatures` / `removeFeatures` 位掩码，生成只读渲染材质，不修改父材质且共享未覆盖纹理。
- **着色器变体宏**：[`MaterialFeature`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/MaterialVariants.h#L10) 定义 10 个不可复用的稳定位标志，[`VariantMacros(mask)`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/MaterialVariants.h#L50) 按固定表顺序生成着色器编译宏，`VariantId(mask)` 输出 8 位十六进制稳定缓存键：

| 枚举位 | 位掩码 | 生成的 HLSL 宏 |
| :--- | :--- | :--- |
| `AlbedoMap` / `NormalMap` / `MetallicRoughnessMap` | `bit 0..2` | `SE_FEATURE_ALBEDO_MAP` / `SE_FEATURE_NORMAL_MAP` / `SE_FEATURE_METALLIC_ROUGHNESS_MAP` |
| `Emissive` / `AlphaTest` / `Skinned` | `bit 3..5` | `SE_FEATURE_EMISSIVE` / `SE_FEATURE_ALPHA_TEST` / `SE_FEATURE_SKINNED` |
| `ReceiveShadow` / `Instanced` / `VertexCompressed` / `DoubleSided` | `bit 6..9` | `SE_FEATURE_RECEIVE_SHADOW` / `SE_FEATURE_INSTANCED` / `SE_FEATURE_VERTEX_COMPRESSED` / `SE_FEATURE_DOUBLE_SIDED` |

## 顶点与纹理压缩

### 16 字节量化顶点

[`CompressMeshVertices()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/VertexCompression.cpp#L101) 将 48 字节的 `MeshVertex` 压缩为固定 16 字节的 [`PackedVertex`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/VertexCompression.h#L15)（3:1 压缩比）：

```cpp
// Source/ResourceLayer/Assets/VertexCompression.h#L15-L22
struct PackedVertex
{
    std::uint16_t position[3]{}; // 按网格 AABB 量化到 16 位无符号定点数（配 MeshQuantization）
    std::uint8_t  normal[2]{};   // 单位法线八面体 8 位编码
    std::uint16_t uv[2]{};       // 半精度浮点 UV (FP16)
    std::uint32_t tangent = 0;   // 4 × 8 位切线与手性符号
};
static_assert(sizeof(PackedVertex) == 16);
```

### BC1 / BC4 / BC5 / BC7 派生纹理烘焙与 `.sebc` 缓存

渲染层不在运行帧实时压缩纹理，而是在资产载入或首帧上传前通过 [`TextureBakeCache`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/TextureBake.h#L43) 一次性生成完整 mip 链块数据，并以 `SHA-256(格式前缀 + 编码器版本 + 源内容键).sebc` 缓存在磁盘（头部 24 字节：`SEBC` magic + version + format + width + height + mipLevels）：

| 格式 | 块大小（4×4） | 压缩比 | 典型用途 | 烘焙入口 |
| :--- | :---: | :---: | :--- | :--- |
| `Bc1Unorm` / `Bc1UnormSrgb` | 8 B | 8:1 | 不透明反照率 / 基础彩色贴图 | [`BakeBc1Texture()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/TextureBake.cpp#L60) |
| `Bc4Unorm` | 8 B | 4:1（单通道） | AO、粗糙度、高度等单通道灰度图（R） | [`BakeBc4Texture()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/TextureBake.cpp#L132) |
| `Bc5Unorm` | 16 B | 4:1（双通道） | 切线空间法线贴图（存 XY，着色器重建 Z） | [`BakeBc5Texture()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/TextureBake.cpp#L97) |
| `Bc7Unorm` / `Bc7UnormSrgb` | 16 B | 4:1（高质量 RGBA） | 带 Alpha 或高质量反照率（Mode 5/6 确定性编码） | [`BakeBc7Texture()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/TextureBake.cpp#L179) |

::: tip 缓存失效与容错策略
- `BakedTextureKey()` 把格式（如 `bc1s:` vs `bc1u:`）和编码器版本（`tex-1` / `bc7-56-1`）编进哈希键，避免线性与 sRGB 纹理串用。
- `.sebc` 磁盘缓存属于**尽力而为的跨进程加速**：读损坏或写盘失败时自动回退到重新编码并返回内存缓存指针，绝不阻断渲染。
:::
