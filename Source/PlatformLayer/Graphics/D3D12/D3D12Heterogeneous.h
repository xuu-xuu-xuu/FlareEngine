#pragma once
#include "D3D12Mesh.h"
#include "D3D12Visibility.h"
#include "CoreLayer/Math/Bounds.h"
#include <array>
#include <cstddef>
#include <memory>
#include <vector>

// RG104-16：命令字段直接使用 D3D12 的公开布局，不截断 GPU 地址。
// b0 为已经打包的实际主绘制常量；只有 InstanceCount 由 compute 改写。
struct D3D12HeterogeneousCommand
{
    D3D12_VERTEX_BUFFER_VIEW vertices{};
    D3D12_INDEX_BUFFER_VIEW indices{};
    D3D12_GPU_VIRTUAL_ADDRESS b0=0;
    D3D12_DRAW_INDEXED_ARGUMENTS draw{};
    UINT padding=0;
};
static_assert(sizeof(D3D12HeterogeneousCommand)==64);
static_assert(offsetof(D3D12HeterogeneousCommand,vertices)==0
    &&offsetof(D3D12HeterogeneousCommand,indices)==16
    &&offsetof(D3D12HeterogeneousCommand,b0)==32
    &&offsetof(D3D12HeterogeneousCommand,draw)==40
    &&offsetof(D3D12HeterogeneousCommand,padding)==60);
static_assert(offsetof(D3D12HeterogeneousCommand,draw)
    +offsetof(D3D12_DRAW_INDEXED_ARGUMENTS,InstanceCount)==44);

struct D3D12HeterogeneousInput
{
    const D3D12MeshDraw* draw=nullptr;
    D3D12UploadSlice b0;
    se::Aabb worldBounds;
    D3D12_GPU_DESCRIPTOR_HANDLE materialTable{};
    // 0 表示使用完整 mesh，且 firstIndex 必须为 0；非 0 可指定 Meshlet 的真实 IB 范围。
    UINT firstIndex=0,indexCount=0;
    // 首版仅接受已验证的源顶点索引，故非零 baseVertex 保守回 CPU。
    INT baseVertex=0;
};
enum class D3D12HeterogeneousFallback : UINT
{
    None,InvalidGeometry,InvalidBounds,InvalidConstants,UnsupportedPipeline,
    Alpha,Instances,Skin,MaterialExtension,Outline,Impostor,Count
};
struct D3D12HeterogeneousStatistics
{
    UINT sourceDraws=0,candidateCommands=0,cpuDraws=0,gpuBuckets=0,cpuBuckets=0;
    UINT unsupportedDraws=0,peerFallbackDraws=0;
    std::array<UINT,static_cast<size_t>(D3D12HeterogeneousFallback::Count)> reasons{};
};
struct D3D12HeterogeneousSegment
{
    UINT sourceFirst=0,sourceCount=0,commandFirst=0,commandCount=0;
    bool cpuFallback=true;
    const D3D12MeshPipeline* pipeline=nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE materialTable{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> signature;
    std::shared_ptr<const D3D12PreparedPipeline> preparedPipeline;
};
struct D3D12HeterogeneousOutput
{
    Microsoft::WRL::ComPtr<ID3D12Resource> commands;
    UINT capacity=0;
};
struct D3D12HiZOutput;
// 只借当前帧完整预绘生成的 HZB；调用者负责在 RecordCull 前构建，禁止前帧替代。
struct D3D12HeterogeneousOcclusion
{
    std::shared_ptr<const D3D12HiZOutput> output;
    se::Mat4 clipFromWorld;
    float depthBias=1e-5f; // 原始0..1深度单位；有效范围[1e-6,.01]，避免零容差自剔。
};
// 必须随 FrameContext 保持到 Fence 完成；上传页不能提前 Reset。
// CPU fallback 的源 Draw 仍由调用者保活、按 segment 的原次序普通录制。
class D3D12HeterogeneousPrepared
{
public:
    std::span<const D3D12HeterogeneousSegment> Segments() const {return segments_;}
    const D3D12HeterogeneousStatistics& Stats() const {return statistics_;}
    bool OcclusionEnabled() const {return bool(occlusion_);}
private:
    friend class D3D12Heterogeneous;
    D3D12UploadSlice inputCommands_,bounds_,constants_;
    std::vector<D3D12HeterogeneousSegment> segments_;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> vertexLeases_,indexLeases_,constantLeases_,textureLeases_;
    std::vector<std::shared_ptr<D3D12DescriptorRange>> viewLeases_;
    D3D12HeterogeneousStatistics statistics_;
    UINT count_=0;
    std::shared_ptr<const D3D12HiZOutput> occlusion_;
};
class D3D12Heterogeneous
{
public:
    HRESULT Initialize(D3D12Context& context,const std::filesystem::path& shader,const Localization& text);
    HRESULT CreateOutput(UINT capacity,D3D12HeterogeneousOutput& output) const;
    // 失败不替换 output。任何不支持输入使所在的整个相邻绑定桶回 CPU，不漏掉桶内其余成员。
    // 容量不足/非法视锥返回失败，调用者应整批回 CPU；不截断、不排序、不进行 CPU 可见性剔除。
    HRESULT Prepare(D3D12UploadArena& arena,const D3D12MeshPipeline& defaultPipeline,
        std::span<const D3D12HeterogeneousInput> input,const se::VisibilityFrustum& frustum,
        UINT capacity,D3D12HeterogeneousPrepared& output,const D3D12HeterogeneousOcclusion* occlusion=nullptr);
    // 固定命令数，不读回可见列表：复制完整命令，然后 GPU 仅改 InstanceCount，最后转换为 INDIRECT_ARGUMENT。
    HRESULT RecordCull(ID3D12GraphicsCommandList& commands,D3D12StateRecording& states,
        const D3D12HeterogeneousPrepared& prepared,const D3D12HeterogeneousOutput& output) const;
    // 调用者先绑定此 segment 的 graphics PSO/root、共同 b1/描述符 heap、viewport/scissor。
    // 本函数绑定材质 table 并实际执行多网格命令。执行后 VB/IB/b0 被 D3D12 清空，旧绑定 cache 必须失效。
    HRESULT RecordBucket(ID3D12GraphicsCommandList& commands,const D3D12HeterogeneousPrepared& prepared,
        const D3D12HeterogeneousOutput& output,UINT segmentIndex) const;
private:
    struct Signature
    {
        Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
        Microsoft::WRL::ComPtr<ID3D12CommandSignature> command;
    };
    HRESULT GetSignature(ID3D12RootSignature& root,Microsoft::WRL::ComPtr<ID3D12CommandSignature>& output);
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> computeRoot_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> cull_;
    std::shared_ptr<D3D12MemoryAllocator> memory_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> nullOcclusionHeap_;
    std::vector<Signature> signatures_;
};
