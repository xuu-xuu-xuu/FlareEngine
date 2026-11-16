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
