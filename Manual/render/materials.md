# 光照着色、材质接口与多 Pass / ASE Shader

这篇文档讲清引擎如何在像素着色阶段计算表面光照（基础受光与 PBR）、CPU 常量/纹理如何与 HLSL 寄存器严格对齐、分层材质与矩形面光如何运作，以及用户自定义着色器（含离线 `.sebin` 包、Unity 风格单/多 Pass `.fshader.hlsl` 与 138 节点 ASE 连线图 `.fshader.json`）如何安全进入运行时。

## 材质与着色数据流

```text
资产材质 MaterialData / MaterialDocument       离线 LUT（RenderingCooker defaults）
   │  ConfigurePbrMaterial()                     │  direct-energy.sebrdf / rectangle.seltc
   ▼                                             │  sheen.sesheen / skin.seskin
PbrRenderMaterial + layerTextures[6]             ▼
   │                                       ShadingResourceCache::Prepare()
   │                                             │  b5 (480B) + t13..t18（AO/LUT/探针）
   └───────────────────────┬─────────────────────┘
                           ▼
        D3D12Mesh / D3D12ShadingExtensions 绑定至图形根签名
        b0(逐绘制 320B)  b1(帧/CSM/集群)  b3(PCSS 80B)  b4(覆盖 32B)
        b5(扩展帧 480B)  b6(材质层 96B)   b7(局部灯 3600B)
        t0..t2(基础贴图) t3(方向阴影)     t4..t6(IBL)   t13..t24(扩展贴图) t25(局部阴影)
                           │
                           ▼
            Pbr.hlsl / BasicLit.hlsl (PSMain) ──▶ 线性 HDR 颜色目标
```

## 基础光照与 PBR 主着色器

两套内置着色器共享相同的顶点输入布局（支持普通 48B、压缩顶点与 80B 蒙皮顶点），由管线描述决定使用哪套着色器：

