# 环境光照、大气与反射

## 环境贴图进入渲染器

[DecodeRadiance()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Assets/HdrImage.cpp#L19) 把 Radiance HDR 解码为浮点像素。[BakeEnvironment()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Assets/Environment.h#L31) 从全景图生成辐照度立方体、预滤波辐射立方体和 BRDF 查找表，[EnvironmentRenderCache](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/EnvironmentRender.h#L14) 保存当前资源版本并上传。[PrepareEnvironmentBindings()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/EnvironmentRender.cpp#L50) 将结果交给天空和材质绘制。

## 大气与雾的 CPU 模型

[Atmosphere](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/Atmosphere.h#L22) 根据大气参数重建透射率查找表，[SkyRadiance()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/Atmosphere.cpp#L114) 沿视线积分天空散射。[FogTransmittance()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/Atmosphere.cpp#L152) 按相机高度和视距计算雾的透射率。时间变化的数据来源见[昼夜系统](../runtime/daylight.md)。

## 探针与屏幕空间反射的 CPU 参考实现

[BoxProjectReflection()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/Reflection.cpp#L66) 将反射方向修正到探针包围盒。[BlendReflectionProbes()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/Reflection.cpp#L95) 混合邻近探针的权重，[PrefilterGgx()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/Reflection.cpp#L131) 按粗糙度预滤波。[TraceScreenSpaceReflection()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/Reflection.cpp#L157) 则沿反射射线查询屏幕深度并报告命中。

## 全局与局部漫反射探针

`render.diffuseSh` 启用九系数球谐漫反射。环境辐照数据可提供全局系数，预滤波环境立方体继续提供镜面光。启用 `render.localDiffuseSh` 并指定有效的 `render.diffuseProbe` 资产后，着色器在格子八角间插值。位置超出网格、格子跨墙或任一角探针无效时，采样回到全局系数。局部烘焙值含场景照明，原样参与着色，全局环境贡献乘环境强度。[帧绑定与资源缓存](https://github.com/xuu-xuu-xuu/FlareEngine/blob/34bad2e/Source/FunctionLayer/Rendering/ShadingResources.cpp#L173) · [CPU 插值参考](https://github.com/xuu-xuu-xuu/FlareEngine/blob/34bad2e/Source/ResourceLayer/Assets/DiffuseProbeBake.cpp#L218)

`DiffuseProbeCooker` 的 `local` 模式读取静态场景、点光和 HDR 环境。射线首个交点接收有遮挡的直接光，其表面颜色决定反弹到探针的漫反射。离线烘焙输出规则网格，移动灯光或几何后需重新烘焙。模板“渲染工作室”使用全局环境系数，局部网格默认关闭。[离线网格烘焙](https://github.com/xuu-xuu-xuu/FlareEngine/blob/34bad2e/Source/ResourceLayer/Assets/DiffuseProbeBake.cpp#L196) · [工具参数](https://github.com/xuu-xuu-xuu/FlareEngine/blob/34bad2e/Source/ToolLayer/Tools/DiffuseProbeCooker.cpp#L37)

## 室内实时光照

`ApplyFeatureFrameData()` 把场景的 `render.indirect`、`render.reflections`、`render.volumetricFog` 写入当前帧。`D3D12Presentation.cpp` 在不透明前向绘制完成后调用 `RecordD3D12IndoorLighting()`，将线性 HDR 复制为只读输入，与同分辨率深度、几何法线和材质数据一同传给 `Resources/Shaders/IndoorLighting.hlsl`。透明物体在这个通道之后合成。预绘制覆盖不完整时保留原帧，避免使用错误的遮挡或材质。

| 开关 | 着色器与数据 | 画面边界 |
| --- | --- | --- |
| `render.indirect` | `DiffuseBounce()` 在宽高各四分之一的缓冲区采样周围可见表面，用两端法线、遮挡、距离和源表面的金属度估计一次漫反射。`ReconstructDiffuse()` 以平面距离和几何法线重建全分辨率入射光，再乘接收表面的漫反射颜色 | 基础环境光或探针光保留。画面外的光源表面无法贡献这次反弹 |
| `render.reflections` | `ReflectionHit()` 在视空间追踪可见表面并细分命中位置，`ReflectedChange()` 按菲涅耳、粗糙度和命中置信度替换相应的环境镜面贡献 | 未命中时保留原始环境反射。掠过屏边时逐渐退回环境，粗糙度达到 0.68 后跳过追踪 |
| `render.volumetricFog` | `ApplyFog()` 按 Beer–Lambert 定律计算均匀介质的透射率。有方向光阴影时沿射线取 16 个可见度样本。每盏局部灯在射线穿过其影响球的区间取 16 个等角度样本，以便密集采样近灯处的亮度和遮挡 | 积到第一个不透明表面或 `render.fogDistance`。透明表面自身的深度不参与这次积分 |

可从启动器的“室内展厅”模板对照这三项设置，也可分别在已有项目的场景渲染设置组件启用。[室内展厅的编辑器操作](../user/indoor-lighting.md) · [正式包同镜头画面对比](../user/indoor-lighting.md#gallery-comparison)
