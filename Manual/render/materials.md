# 光照着色、材质接口与 Shader 包

这篇文档讲清引擎如何在像素着色阶段计算表面光照（基础受光与 PBR）、CPU 常量/纹理如何与 HLSL 寄存器严格对齐、分层材质与矩形面光如何运作，以及用户自定义 PBR 着色器如何安全打包进入运行时。

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

::: warning 修改材质常量布局必检三处
改动任意 `b0`–`b7` 结构体或 `t*` 纹理槽时，必须同步核对：
1. C++ 侧 `Source/PlatformLayer/Graphics/MaterialLayout.h` 的 `static_assert` 大小、偏移与布局版本号；
2. HLSL 侧对应 `.hlsli` 的 `cbuffer` 声明与 `register(...)` 槽位；
3. `D3D12PipelineCache.cpp` 中的根签名参数表与持久库版本（如 `libraryVersion`）。
:::
