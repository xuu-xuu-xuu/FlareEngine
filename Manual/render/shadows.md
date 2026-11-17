# 阴影、分簇灯光与环境遮蔽

## 方向光阴影

[PrepareDirectionalShadow()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/ShadowSystem.cpp#L6) 从方向光和场景阴影视图准备矩阵与阴影参数。绘制先在光源视角写入深度，再由着色器比较接收点深度与阴影图。D3D12 的记录入口是 [RecordD3D12Shadow()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/D3D12/D3D12Shadow.cpp#L36)。

## 级联与灯光分簇

CPU 算法 [PrepareCascadeShadows()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/CascadeShadow.cpp#L19) 根据相机视锥切片拟合各级阴影投影，[SelectCascade()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/CascadeShadow.cpp#L78) 按视深选择级别和混合权重。点光源分簇由 [BuildLightClusters()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/LightClustering.cpp#L32) 构建每个空间分区的光源列表，容量不足时可用 [FillLightClustersGlobal()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/LightClustering.cpp#L111) 回退到全局列表。

## 屏幕空间遮蔽的 CPU 参考实现

[ComputeGtao()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/AmbientOcclusion.cpp#L65) 从视空间深度和法线估算间接光遮蔽。[ComputeContactShadows()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/AmbientOcclusion.cpp#L108) 沿光源方向检查近距离遮挡，输出直接光可见度。低分辨率结果通过 [UpsampleBilateral()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/AmbientOcclusion.cpp#L144) 按深度和法线升采样。

## 方向光级联和 Forward+ 的 GPU 路径

从场景的渲染设置启用 `render.cascades`，`PrepareDirectionalCascadeShadow()` 按相机视锥切片生成最多三级光空间投影。绘制阴影时逐级挑选投射者，因此镜头外的物体也能在镜头内留下影子。`render.shadowPcss` 在级联与 PCF 开启时使用遮挡搜索，根据光源角半径与遮挡距离扩大半影。[级联准备](https://github.com/xuu-xuu-xuu/FlareEngine/blob/34bad2e/Source/FunctionLayer/Rendering/ShadowSystem.cpp#L39)

`render.clusteredLighting` 收集最多 256 盏点光。D3D12 按 16 × 9 个屏幕分区、24 个深度区间建表，每簇保留最近的 32 盏灯，以稳定实体 ID 处理等距离情况。全局索引容量溢出时该帧退回旧路径的前 8 盏灯。`render.clusterDebug` 可显示空间分簇热力图。[整理灯光快照](https://github.com/xuu-xuu-xuu/FlareEngine/blob/34bad2e/Source/FunctionLayer/Rendering/ForwardRenderer.cpp#L9) · [GPU 建表](https://github.com/xuu-xuu-xuu/FlareEngine/blob/34bad2e/Source/PlatformLayer/Graphics/D3D12/D3D12LightClustering.cpp#L107)

## 深度、法线与屏幕空间遮蔽

`DepthNormals` 使用实际网格和蒙皮姿态写入 D32 深度和 RG16F 世界空间几何法线，裁切材质与主绘制共享覆盖规则。半分辨率 GTAO 结果经深度与法线引导的双边升采样，在不透明着色时调制间接光。接触阴影沿方向光查询深度，只作用于接收阴影的表面。将 AO 强度设为零时遮蔽系数为 1。[预绘制](https://github.com/xuu-xuu-xuu/FlareEngine/blob/34bad2e/Source/PlatformLayer/Graphics/D3D12/D3D12DepthNormals.cpp#L109) · [遮蔽计算](https://github.com/xuu-xuu-xuu/FlareEngine/blob/34bad2e/Source/PlatformLayer/Graphics/D3D12/D3D12AmbientOcclusion.cpp#L253)

## 点光投影与聚光锥

`PrepareForwardLighting()` 按稳定实体 ID 分配八个 `ForwardLighting::localLights` 槽位。聚光灯和可投影的点光各占一个槽。超量点光回到普通点光路径，超量聚光会生成诊断。聚光在内外锥夹角之间平滑衰减；设置 `innerAngle == outerAngle` 可获得硬切光锥。

一个投影点光占用同一 D32 深度数组的六个面，投影聚光占用一层，每层为 512 × 512。`RecordShadowedFrame()` 与 `RecordShadowedFrameGraph()` 都使用完整投射者列表。即使场景没有太阳，局部灯仍能投影。绘制材质时，HLSL 在 `b7` 读取局部光常量、在 `t25` 采样对应的深度层，将 3 × 3 PCF 可见度用于该灯的直接光。关掉网格的接收属性可单独移除这部分遮挡。[灯光组件与场景格式](../runtime/light.md)
