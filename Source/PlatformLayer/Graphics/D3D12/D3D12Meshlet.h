#pragma once
#include "D3D12Heterogeneous.h"
#include "D3D12Mesh.h"
#include "D3D12Visibility.h"
#include "CoreLayer/Math/Bounds.h"
#include <array>
#include <filesystem>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

class D3D12StateRecording;
struct D3D12Context;
struct D3D12HiZOutput;
class Localization;

// HLSL DXC (SM 6.5) 离线编包入口，供 ShaderBuilder 在构建期调用；运行期不加载 DXC。
bool WriteD3D12MeshletShader(const std::filesystem::path& source, const std::filesystem::path& destination, const Localization& text);

struct D3D12MeshletInput
{
    const D3D12MeshDraw* draw = nullptr;
    D3D12UploadSlice b0;
    flare::Aabb worldBounds;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 7> materialViews{};
    UINT materialViewCount = 0;
    UINT firstIndex = 0, indexCount = 0;
    INT baseVertex = 0;
};

struct D3D12MeshletStatistics
{
    UINT sourceDraws = 0, sourceRanges = 0, gpuRanges = 0, gpuMeshlets = 0;
    UINT cpuDraws = 0, gpuBuckets = 0, cpuBuckets = 0, dispatchCount = 0;
    std::array<UINT, static_cast<size_t>(D3D12HeterogeneousFallback::Count)> reasons{};
};

struct D3D12MeshletDispatchRun
{
    UINT firstMeshlet = 0, meshletCount = 0;
    D3D12_GPU_VIRTUAL_ADDRESS b0 = 0, vertexSrv = 0, indexSrv = 0;
    UINT vertexByteOffset = 0, indexElementOffset = 0;
    ID3D12Resource* vertexResource = nullptr;
    ID3D12Resource* indexResource = nullptr;
};

struct D3D12MeshletSegment
{
    UINT sourceFirst = 0, sourceCount = 0;
    bool cpuFallback = true;
    bool isForward = false;
    flare::rhi::PipelineKind kind = flare::rhi::PipelineKind::PbrMesh;
    bool bc5Normals = false;
    flare::rhi::ColorFormat colorFormat = flare::rhi::ColorFormat::Rgba8Unorm;
    flare::rhi::DepthMode depthMode = flare::rhi::DepthMode::ReadWriteLess;
    D3D12_GPU_DESCRIPTOR_HANDLE materialTable{};
    std::vector<D3D12MeshletDispatchRun> runs;
};

// 每帧槽持有的常驻着色器可见描述符堆，Fence 完成前不复用。
struct D3D12MeshletOutput
{
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptors;
    UINT descriptorCapacity = 0;
};

class D3D12MeshletPrepared
{
public:
    std::span<const D3D12MeshletSegment> Segments() const noexcept { return segments_; }
    const D3D12MeshletStatistics& Stats() const noexcept { return statistics_; }
    bool OcclusionEnabled() const noexcept { return bool(occlusion_); }
    D3D12_GPU_VIRTUAL_ADDRESS CullConstantsGpu() const noexcept { return cullConstants_.Gpu(); }
    D3D12_GPU_VIRTUAL_ADDRESS BoundsGpu() const noexcept { return bounds_.Gpu(); }
    D3D12_GPU_VIRTUAL_ADDRESS ItemsGpu() const noexcept { return items_.Gpu(); }
    D3D12_GPU_DESCRIPTOR_HANDLE HzbTableGpu() const noexcept { return hzbTable_; }

private:
    friend class D3D12Meshlet;
    D3D12UploadSlice bounds_, items_, cullConstants_;
    std::vector<D3D12MeshletSegment> segments_;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> vertexLeases_, indexLeases_, constantLeases_, textureLeases_;
    std::vector<std::shared_ptr<D3D12DescriptorRange>> viewLeases_;
    std::shared_ptr<const D3D12HiZOutput> occlusion_;
    ID3D12DescriptorHeap* descriptorHeap_ = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE hzbTable_{};
    D3D12MeshletStatistics statistics_;
    UINT totalMeshlets_ = 0;
};

// RG104-17.02：硬件 Amplification/Mesh Shader 提交执行器（从预编译 .sebin DXIL 加载，零运行时 DXC）。
class D3D12Meshlet
{
public:
    HRESULT Initialize(D3D12Context& context, const std::filesystem::path& shaderDirectory, const Localization& text);
    bool Initialized() const noexcept { return initialized_; }

    HRESULT Prepare(D3D12UploadArena& arena, D3D12MeshletOutput& output, const D3D12MeshPipeline& defaultPipeline,
        std::span<const D3D12MeshletInput> input, const flare::VisibilityFrustum& frustum,
        D3D12MeshletPrepared& prepared, const D3D12HeterogeneousOcclusion* occlusion, const Localization& text);

    HRESULT RecordBucket(ID3D12GraphicsCommandList& commands, D3D12StateRecording& states,
        const D3D12MeshletPrepared& prepared, D3D12_GPU_VIRTUAL_ADDRESS forwardConstantsGpu,
        UINT width, UINT height, UINT segmentIndex, const Localization& text);

private:
    struct PipelineEntry
    {
        Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    };

    const PipelineEntry* GetOrCreatePipeline(flare::rhi::PipelineKind kind, bool bc5Normals,
        flare::rhi::ColorFormat colorFormat, flare::rhi::DepthMode depthMode, const Localization& text);

    bool initialized_ = false;
    Microsoft::WRL::ComPtr<ID3D12Device2> device2_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> emptyComputeRoot_;
    std::array<std::vector<std::uint8_t>, 1> asBlob_{};
    std::vector<std::uint8_t> msBlob_;
    std::array<std::vector<std::uint8_t>, 6> psBlobs_{}; // Pbr, PbrBc5, Forward, Shadowed, Environment, BasicLit
    std::unordered_map<std::uint64_t, PipelineEntry> pipelines_;
};
