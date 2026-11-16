# 可见性、Hi-Z 与间接绘制

## 包围体与 GPU 候选集

[WorldBoundsCache](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/Visibility.h#L9) 缓存物体的世界包围盒，[BuildFrustum()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/CoreLayer/Math/Bounds.h#L33) 从相机矩阵提取视锥平面。CPU 参考 [CullVisibilityReference()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/VisibilityData.cpp#L57) 与 GPU 共用可见性输入布局，便于逐项比较候选集。

## 层次深度

深度预通道先写入近处遮挡物，[D3D12HiZ](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/D3D12/D3D12HiZ.h#L27) 将深度逐级归约。CPU 对照实现 [TestOcclusion()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/HiZ.cpp#L86) 将世界包围盒投影到屏幕矩形，按覆盖范围选 mip，仅在整个包围盒确定被挡住时剔除。[HiZHistory](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/HiZ.h#L76) 管理分辨率或相机突变后的历史有效性。

## 间接提交与统计

[D3D12Visibility](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/D3D12/D3D12Visibility.h#L25) 执行 GPU 可见性计算，[D3D12Indirect](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/D3D12/D3D12Indirect.h#L34) 准备并记录间接绘制命令。[BuildOcclusionHud()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/OcclusionHud.cpp#L30) 将遮挡统计转成调试显示，[RenderTelemetry](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/RenderTelemetry.h#L84) 保存逐帧的绘制和耗时记录。

## 普通静态网格与 Meshlet 的间接提交

`render.gpuDriven` 允许兼容的不透明静态绘制共享间接提交。每条 64 字节命令携带顶点与索引缓冲视图、对象常量缓冲地址及索引绘制参数。`render.meshletCulling` 按已验证的连续索引区间进行 GPU 视锥剔除，仍使用索引绘制。自定义顶点变形或不满足分组条件的对象走整对象回退路径。[命令组织与提交](https://github.com/xuu-xuu-xuu/FlareEngine/blob/34bad2e/Source/PlatformLayer/Graphics/D3D12/D3D12Heterogeneous.cpp)

`render.gpuDrivenHiZ` 使用当前帧完整几何预绘制的层次深度，只有整个候选范围确定被遮挡时才剔除。近裁面相交或缺少可信深度时保留绘制。两种剔除开关在渲染工作室均默认关闭，调整时应同时观察可见性结果和场景帧耗时。[当前帧可见性着色器](https://github.com/xuu-xuu-xuu/FlareEngine/blob/34bad2e/Resources/Shaders/HeterogeneousCull.hlsl)
