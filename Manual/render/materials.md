# 光照着色、材质接口与 Shader 包

## 基础光照与 PBR

基础光照的 [PSMain()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Resources/Shaders/BasicLit.hlsl#L43) 从法线、光照方向和观察方向计算漫反射与高光。PBR 着色器的 [PSMain()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Resources/Shaders/Pbr.hlsl#L69) 使用金属度、粗糙度及法线等材质输入。两种路径接收的网格顶点可以共用，使用哪套着色器由管线描述决定。

## 环境与材质绑定

[ConfigurePbrMaterial()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/StaticModelRender.cpp#L57) 将渲染材质准备为绘制绑定。环境资产提供漫反射辐照度、预滤波辐射和 BRDF 查找表，[PrepareEnvironmentBindings()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/EnvironmentRender.cpp#L50) 为环境光照路径组织这三种输入。

## 自定义着色器如何进入运行时

[ReadD3D12ShaderSource()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/D3D12/D3D12ShaderProgram.cpp#L412) 记录源码、预处理结果和依赖。[WriteD3D12UserPbrPackage()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/D3D12/D3D12ShaderProgram.cpp#L535) 将固定 PBR 接口的程序打包；[LoadD3D12UserPbrPackage()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/D3D12/D3D12ShaderProgram.cpp#L555) 在创建管线时读取并校验接口。修改材质绑定布局时需要同时核对 CPU 常量、HLSL 声明和接口摘要。

## 材质分层和矩形面光

HDR 前向材质使用独立的 `ShadingExtensionLayoutVersion` 40。每帧常量 `b5`（480 字节）保存全局和局部漫反射探针、矩形面光等数据，每材质常量 `b6`（96 字节）保存清漆、绒光、各向异性和次表面漫反射参数。纹理位于 `t13..t24`，其中绒光颜色使用 sRGB，绒光粗糙度读取数据纹理的 alpha。裁切与透明的覆盖布局 38 可和布局 40 同时使用。[C++ 布局定义](https://github.com/xuu-xuu-xuu/FlareEngine/blob/34bad2e/Source/PlatformLayer/Graphics/MaterialLayout.h#L10) · [HLSL 绑定](https://github.com/xuu-xuu-xuu/FlareEngine/blob/34bad2e/Resources/Shaders/ShadingBindings.hlsli#L4)

`BuildFrameData()` 从启用的矩形面光取出中心、半宽轴、半高轴及辐亮度。`ShadingResourceCache::Prepare()` 按稳定实体 ID 准备最多四个矩形面光以及可用的查找表。普通材质用 LTC 表计算矩形光的贡献，带分层或多散射的路径对面光积分。四种查找表由 `RenderingCooker defaults` 离线生成，随 Player 资源一起分发。[灯光快照](https://github.com/xuu-xuu-xuu/FlareEngine/blob/34bad2e/Source/FunctionLayer/Application/FrameData.cpp#L30) · [查表与面光准备](https://github.com/xuu-xuu-xuu/FlareEngine/blob/34bad2e/Source/FunctionLayer/Rendering/ShadingResources.cpp#L173) · [离线工具](https://github.com/xuu-xuu-xuu/FlareEngine/blob/34bad2e/Source/ToolLayer/Tools/RenderingCooker.cpp#L23)
