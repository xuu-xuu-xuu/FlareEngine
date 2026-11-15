// W03：蒙皮前向家族（BasicLit / Shadows / Environment 共用 ShadowedForward 语义）。
// 通过 PipelineDescription::macros 传入 SE_SKINNED=1，因此本文件不再 #define，
// 保证 ShaderBuilder 产物与运行时源码键（宏 + 依赖 + 布局）完全一致。
#include "ShadowedForward.hlsl"
