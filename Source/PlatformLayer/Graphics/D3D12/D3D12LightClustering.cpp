#include "D3D12LightClustering.h"
#include "D3D12Memory.h"
#include "D3D12Shader.h"
#include "D3D12Result.h"
#include "D3D12Presentation.h"
#include <algorithm>
#include <cstring>
#include <cmath>

using Microsoft::WRL::ComPtr;
D3D12_CPU_DESCRIPTOR_HANDLE D3D12ClusterOutput::Cpu(UINT slot) const
{auto handle=descriptors->GetCPUDescriptorHandleForHeapStart();handle.ptr+=SIZE_T(slot)*increment;return handle;}
D3D12_GPU_DESCRIPTOR_HANDLE D3D12ClusterOutput::Gpu(UINT slot) const
{auto handle=descriptors->GetGPUDescriptorHandleForHeapStart();handle.ptr+=UINT64(slot)*increment;return handle;}
std::array<D3D12_CPU_DESCRIPTOR_HANDLE,4> D3D12ClusterOutput::GraphicsViews() const
{
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE,4> result{};auto handle=graphicsDescriptors->GetCPUDescriptorHandleForHeapStart();
    for(auto& entry:result){entry=handle;handle.ptr+=increment;}return result;
}
HRESULT D3D12LightClustering::Initialize(ID3D12Device& device,std::shared_ptr<D3D12MemoryAllocator> memory,
    const std::filesystem::path& shaders,const Localization& text)
{
    if(device_)return E_UNEXPECTED;
    std::array<ComPtr<ID3DBlob>,4> code;const char* entries[]{"CountLights","PrefixLights","FillSparseLights","FillDenseLights"};
    for(UINT i=0;i<4;++i)
        if(!CompileD3D12Shader(shaders/"LightClustering.hlsl",entries[i],"cs_5_1",code[i],text,D3DCOMPILE_IEEE_STRICTNESS))return E_FAIL;
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,9,0,D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
    ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,4,0,0,D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;parameters[0].Descriptor={1,0};
    parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[1].DescriptorTable={1,&ranges[0]};
    parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[2].DescriptorTable={1,&ranges[1]};
    D3D12_ROOT_SIGNATURE_DESC description{};description.NumParameters=3;description.pParameters=parameters;
    ComPtr<ID3DBlob> bytes,error;auto result=D3D12SerializeRootSignature(&description,D3D_ROOT_SIGNATURE_VERSION_1,bytes.GetAddressOf(),error.GetAddressOf());
    if(FAILED(result))return result;
    result=device.CreateRootSignature(0,bytes->GetBufferPointer(),bytes->GetBufferSize(),IID_PPV_ARGS(root_.GetAddressOf()));
    if(FAILED(result))return result;
    for(UINT i=0;i<4;++i)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};pipeline.pRootSignature=root_.Get();pipeline.CS={code[i]->GetBufferPointer(),code[i]->GetBufferSize()};
        result=device.CreateComputePipelineState(&pipeline,IID_PPV_ARGS(stages_[i].GetAddressOf()));if(FAILED(result))return result;
    }
    device_=&device;memory_=std::move(memory);return S_OK;
}
HRESULT D3D12LightClustering::CreateOutput(std::shared_ptr<D3D12ClusterOutput>& output) const
{
    if(!device_||!memory_)return E_UNEXPECTED;
    auto next=std::make_shared<D3D12ClusterOutput>();
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Height=desc.DepthOrArraySize=desc.MipLevels=1;
    desc.SampleDesc={1,0};desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_UPLOAD;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    desc.Width=ClusterInputBytes;
    auto result=device_->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(next->input.GetAddressOf()));
    if(FAILED(result))return result;
    auto states=D3D12StateRegistry::ForDevice(*device_.Get());
    result=states->Register(*next->input.Get(),D3D12_RESOURCE_STATE_GENERIC_READ,D3D12TrackedHeap::Upload,"Cluster.Input");
    if(FAILED(result))return result;
    heap.Type=D3D12_HEAP_TYPE_READBACK;desc.Width=ClusterCountersBytes;
    result=device_->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(next->counterReadback.GetAddressOf()));
    if(FAILED(result))return result;
    result=states->Register(*next->counterReadback.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12TrackedHeap::Readback,"Cluster.CounterReadback");
    if(FAILED(result))return result;
    desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const UINT64 sizes[]{ClusterCountStorageBytes,ClusterRangesBytes,ClusterIndicesBytes,ClusterCountersBytes};
    ComPtr<ID3D12Resource>* resources[]{&next->counts,&next->ranges,&next->indices,&next->counters};
    const char* names[]{"Cluster.Counts","Cluster.Ranges","Cluster.Indices","Cluster.Counters"};
    for(UINT i=0;i<4;++i)
    {desc.Width=sizes[i];result=memory_->Create(desc,D3D12_RESOURCE_STATE_COMMON,nullptr,*resources[i],names[i]);if(FAILED(result))return result;}
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.NumDescriptors=8;hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    result=device_->CreateDescriptorHeap(&hd,IID_PPV_ARGS(next->descriptors.GetAddressOf()));if(FAILED(result))return result;
    next->increment=device_->GetDescriptorHandleIncrementSize(hd.Type);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements=256;srv.Buffer.StructureByteStride=32;device_->CreateShaderResourceView(next->input.Get(),&srv,next->Cpu(0));
    const UINT counts[]{ClusterCount*9,ClusterCount,ClusterIndexCapacity,8},strides[]{4,8,4,4};
    for(UINT i=0;i<4;++i)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;uav.Buffer.NumElements=counts[i];uav.Buffer.StructureByteStride=strides[i];
        device_->CreateUnorderedAccessView(resources[i]->Get(),nullptr,&uav,next->Cpu(1+i));
        if(i>0){srv.Buffer.NumElements=counts[i];srv.Buffer.StructureByteStride=strides[i];device_->CreateShaderResourceView(resources[i]->Get(),&srv,next->Cpu(4+i));}
    }
    // CopyDescriptors 的源必须来自非 shader-visible 堆；图形表复用现有逐帧描述符租约。
    hd.NumDescriptors=4;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    result=device_->CreateDescriptorHeap(&hd,IID_PPV_ARGS(next->graphicsDescriptors.GetAddressOf()));if(FAILED(result))return result;
    const auto views=next->GraphicsViews();
    srv.Buffer.NumElements=256;srv.Buffer.StructureByteStride=32;
    device_->CreateShaderResourceView(next->input.Get(),&srv,views[0]);
    for(UINT i=1;i<4;++i)
    {
        srv.Buffer.NumElements=counts[i];srv.Buffer.StructureByteStride=strides[i];
        device_->CreateShaderResourceView(resources[i]->Get(),&srv,views[i]);
    }
    output=std::move(next);return S_OK;
}
HRESULT D3D12LightClustering::Prepare(D3D12ClusterOutput& output,const se::rhi::ClusteredLighting& configuration) const
{
    if(configuration.lights.size()>256)return E_INVALIDARG;
    std::array<ClusterGpuLight,256> data{};
    for(size_t i=0;i<configuration.lights.size();++i)
    {
        const auto& light=configuration.lights[i];
        data[i]={{light.position.x,light.position.y,light.position.z,light.range},{light.radiance.x,light.radiance.y,light.radiance.z,float(i)}};
    }
    void* mapped=nullptr;const D3D12_RANGE noRead{0,0};auto result=output.input->Map(0,&noRead,&mapped);if(FAILED(result))return result;
    std::memcpy(mapped,data.data(),sizeof(data));const D3D12_RANGE written{0,sizeof(data)};output.input->Unmap(0,&written);
    output.configuration=configuration;return S_OK;
}
HRESULT D3D12LightClustering::Record(ID3D12GraphicsCommandList& commands,D3D12StateRecording& states,
    D3D12ClusterOutput& output,D3D12_GPU_VIRTUAL_ADDRESS constants,const D3D12ClusterCapture* capture) const
{
    auto result=states.Use(commands,*output.input.Get(),D3D12_RESOURCE_STATE_GENERIC_READ,"Cluster.Input");if(FAILED(result))return result;
    ID3D12Resource* buffers[]{output.counts.Get(),output.ranges.Get(),output.indices.Get(),output.counters.Get()};
    for(auto* resource:buffers){result=states.Use(commands,*resource,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,"Cluster.Build");if(FAILED(result))return result;}
    commands.SetComputeRootSignature(root_.Get());commands.SetComputeRootConstantBufferView(0,constants);
    ID3D12DescriptorHeap* heaps[]{output.descriptors.Get()};commands.SetDescriptorHeaps(1,heaps);
    commands.SetComputeRootDescriptorTable(1,output.Gpu(0));commands.SetComputeRootDescriptorTable(2,output.Gpu(1));
    for(UINT stage=0;stage<4;++stage)
    {
        commands.SetPipelineState(stages_[stage].Get());commands.Dispatch(stage==0?ClusterCount:stage==1?1:54,1,1);
        for(auto* resource:buffers){result=states.UavBarrier(commands,*resource,"Cluster.Build");if(FAILED(result))return result;}
    }
    if(capture)
    {
        if(!capture->readback)return E_INVALIDARG;
        result=states.Use(commands,*capture->readback,D3D12_RESOURCE_STATE_COPY_DEST,"Cluster.Capture");if(FAILED(result))return result;
        commands.CopyBufferRegion(capture->readback,ClusterInputOffset,output.input.Get(),0,ClusterInputBytes);
        const UINT64 offsets[]{ClusterCountsOffset,ClusterRangesOffset,ClusterIndicesOffset,ClusterCountersOffset};
        const UINT64 sizes[]{ClusterCountsBytes,ClusterRangesBytes,ClusterIndicesBytes,ClusterCountersBytes};
        for(UINT i=0;i<4;++i)
        {
            result=states.Use(commands,*buffers[i],D3D12_RESOURCE_STATE_COPY_SOURCE,"Cluster.Capture");if(FAILED(result))return result;
            commands.CopyBufferRegion(capture->readback,offsets[i],buffers[i],0,sizes[i]);
        }
    }
    if(output.configuration.counterReadback)
    {
        result=states.Use(commands,*output.counters.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,"Cluster.Counters");if(FAILED(result))return result;
        result=states.Use(commands,*output.counterReadback.Get(),D3D12_RESOURCE_STATE_COPY_DEST,"Cluster.Counters");if(FAILED(result))return result;
        commands.CopyBufferRegion(output.counterReadback.Get(),0,output.counters.Get(),0,ClusterCountersBytes);
    }
    for(auto* resource:{output.ranges.Get(),output.indices.Get(),output.counters.Get()})
    {result=states.Use(commands,*resource,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"Cluster.Sample");if(FAILED(result))return result;}
    return S_OK;
}

