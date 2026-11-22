#include "D3D12SceneCoverage.h"
#include "D3D12Presentation.h"
#include "D3D12Shader.h"
#include "D3D12Result.h"

using Microsoft::WRL::ComPtr;
bool D3D12SceneCoverage::Initialize(D3D12Presentation& p,UINT width,UINT height,const std::filesystem::path& shader,const Localization& text)
{
    const auto check=[&](HRESULT hr){return CheckGraphicsResult(hr,text,"D3D12.SceneCoverageFailed");};
    if(!width||!height||width>D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION/2||height>D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION/2)return check(E_INVALIDARG);
    ComPtr<ID3D12Device> device;
    if(!check(p.queue->GetDevice(IID_PPV_ARGS(&device)))||
        !CreateD3D12GuiTarget(p,{width*2,height*2,true,false,flare::rhi::TextureFormat::Rgba16Float},samples,text,nullptr,true))return false;
    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,2,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
    if(!check(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&views))))return false;
    auto cpu=views->GetCPUDescriptorHandleForHeapStart();
    device->CopyDescriptorsSimple(1,cpu,samples.color.views->Cpu(1),heap.Type);
    cpu.ptr+=device->GetDescriptorHandleIncrementSize(heap.Type);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_R32_FLOAT;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
    device->CreateShaderResourceView(samples.depth.Get(),&srv,cpu);
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0};
    D3D12_ROOT_PARAMETER table{};table.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    table.DescriptorTable={1,&range};table.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC signature{1,&table,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob,errors,vs,ps;
    if(!check(D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors))||
        !check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)))||
        !CompileD3D12Shader(shader,"VSMain","vs_5_1",vs,text)||!CompileD3D12Shader(shader,"PSMain","ps_5_1",ps,text))return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};desc.pRootSignature=root.Get();
    desc.VS={vs->GetBufferPointer(),vs->GetBufferSize()};desc.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
    desc.SampleDesc.Count=1;desc.SampleMask=UINT_MAX;desc.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;desc.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;desc.RasterizerState.DepthClipEnable=TRUE;
    desc.NumRenderTargets=1;desc.RTVFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;desc.DSVFormat=DXGI_FORMAT_D32_FLOAT;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.DepthStencilState.DepthEnable=TRUE;desc.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;desc.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
    return check(device->CreateGraphicsPipelineState(&desc,IID_PPV_ARGS(&pipeline)));
}
bool D3D12SceneCoverage::Resolve(D3D12Presentation& p,D3D12StateRecording& states,ID3D12Resource& color,ID3D12Resource& depth,
    D3D12_CPU_DESCRIPTOR_HANDLE rtv,D3D12_CPU_DESCRIPTOR_HANDLE dsv,const Localization& text) const
{
    auto& cmd=*p.commandList.Get();
    const auto use=[&](ID3D12Resource& r,D3D12_RESOURCE_STATES s){return CheckGraphicsResult(states.Use(cmd,r,s,"SceneCoverageResolve"),text,"D3D12.StateTrackingFailed");};
    if(!use(*samples.color.resource.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)||!use(*samples.depth.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
        ||!use(color,D3D12_RESOURCE_STATE_RENDER_TARGET)||!use(depth,D3D12_RESOURCE_STATE_DEPTH_WRITE))return false;
    cmd.SetGraphicsRootSignature(root.Get());cmd.SetPipelineState(pipeline.Get());
    ID3D12DescriptorHeap* heaps[]{views.Get()};cmd.SetDescriptorHeaps(1,heaps);cmd.SetGraphicsRootDescriptorTable(0,views->GetGPUDescriptorHandleForHeapStart());
    const UINT w=samples.color.width/2,h=samples.color.height/2;
    const D3D12_VIEWPORT viewport{0,0,float(w),float(h),0,1};const D3D12_RECT scissor{0,0,LONG(w),LONG(h)};
    cmd.RSSetViewports(1,&viewport);cmd.RSSetScissorRects(1,&scissor);cmd.OMSetRenderTargets(1,&rtv,FALSE,&dsv);
    cmd.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);cmd.DrawInstanced(3,1,0,0);return true;
}
