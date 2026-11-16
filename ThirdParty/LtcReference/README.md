# LTC 离线参考依赖

本目录只服务于 CPU 离线拟合器；运行时不链接 GLM 或原始演示程序，没有引入 CImg。

- LTC 仓库：<https://github.com/selfshadow/ltc_code>，固定提交 `31e5e96b54f98f33098f8503003119ba2231a1c6`。
- GLM 仓库：<https://github.com/g-truc/glm>，固定提交 `40398d67cd3e4f74b08649eda428dc411d801fd5`。这是上述 LTC 提交的 `external/glm` gitlink，不能换成浮动最新版。
- `fit/`、`original/fitLTC.cpp`、`original/README.md` 与 GLM 原始文件均按上游字节保留，逐文件 SHA-256 见 `UPSTREAM.json`。
- `LICENSE` 是 LTC 原许可证，要求保留版权、再分发条款和论文引用；二进制分发也需携带。
- GLM 采用其 MIT 选项；完整上游说明在 `glm/manual.md` 与 `glm/readme.md`，便于分发的完整 MIT 条文另存 `glm/LICENSE-MIT.txt`。

论文引用：Eric Heitz, Jonathan Dupuy, Stephen Hill, David Neubelt. **Real-Time Polygonal-Light Shading with Linearly Transformed Cosines**. ACM Transactions on Graphics 35(4), 2016. 原作者项目及代码链接见上游 README。

引擎衍生适配器是 `Source/ResourceLayer/Assets/LtcFitAdapter.h`，没有覆盖上游原件。它移除了绘图/演示/文件输出，保留 MIS 三次误差目标、顺序 warm start 与 100 次 Nelder–Mead；生产域为 roughness `[.045,1]`、Nv `[1e-4,1]`。V2 将原 NDF 分支替换为 Heitz 2018 的 GGX 可见法线采样及相应 PDF，并在 Nv 两端加密。该采样数学参考为 [JCGT 7(4) 的原论文](https://jcgt.org/published/0007/04/01/)，不是下载其示例代码后遗漏许可证。

构建新 `LtcBake.cpp` 时，除既有 `Source`、`ThirdParty` 包含目录外，需要**私有**包含目录 `ThirdParty/LtcReference/glm/headers`。公共头 `LtcBake.h` 不暴露 GLM 类型。资料获取与固定提交均已实际执行，现有 vendor 可完全离线构建，重烘不需要网络。
