#pragma once
#include "ResourceLayer/Assets/MeshletBake.h"
#include "PlatformLayer/Graphics/Rhi.h"
#include <span>
#ifdef DeviceCapabilities
#undef DeviceCapabilities
#endif

namespace se
{
enum class MeshletSubmissionMode : std::uint8_t
{
    Disabled=0,
    ComputeIndirect,
    MeshShader
};
struct MeshletSubmissionSettings
{
    bool enabled=false;
    bool preferMeshShader=true;
};
// 按设备硬件能力与运行选项选择 Meshlet 提交轨（零 D3D12 依赖）；不支持硬件或显式关闭时自动回退 Compute+ExecuteIndirect。
MeshletSubmissionMode SelectMeshletSubmissionMode(const rhi::DeviceCapabilities& capabilities,
    const MeshletSubmissionSettings& settings) noexcept;
// 一个 cluster 可以对应多个不相邻范围：范围顺序严格等于实际上传源 IB 的三角形顺序。
// GPU 可用每范围的 world AABB 裁剪；首版不启用法线锥，双面/负缩放不影响这一保守测试。
struct MeshletSubmissionRange
{
    std::uint32_t cluster=0,firstIndex=0,indexCount=0;
    Aabb localBounds;
};
struct MeshletSubmission
{
    std::uint32_t sourceVertexCount=0,triangleCount=0,clusterCount=0;
    std::vector<std::uint32_t> indices; // 全局源顶点索引；可直接建立 R32 IB。
    std::vector<MeshletSubmissionRange> ranges;
};
inline constexpr std::uint32_t MaximumMeshletSubmissionRanges=65536;

// 强校验 cluster 的完整有向三角形多重集合与 source 相等，不能以计数相等代替覆盖相等。
// source 必须是实际上传 IB 的顺序（若先做 cache 优化，此处传优化后的 mesh）。
// skinned 输入及超过 command 容量均失败保旧，调用方整桶按原 mesh 回 CPU，不截断范围。
bool FlattenMeshletSubmission(const MeshData& source,const MeshletSet& meshlets,MeshletSubmission& output,
    std::vector<AssetDiagnostic>& diagnostics,std::uint32_t maximumRanges=MaximumMeshletSubmissionRanges);
bool BakeMeshletSubmission(const MeshData& source,MeshletSubmission& output,
    std::vector<AssetDiagnostic>& diagnostics,std::uint32_t maximumRanges=MaximumMeshletSubmissionRanges);
// 只构造保守世界 AABB，不做 CPU 剔除；各范围由同一异构 GPU 命令模块计算可见性。
// 非有限、非仿射、奇异矩阵或非法范围失败保旧，让调用者完整回退。
bool TransformMeshletSubmissionBounds(const MeshletSubmission& source,const Mat4& world,std::vector<Aabb>& output);
}
