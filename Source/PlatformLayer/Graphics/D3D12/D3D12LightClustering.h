#pragma once
#include "PlatformLayer/Graphics/Rhi.h"
#include "D3D12StateTracker.h"
#include <filesystem>

class D3D12MemoryAllocator;
class Localization;
struct D3D12Presentation;
void PollD3D12ClusterCounters(D3D12Presentation& presentation);
bool ValidD3D12ClusterProjection(const flare::Mat4& projection,float nearPlane,float farPlane);
inline constexpr UINT ClusterCount=16*9*24,ClusterIndexCapacity=65536;
struct ClusterGpuLight {flare::Vec4 positionRange,radiancePad;};
static_assert(sizeof(ClusterGpuLight)==32);
// 显式正确性读回的固定区间；只有indices中requested部分可读，溢出时有效长度为零。
inline constexpr UINT64 ClusterInputOffset=0,ClusterInputBytes=256*sizeof(ClusterGpuLight);
inline constexpr UINT64 ClusterCountsOffset=ClusterInputBytes,ClusterCountsBytes=ClusterCount*sizeof(UINT);
inline constexpr UINT64 ClusterCountStorageBytes=ClusterCountsBytes*9; // 每簇 rawCount 和 8 个 32-bit 候选掩码。
inline constexpr UINT64 ClusterRangesOffset=ClusterCountsOffset+ClusterCountsBytes,ClusterRangesBytes=ClusterCount*2*sizeof(UINT);
inline constexpr UINT64 ClusterIndicesOffset=ClusterRangesOffset+ClusterRangesBytes,ClusterIndicesBytes=ClusterIndexCapacity*sizeof(UINT);
inline constexpr UINT64 ClusterCountersOffset=ClusterIndicesOffset+ClusterIndicesBytes,ClusterCountersBytes=8*sizeof(UINT);
inline constexpr UINT64 ClusterCaptureBytes=ClusterCountersOffset+ClusterCountersBytes;
struct D3D12ClusterCapture {ID3D12Resource* readback=nullptr;};
struct D3D12ClusterOutput
{
    Microsoft::WRL::ComPtr<ID3D12Resource> input,counts,ranges,indices,counters,counterReadback;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptors,graphicsDescriptors;
    UINT increment=0;
    flare::rhi::ClusteredLighting configuration;
    UINT64 pendingFence=0,sourceFrame=0;
    bool countersPending=false;
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT slot) const;
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(UINT slot) const;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE,4> GraphicsViews() const;
};
class D3D12LightClustering
{
public:
    HRESULT Initialize(ID3D12Device& device,std::shared_ptr<D3D12MemoryAllocator> memory,
        const std::filesystem::path& shaders,const Localization& text);
    HRESULT CreateOutput(std::shared_ptr<D3D12ClusterOutput>& output) const;
    HRESULT Prepare(D3D12ClusterOutput& output,const flare::rhi::ClusteredLighting& configuration) const;
    HRESULT Record(ID3D12GraphicsCommandList& commands,D3D12StateRecording& states,
        D3D12ClusterOutput& output,D3D12_GPU_VIRTUAL_ADDRESS constants,const D3D12ClusterCapture* capture=nullptr) const;
private:
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    std::shared_ptr<D3D12MemoryAllocator> memory_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>,4> stages_;
};