- **基础受光**：[`Resources/Shaders/BasicLit.hlsl`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Resources/Shaders/BasicLit.hlsl#L53) 的 `PSMain()` 采用改进 Blinn-Phong 模型，不做切线空间 TBN 重建，支持方向光阴影、Forward+ 点光、局部灯阴影及白模/Toon 外观覆盖（`ApplyLook`）。
- **PBR 完整路径**：[`Resources/Shaders/Pbr.hlsl`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Resources/Shaders/Pbr.hlsl#L103) 的 `PSMain()` 根据宏开关组合以下管线特性：
  - `SE_VERTEX_COMPRESSED` / `SE_SKINNED` / `SE_INSTANCED`：八面体法线解压、骨骼调色板蒙皮（`t8`）、实例化数据拉取（`t7`）。
  - `SE_NORMAL_BC5`：BC5 法线贴图只采样 RG 并解析重建 `Z = sqrt(saturate(1 - x² - y²))`。
  - `SE_MATERIAL_SURFACE`（布局 38，`b4`）：统一处理 `Opaque` / `Mask`（Alpha 裁切）/ `Blend`（半透明）及双面逻辑法线翻转（`logicalFront`）。
  - `SE_SHADING_EXTENSIONS`（布局 40，`b5` / `b6` / `t13..t24`）：启用 Kulla–Conty 多散射能量补偿、清漆（ClearCoat）、绒光（Sheen）、各向异性（Anisotropy）、预积分次表面散射（Subsurface）与矩形面光（Area Light）。

## 环境与材质绑定准备

- **材质绑定**：[`ConfigurePbrMaterial()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/StaticModelRender.cpp#L77) 把 CPU 材质资产的基础色、金属度、粗糙度、法线强度（钳制到 `[-8, 8]`）、Alpha 模式以及四个分层材质参数写入 `PbrRenderMaterial`。
- **环境光照绑定**：[`PrepareEnvironmentBindings()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/EnvironmentRender.cpp#L61) 把辐照度立方体（`t4`）、预滤波镜面立方体（`t5`）、BRDF 查找表（`t6`）、相机天空逆旋转投影矩阵以及曝光/偏航角打包为 `rhi::EnvironmentBindings`。

## 材质分层与矩形面光（Schema 40）

HDR 前向材质扩展使用独立布局版本 `ShadingExtensionLayoutVersion = 40`，定义在 [`MaterialLayout.h`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/MaterialLayout.h#L10) 并由 `static_assert` 锁定大小和字节偏移，与 [`ShadingBindings.hlsli`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Resources/Shaders/ShadingBindings.hlsli#L4) 一一对应；它与材质覆盖布局 38（`b4`，32 字节）正交组合，避免管线布局版本号爆炸。

```cpp
// Source/PlatformLayer/Graphics/MaterialLayout.h#L55
struct alignas(16) ShadingFrameConstants          // register(b5), 480 字节
{
    Vec4 modes{}, viewport{}, probeOrigin{}, probeSpacing{}, probeDimensions{};
    std::array<Vec4, 9> globalSh{};               // 全局 9 系数球谐漫反射
    std::array<Vec4, 4> rectCenter{}, rectHalfU{}, rectHalfV{}, rectRadiance{}; // 最多 4 盏矩形面光
};

struct alignas(16) ShadingMaterialConstants       // register(b6), 96 字节
{
    Vec4 coat{}, sheen{}, anisotropy{}, subsurface{}, subsurfaceColor{}, textureFlags{};
};
```

### 扩展寄存器与纹理槽速查表

| 寄存器 | 名称 / 格式 | 含义 |
| --- | --- | --- |
| `b4` | `MaterialSurfaceConstants` (32 B) | 布局 38：`baseColorFactor` + `cutoff / alphaMode / doubleSided / detSign` |
| `b5` | `ShadingFrameConstants` (480 B) | 布局 40：多散射/探针开关、视口、9 系数全局 SH、最多 4 盏矩形面光 |
| `b6` | `ShadingMaterialConstants` (96 B) | 布局 40：清漆、绒光、各向异性、次表面及 6 槽贴图掩码 `textureFlags` |
| `b7` | `LocalLightConstants` (3600 B) | 最多 8 盏局部光参数与 48 个投影矩阵（详见 [阴影文档](shadows.md)） |
| `t13` | `screenOcclusionMap` (`RG16F`) | `x` = 屏幕空间或光追 AO，`y` = 接触阴影或太阳光追可见度 |
| `t14` | `directEnergyMap` (`RGBA32F`) | `direct-energy.sebrdf`，Kulla–Conty 直接光多散射能量补偿表 |
| `t15` | `diffuseProbeMap` (`RGBA32F`) | 局部漫反射 SH 网格（宽 10 像素 × 最多 16384 探针行） |
| `t16` / `t17` | `ltcMatrixMap` / `ltcAmplitudeMap` | `rectangle.seltc` + `sheen.sesheen`（上传时合并打包） |
| `t18` | `skinDiffuseMap` (`RGBA32F`) | `skin.seskin`，曲率驱动预积分次表面漫反射表 |
| `t19..t21` | `coatWeight/Roughness/NormalMap` | 清漆权重（R）、粗糙度（G）、独立清漆法线（RGB） |
| `t22..t23` | `sheenColorMap` / `sheenRoughnessMap` | 绒光颜色（**sRGB**）与绒光粗糙度（**读取 Alpha 通道**） |
| `t24` | `anisotropyMap` | 各向异性切线方向（RG 编码 `[-1, 1]`）与强度（B） |

### 矩形面光的双精度积分路径

[`BuildFrameData()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Application/FrameData.cpp#L50) 从启用的 `RectangleLightComponent` 提取中心、半宽轴、半高轴与辐亮度；[`ShadingResourceCache::Prepare()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/ShadingResources.cpp#L173) 按稳定实体 ID 选出最多 4 盏并按需加载离线 LUT。

着色器中的 [`RectangleLighting()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Resources/Shaders/ShadingBindings.hlsli#L211) 会自动区分材质复杂度：普通金属/粗糙度材质直接走解析 **LTC（Linearly Transformed Cosines）** 地平线裁剪与多边形立体角积分；而开启了多散射、清漆、绒光、各向异性或次表面的材质，为了不拿单散射 GGX 的 LTC 表错误冒充多层积分，会切换为 8×8 面积求积并复用完整的 `SurfaceDirectBrdf()`：

```hlsl
// Resources/Shaders/ShadingBindings.hlsli#L217
[loop] for (uint i = 0; i < min((uint)shadingModes.w, 4u); ++i)
{
    if (shadingModes.x > .5 || layerCoat.x > 0 || any(layerSheen.rgb > 0)
        || layerAnisotropy.x > 0 || layerSubsurface.x > 0)
    {
        // 分层材质或开启多散射补偿：8×8 固定面积求积，复用同一 SurfaceDirectBrdf
        [loop] for (uint y = 0; y < 8; ++y)
        [loop] for (uint x = 0; x < 8; ++x)
        {
            float2 q = (float2(x, y) + .5) / 8 * 2 - 1;
            float3 samplePosition = rectCenter[i].xyz + rectHalfU[i].xyz * q.x + rectHalfV[i].xyz * q.y;
            ...
            result += SurfaceDirectBrdf(...) * rectRadiance[i].rgb * (area * emitterCos / (64 * squared));
        }
        continue;
    }
    // 普通 PBR 材质：LTC 解析多边形积分（漫反射单位矩阵 + 镜面逆 LTC 矩阵）
    float diffuse  = RectangleCosineIntegral(i, p, n, t, b, float4(1, 0, 0, 1));
    float specular = RectangleCosineIntegral(i, p, n, t, b, ltcInverse);
    result += rectRadiance[i].rgb * (color * (1 - metal) * (1 - f0) * diffuse
                                   + (f0 * amplitude.x + (1 - f0) * amplitude.y) * specular);
}
```

四种默认查找表由离线工具 [`RenderingCooker defaults`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Tools/RenderingCooker.cpp#L23) 生成到 `Resources/Lighting/` 并随 Player 资源分发，渲染线程绝不现场烘焙 LUT。

## 自定义着色器如何进入运行时

用户编写的自定义 PBR 着色器不能绕过固定绑定契约，必须通过校验与打包流程进入 PSO 缓存：

1. **源码与依赖摘要**：[`ReadD3D12ShaderSource()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12ShaderProgram.cpp#L613) 展开 `#include`、宏定义、绑定布局版本与源码 SHA-256，生成唯一缓存键。
2. **固定接口打包**：[`WriteD3D12UserPbrPackage()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12ShaderProgram.cpp#L785) 将通过 HLSL 反射校验的字节码与接口摘要写入用户着色器包。
3. **运行时加载校验**：[`LoadD3D12UserPbrPackage()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12ShaderProgram.cpp#L805) 在 `D3D12PipelineCache::Prepare()` 每次准备 Artifact 时校验文件完整性与寄存器反射签名，拒绝被篡改或布局不匹配的产物。

### 表面着色器（`.fshader.hlsl`）与 ASE 节点图（`.fshader.json`）多 Pass 热编译通道

除离线 `.sebin` 包外，引擎还提供面向日常开发的零重启热编译通道（`UserSurfaceMaterialLayoutVersion = 43`），原生支持 **Unity ShaderLab 风格单 Pass / 多 Pass 着色器**与 **138 节点 ASE 可视化连线图**：

```text
.fshader.hlsl (Shader / Properties / SubShader / [HLSLINCLUDE] / Pass 1..8)
.fshader.json (138 节点 ASE 图 ──► CompileShaderGraphToHlsl() 拓扑生成等价 HLSL)
        │
        ▼ ParseSurfaceShaderSource()  [Source/ResourceLayer/Assets/SurfaceShader.cpp]
SurfaceShaderMetadata
  ├─ parameters[0..N] ──► PackSurfaceShaderParameters() ──► register(b4) flareUserParams[8]
  ├─ shadowPassIndex  ──► 自动挑选首个非 Cull Front 写深度不透明/镂空 Pass 投射阴影
  └─ passes[0..7]     ──► StageSurfaceShaderVariant(..., passIndex) ──► FlareUserSurfaceBody.hlsli
                                │
                                ▼ MaterialSurfacePipelineCache::LoadUserSurface()
                   按 (sha256, passIndex, kind, alphaMode, blend, depth, ...) 编译/缓存 PSO
                     ├─ 失败自动回退 lastGoodUserPipelines_（视口零黑屏 + 精准文件行列报错）
                     └─ SandboxRenderer 按 Pass 独立分发至 opaque / transparent 队列
```

1. **零额外根签名槽位复用与全阶段采样**：原有逐 Draw `MaterialSurfaceConstants`（`register(b4)`，32B）已按 D3D12 CBV 硬件要求分配 256B 对齐切片；布局 `43` 直接复用剩余字节扩展为 176B 的 `UserMaterialSurfaceConstants`（追加 `flareTime` 与 `flareUserParams[8]` 共 32 个标量槽位），并将 `b4`、材质基础贴图 `t0..t2` 及各向异性采样器 `s0` 的可见性统一扩为 `D3D12_SHADER_VISIBILITY_ALL`，使 `vert()`（`VSMain` 顶点位移高度图 `tex2Dlod`）与 `surf()`（`PSMain`）均可直接读取材质参数与贴图。
2. **Unity ShaderLab 风格多 Pass 解析与词法桥接**：
   - [`ParseSurfaceShaderSource()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/2318549/Source/ResourceLayer/Assets/SurfaceShader.cpp#L1283) 解析 `Shader "Name" { Properties { ... } SubShader { [HLSLINCLUDE ... ENDHLSL] Pass { Name / Shading / Blend / Cull / ZWrite / ZTest / HLSLPROGRAM ... ENDHLSL } } }`（单着色器最多 `MaxSurfaceShaderPasses = 8`，保留显式 `Shading` 不被 `#pragma surface` 覆盖，并 100% 向后兼容旧版 `// @FlareShader` 指令与 `CGPROGRAM` / `struct Input` 粘贴代码）。
   - [`UserSurfaceContract.hlsli`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/2318549/Resources/Shaders/UserSurfaceContract.hlsli#L92) 提供 `FlareSurfInput`（含 `uv`、`worldPos`、`localPos`、`objectPos`、`viewDir`、`lightDir`、`lightColor`、`screenUV`、`facing` 等）/ `FlareSurfOutput` / `FlareVertexData`、`_Time` / `_SinTime` / `_CosTime` / `unity_DeltaTime`，以及通过宏桥接避开 HLSL 内置重载冲突的 `tex2D(_MainTex / _BumpMap / _MetallicGlossMap, uv)`、`tex2Dlod` 与 `UnpackNormal`。
   - [`BuildSurfaceShaderUserInclude()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/2318549/Source/ResourceLayer/Assets/SurfaceShader.cpp#L1617) 为每个 Pass 生成带 `#line` 原始文件行号映射的 `FlareUserSurfaceBody.hlsli`：对 `Input`、`SurfaceOutput`、`VertexInput`、`fixed*` 使用作用域内宏桥接，而对 `Properties` 参数名采用词法级独立标识符替换 [`ExpandUserParamReferences()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/2318549/Source/ResourceLayer/Assets/SurfaceShader.cpp#L1486)（跳过注释、字符串与 `.` 成员访问，确保用户声明同名属性时写 `o.Metallic = Metallic;` 不会破坏结构体字段且 1:1 保留行号）。
3. **多 Pass D3D12 管线状态扩展**：
   - **发光叠加与隔墙透视**：`rhi::BlendMode::Additive`（`SrcAlpha + One`）与 `rhi::DepthMode::ReadOnlyGreater`（`D3D12_COMPARISON_FUNC_GREATER` 配合只读 `transparentDsv` 实现角色隔墙 X-Ray 轮廓）；`ValidMaterialSurfacePipeline()` 仅对 `UserSurfaceMaterialLayoutVersion` 开放上述扩展组合，不破坏既有内建材质校验契约。
   - **镜像安全且不污染屏幕缓冲的 `Cull Front`**：`DrawIndexed::Pbr::cullFront` 写入 `b0.options.z = 2.0` 与 `b4.surfaceCoverage.z = 2.0`，在 `Pbr.hlsl`、`MaterialCoverage.hlsli` 与 `ShadowDepthSurface.hlsli` 中依据 `logicalFront` 丢弃正面保留背面（兼容负缩放镜像物体）；同时 [`D3D12DepthNormals.cpp#L181`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/2318549/Source/PlatformLayer/Graphics/D3D12/D3D12DepthNormals.cpp#L181) 在屏幕空间深度法线预绘中显式跳过 `cullFront` 外扩描边壳，防止反向法线污染 GTAO / SSR / SSGI。
   - **智能阴影 Pass 选择**：`FinalizeMetadataPasses()` 自动计算 `shadowPassIndex`（优先第一个非 `Cull Front` 且写深度的不透明/镂空 Pass），避免外扩描边或外层护盾污染阴影贴图；同时通过 `usesAlphaCoverage` 在任意 Pass 需要 Alpha 时自动保护纹理槽位使用 `TextureRole::CoverageColor`。
4. **ASE 节点图（138 节点）与热重载保底**：
   - [`CompileShaderGraphToHlsl()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/2318549/Source/ResourceLayer/Assets/ShaderGraph.cpp#L983) 内置 **9 大分类 138 个节点**（`Input`、`Property`、`Texture`、`UV`、`Math`、`Vector`、`Color`、`Procedural`、`Artistic` 及 `MasterSurface`），经环检测与双通道独立拓扑排序（`WorldOffset` 仅提取顶点可达子图写入 `vert()`）、自动向量宽度转换后生成统一 `Shader / Properties / SubShader` 源码，并同步提供 CPU `112×112` 动态光源预览球渲染（[`RenderShaderGraphPreviewSphere()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/2318549/Source/ResourceLayer/Assets/ShaderGraph.cpp#L2233)）。
   - [`MaterialSurfacePipelineCache::LoadUserSurface()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/2318549/Source/FunctionLayer/Rendering/MaterialSurfacePipelines.cpp#L53) 按 `(sha256, passIndex, kind, alphaMode, blend, depth, ...)` 缓存 PSO 与 `.sebin`；编辑保存触发 `WM_ACTIVATEAPP` 热编译失败时，逐 Pass 回退到 `lastGoodUserPipelines_` 保证视口零黑屏，并将带原始文件行列号的诊断推送至控制台支持双击直达。

::: warning 修改材质常量布局必检三处
改动任意 `b0`–`b7` 结构体或 `t*` 纹理槽时，必须同步核对：
1. C++ 侧 `Source/PlatformLayer/Graphics/MaterialLayout.h` 的 `static_assert` 大小、偏移与布局版本号；
2. HLSL 侧对应 `.hlsli` 的 `cbuffer` 声明与 `register(...)` 槽位；
3. `D3D12PipelineCache.cpp` 中的根签名参数表与持久库版本（如 `libraryVersion`）。
:::