void PollD3D12ClusterCounters(D3D12Presentation& presentation)
{
    if(!presentation.fence)return;
    const auto completed=presentation.fence->GetCompletedValue();if(completed==UINT64_MAX)return;
    for(auto& frame:presentation.frames)
    {
        if(!frame.clusters)continue;auto& output=*frame.clusters;
        if(!output.countersPending||!output.pendingFence||output.pendingFence>completed)continue;
        void* mapped=nullptr;const D3D12_RANGE read{0,ClusterCountersBytes};
        if(SUCCEEDED(output.counterReadback->Map(0,&read,&mapped)))
        {
            if(!presentation.clustered.valid||output.sourceFrame>=presentation.clustered.sourceFrame)
            {
                presentation.clustered.valid=true;presentation.clustered.sourceFrame=output.sourceFrame;
                std::memcpy(presentation.clustered.counters.data(),mapped,ClusterCountersBytes);
            }
            const D3D12_RANGE noWrites{0,0};output.counterReadback->Unmap(0,&noWrites);output.countersPending=false;
        }
    }
}

bool ValidD3D12ClusterProjection(const se::Mat4& projection,float nearPlane,float farPlane)
{
    if(!se::IsFinite(projection)||!se::IsFinite(nearPlane)||!se::IsFinite(farPlane)||nearPlane<=0||farPlane<=nearPlane)return false;
    const auto inverse=se::Inverse(projection);if(!inverse)return false;
    // D3D 的 Z=0/1 逆投影端点必须对应真实相机裁面，不能用接近 1 的远端 NDC 容差混入阴影距离。
    const auto nearPoint=*inverse*se::Vec4{0,0,0,1},farPoint=*inverse*se::Vec4{0,0,1,1};
    if(!se::IsFinite(nearPoint)||!se::IsFinite(farPoint)||std::abs(nearPoint.w)<1e-12f||std::abs(farPoint.w)<1e-12f)return false;
    const float actualNear=-nearPoint.z/nearPoint.w,actualFar=-farPoint.z/farPoint.w;
    const float farTolerance=std::clamp((farPlane/nearPlane)*2e-6f,.002f,.05f);
    return se::IsFinite(actualNear)&&se::IsFinite(actualFar)&&std::abs(actualNear-nearPlane)<=nearPlane*.002f
        &&std::abs(actualFar-farPlane)<=farPlane*farTolerance;
}
