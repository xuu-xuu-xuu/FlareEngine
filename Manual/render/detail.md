# LOD、HLOD 与代理几何

这篇文档说明引擎如何在不同观察距离上自动切换几何细节：近景用顶点缓存优化与单模型 LOD（带 4×4 Bayer 像素抖动平滑交接），中远景用按空间单元分区的异步流送 HLOD 代理，超远景则切换到多视角烘焙的公告板 Impostor 代理。

## 多层次细节选择总览

```text
离线资产烘焙（ResourceLayer/Assets）                运行时选择与流送（FunctionLayer/Rendering）
────────────────────────────────────                ────────────────────────────────────────────
StaticModel ──▶ OptimizeVertexCache()               近景：SelectLodLevel() + UpdateLodTransition()
            ──▶ BakeLodChain() (LOD 0..N-1)               └─ Pbr.hlsl 4×4 Bayer Dither 跨层过渡
            ──▶ BakeMeshlets() (≤64 三角形簇)       中景：HlodStreamer (后台线程读 .sehlod)
                                                          └─ HlodStream::UpdateResidency()
HlodInstance[] ──▶ BakeHlod() (.sehlod)                   └─ SelectHlodProxy() + HlodProxyCache (LRU/预算)
                                                    远景：ImpostorViewPair() (16 视角图集混合)
MeshData ──▶ BakeImpostorAtlas() (多方位角图集)
```

## 单模型 LOD 与顶点缓存优化

| 函数 | 源码位置 | 职责 |
| --- | --- | --- |
| `OptimizeVertexCache()` | [`MeshOptimization.h#L13`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/MeshOptimization.h#L13) | 贪心重排索引（`≤ 4096` 三角形）改善 GPU 32 槽 FIFO 顶点缓存命中率（可用 `AverageCacheMissRatio()` 对比 ACMR） |
| `BakeLodChain()` | [`LodBake.cpp#L117`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/LodBake.cpp#L117) | 离线生成简化层级链；运行时 `LOD 0` 复用主索引缓冲，`LOD 1..N-1` 存放在 `StaticRenderMesh::lodIndices` |
| `SelectLodLevel()` | [`LodSelect.cpp#L13`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/LodSelect.cpp#L13) | 根据包围球屏幕投影像素误差选出目标层级（含滞回区间避免边界来回跳动） |
| `UpdateLodTransition()` | [`LodSelect.cpp#L54`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/LodSelect.cpp#L54) | 推进新旧 LOD 过渡进度，交由 [`Pbr.hlsl#L113`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Resources/Shaders/Pbr.hlsl#L113) 的 `ApplyLodCoverage()` 用 4×4 Bayer 阈值做像素级互补抖动交接 |

## 分区代理（HLOD）与后台流送

当场景包含大量中远景独立实例时，HLOD 将同一空间单元内的多个实例合并为低面数代理几何：

1. **离线分区烘焙**：[`BakeHlod()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/HlodBake.cpp#L71) 按单元边长 `cellSize` 把静态实例归组并烘焙代理网格与材质，导出 `.sehlod` 文件。
2. **后台线程流送**：[`HlodStreamer`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/HlodStreamer.h#L13) 在独立工作线程完成文件分块读取与反序列化（取消时通过 `std::stop_source` 协作中止并 `join`，绝不 `detach` 线程），呈现线程每帧调用 `Poll()` 非阻塞获取结果；读取未完成时无缝回退到原始实例绘制。
3. **驻留半径与显存预算**：
   - [`HlodStream::UpdateResidency()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/HlodStream.cpp#L22) 按相机位置与驻留半径更新活跃单元；
   - [`SelectHlodProxy()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/HlodSelect.cpp#L13) 按屏幕投影像素误差决定单元渲染原始实例还是代理网格；
   - [`HlodProxyCache`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/HlodProxyCache.h#L9) 纯策略控制显存上限（`budgetBytes`）、每帧上传上限（`uploadsPerFrame`）与 LRU 淘汰选定（只淘汰本帧未被选中的单元），不额外复制 GPU 资源所有权：

```cpp
// Source/FunctionLayer/Rendering/HlodProxyCache.h#L18
cache.Configure(vramBudgetBytes, uploadsPerFrame);
cache.BeginFrame();
// 选中本帧需要代理的单元：命中则 Touch(cell)，未命中且 CanUpload() 时上传，超预算时循环 SelectEviction() 释放最久未用单元
```

## Meshlet 与公告板代理（Impostor）

- **Meshlet 切分与 CPU 剔除参考**：[`BakeMeshlets()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/MeshletBake.cpp#L92) 把网格划分为顶点与三角形数量受限的小簇，[`CullMeshlets()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/MeshletBake.cpp#L233) 提供 CPU 端可见性筛选对照；运行时 GPU 双轨提交见 [Mesh Shader 双轨管线](mesh-shader.md)。
- **多视角公告板代理**：[`BakeImpostorAtlas()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/ImpostorBake.cpp#L40) 从环绕模型的多个方位角离线渲染图集；运行时 [`ImpostorViewPair()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/ImpostorBake.cpp#L156) 根据当前相机视线方位角选出相邻两帧视角索引及混合权重，超过 `impostorDistance` 的超远实例直接渲染朝向相机的 Alpha-Test 四边形。

::: tip 调试与开关互斥提醒
- 环境变量 `FLARE_LOD_LEVEL=0..3` 可将所有 LOD 锁定到指定层级并跳过过渡抖动，便于同相机做像素对比测试。
- 当前运行时在开启 `render.instancing`（GPU 实例化）时会自动关闭单模型 LOD 与 HLOD，以避免跨实例网格切分破坏同批次实例化合批（见 [`FeatureFrameData.cpp#L99`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Application/FeatureFrameData.cpp#L99)）。
:::
