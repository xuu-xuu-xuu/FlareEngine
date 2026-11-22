#include "D3D12Mesh.h"
#include "D3D12ShadingExtensions.h"
#include "D3D12Context.h"
#include "D3D12Presentation.h"
#include "D3D12PipelineCache.h"
#include "D3D12Result.h"
#include "D3D12Upload.h"
#include "D3D12Buffer.h"
#include "D3D12Indirect.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Log.h"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <utility>

using Microsoft::WRL::ComPtr;
namespace
{
constexpr UINT64 maxBytes=256ull*1024*1024;
using MeshConstants=flare::rhi::MeshDrawConstants;

bool ValidGeometry(std::span<const D3D12MeshVertex> vertices,std::span<const std::uint32_t> indices)
{
    if(vertices.empty()||indices.empty()||indices.size()%3||vertices.size_bytes()>maxBytes||indices.size_bytes()>maxBytes)return false;
    for(const auto index:indices)if(index>=vertices.size())return false;
    for(const auto& vertex:vertices)
    {
        for(const auto value:vertex.position)if(!flare::IsFinite(value))return false;
        for(const auto value:vertex.normal)if(!flare::IsFinite(value))return false;
        for(const auto value:vertex.uv)if(!flare::IsFinite(value))return false;
        for(const auto value:vertex.tangent)if(!flare::IsFinite(value))return false;
        const float length=flare::LengthSquared(flare::Vec3{vertex.normal[0],vertex.normal[1],vertex.normal[2]});
        if(!flare::IsFinite(length)||length<1e-12f)return false;
    }
    return true;
}
}

bool InitializeD3D12MeshPipeline(D3D12MeshPipeline& output,const D3D12Context& context,
    const std::filesystem::path& shader,const Localization& text)
{
    flare::rhi::PipelineDescription description;description.shader=shader;
    return InitializeD3D12MeshPipeline(output,context,description,text);
}
bool InitializeD3D12MeshPipeline(D3D12MeshPipeline& output,const D3D12Context& context,
    const flare::rhi::PipelineDescription& description,const Localization& text)
{
    if(output.rootSignature||output.pipeline||output.prepared||!context.device||!context.pipelines||description.shader.empty()
        ||(description.kind!=flare::rhi::PipelineKind::TexturedMesh&&!flare::rhi::UsesLitLayout(description.kind)))
    {Log(LogLevel::Error,text.GetText("D3D12.MeshInputsInvalid"));return false;}
    const auto prepared=context.pipelines->Prepare(description,text);if(!prepared)return false;
    output.prepared=prepared;output.rootSignature=prepared->rootSignature;output.pipeline=prepared->pipeline;return true;
}

bool InitializeD3D12Mesh(D3D12Mesh& output,const D3D12Context& context,D3D12Presentation& presentation,
    std::span<const D3D12MeshVertex> vertices,std::span<const std::uint32_t> indices,
    D3D12TexturePixels pixels,const Localization& text)
{
    if(output.vertices||output.indices||output.texture||output.textureViews||!ValidGeometry(vertices,indices)
        ||!ValidD3D12TexturePixels(pixels))
    {Log(LogLevel::Error,text.GetText("D3D12.MeshInputsInvalid"));return false;}
    D3D12Texture texture;
    return InitializeD3D12Texture(texture,context,presentation,pixels,text)
        &&InitializeD3D12Mesh(output,context,presentation,vertices,indices,texture,text);
}

bool InitializeD3D12Mesh(D3D12Mesh& output,const D3D12Context& context,D3D12Presentation& p,
    std::span<const D3D12MeshVertex> vertices,std::span<const std::uint32_t> indices,
    const D3D12Texture& texture,const Localization& text)
{
    if(output.vertices||output.indices||output.texture||output.textureViews||!CanUploadD3D12(context,p)
        ||!ValidGeometry(vertices,indices)||!texture.resource||!texture.views||!texture.width||!texture.height
        ||texture.cube||texture.arrayLayers!=1||texture.format!=flare::rhi::TextureFormat::Rgba8
        ||texture.views->Count()!=2||texture.views->Device()!=context.device.Get())
    {Log(LogLevel::Error,text.GetText("D3D12.MeshInputsInvalid"));return false;}
    ComPtr<ID3D12Device> textureDevice;
    if(FAILED(texture.resource->GetDevice(IID_PPV_ARGS(textureDevice.GetAddressOf())))||textureDevice.Get()!=context.device.Get())
    {Log(LogLevel::Error,text.GetText("D3D12.MeshInputsInvalid"));return false;}
    D3D12Mesh candidate;
    D3D12UploadOperation operation(p,text);if(!operation)return false;
    if(!UploadD3D12Buffer(candidate.vertices,context,p,std::as_bytes(vertices),D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,text)
        ||!UploadD3D12Buffer(candidate.indices,context,p,std::as_bytes(indices),D3D12_RESOURCE_STATE_INDEX_BUFFER,text)
        ||!operation.Finish())return false;
    candidate.vertexView={candidate.vertices->GetGPUVirtualAddress(),static_cast<UINT>(vertices.size_bytes()),sizeof(D3D12MeshVertex)};
    candidate.indexView={candidate.indices->GetGPUVirtualAddress(),static_cast<UINT>(indices.size_bytes()),DXGI_FORMAT_R32_UINT};
    candidate.indexCount=static_cast<UINT>(indices.size());
    candidate.texture=texture.resource;candidate.textureViews=texture.views;
    candidate.textureWidth=texture.width;candidate.textureHeight=texture.height;
    output=std::move(candidate);
    return true;
}

bool ValidateD3D12MeshDraws(const D3D12MeshPipeline& pipeline,std::span<const D3D12MeshDraw> draws,
    const flare::Mat4& viewProjection,const Localization& text,bool allowEmptyInstanced)
{
    const bool defaultForward=flare::rhi::IsForward(pipeline.prepared?pipeline.prepared->program->source.kind:flare::rhi::PipelineKind::TexturedMesh);
    if(!pipeline.rootSignature||!pipeline.pipeline||!pipeline.prepared||pipeline.prepared->colorFormat!=(defaultForward?flare::rhi::ColorFormat::Rgba16Float:flare::rhi::ColorFormat::Rgba8Unorm)
        ||draws.size()>D3D12MeshMaxDraws||!flare::IsFinite(viewProjection))
    {Log(LogLevel::Error,text.GetText("D3D12.MeshDrawsInvalid"));return false;}
    // 同批绘制必须同 pass 类别；Pipeline 可以按材质不同，但类别、颜色格式与实例化模式必须一致。
    const auto defaultKind=pipeline.prepared->program->source.kind;
    const bool defaultShadowed=flare::rhi::IsShadowed(defaultKind);
    const bool defaultEnvironment=flare::rhi::IsEnvironment(defaultKind);
    size_t instanceCount=0;
    for(const auto& draw:draws)
    {
        const auto& active=draw.pipeline?*draw.pipeline:pipeline;
        if(!active.rootSignature||!active.pipeline||!active.prepared
            ||flare::rhi::IsForward(active.prepared->program->source.kind)!=defaultForward
            ||flare::rhi::IsShadowed(active.prepared->program->source.kind)!=defaultShadowed
            ||flare::rhi::IsCascaded(active.prepared->program->source.kind)!=flare::rhi::IsCascaded(defaultKind)
            ||flare::rhi::IsClustered(active.prepared->program->source.kind)!=flare::rhi::IsClustered(defaultKind)
            ||flare::rhi::IsShadowFilterLayout(active.prepared->program->source.layoutVersion)!=flare::rhi::IsShadowFilterLayout(pipeline.prepared->program->source.layoutVersion)
            ||flare::rhi::IsEnvironment(active.prepared->program->source.kind)!=defaultEnvironment
            ||active.prepared->colorFormat!=(defaultForward?flare::rhi::ColorFormat::Rgba16Float:flare::rhi::ColorFormat::Rgba8Unorm))
        {Log(LogLevel::Error,text.GetText("D3D12.MeshDrawsInvalid"));return false;}
        if(active.prepared->alphaMode!=draw.alphaMode||!flare::IsValidMaterialAlphaMode(draw.alphaMode)||!flare::IsFinite(draw.alphaCutoff)||draw.alphaCutoff<0
            ||(draw.alphaMode!=flare::MaterialAlphaMode::Opaque&&active.prepared->program->source.materialLayoutVersion!=flare::rhi::MaterialSurfaceLayoutVersion))
        {Log(LogLevel::Error,text.GetText("D3D12.MeshDrawsInvalid"));return false;}
        const bool instanced=flare::rhi::IsInstanced(active.prepared->program->source.kind);
        const bool instanceDraw=!draw.instances.empty()||(allowEmptyInstanced&&draws.size()==1&&instanced);
        if(instanced!=instanceDraw||draw.instances.size()>flare::rhi::MaximumFrameInstances||instanceCount>flare::rhi::MaximumFrameInstances-draw.instances.size())
        {Log(LogLevel::Error,text.GetText("D3D12.MeshDrawsInvalid"));return false;}
        instanceCount+=draw.instances.size();
        for(const auto& instance:draw.instances)
        {
            const auto& world=instance.world;const auto color=instance.tint;
            if(!flare::IsFinite(world)||!flare::Inverse(world)||!flare::IsFinite(viewProjection*world)||!flare::IsFinite(color)
                ||std::abs(world(3,0))>1e-5f||std::abs(world(3,1))>1e-5f||std::abs(world(3,2))>1e-5f||std::abs(world(3,3)-1)>1e-5f
                ||color.x<0||color.y<0||color.z<0||color.w<0||color.x>1||color.y>1||color.z>1||color.w>1
                ||!std::isfinite(instance.metallic)||instance.metallic<0||instance.metallic>1||!std::isfinite(instance.roughness)||instance.roughness<0||instance.roughness>1
                ||!std::isfinite(instance.normalScale)||instance.normalScale<-8||instance.normalScale>8)
            {Log(LogLevel::Error,text.GetText("D3D12.MeshDrawsInvalid"));return false;}
        }
        const auto* mesh=draw.mesh;
        const auto inverse=flare::IsFinite(draw.world)?flare::Inverse(draw.world):std::nullopt;
        const auto color=draw.tint;
        if(!mesh||!mesh->vertices||!mesh->indices||!mesh->texture||!mesh->textureViews||mesh->textureViews->Count()!=2||!mesh->indexCount
            ||!inverse||!flare::IsFinite(viewProjection*draw.world)||!flare::IsFinite(color)
            ||std::abs(draw.world(3,0))>1e-5f||std::abs(draw.world(3,1))>1e-5f
            ||std::abs(draw.world(3,2))>1e-5f||std::abs(draw.world(3,3)-1)>1e-5f
            ||color.x<0||color.y<0||color.z<0||color.w<0||color.x>1||color.y>1||color.z>1||color.w>1)
        {Log(LogLevel::Error,text.GetText("D3D12.MeshDrawsInvalid"));return false;}
        // W03：蒙皮管线的顶点缓冲必须是 80 字节 v3 布局，且调色板字节数 = 关节数 × 48；
        // 静态管线收到调色板同样视为调用方错误，避免静默丢弃数据。
        const bool skinned=active.prepared->program->source.skinnedVertex;
        const bool skinnedStride=mesh->vertexView.StrideInBytes==sizeof(flare::rhi::SkinnedVertex);
        if(skinned!=skinnedStride)
        {
            Log(LogLevel::Error,"蒙皮校验: pipelineSkinned="+std::to_string(skinned)+" stride="+std::to_string(mesh->vertexView.StrideInBytes)
                +" expected="+std::to_string(sizeof(flare::rhi::SkinnedVertex))+" palette="+std::to_string(draw.skinPalette.size())+" joints="+std::to_string(draw.skinJointCount));
            Log(LogLevel::Error,text.GetText("D3D12.MeshDrawsInvalid"));return false;
        }
        if(skinned)
        {
            if(draw.skinPalette.empty()||!draw.skinJointCount||draw.skinJointOffset>draw.skinPalette.size()
                ||draw.skinPalette.size()-draw.skinJointOffset!=static_cast<std::size_t>(draw.skinJointCount)*48)
            {
                Log(LogLevel::Error,"蒙皮调色板校验: size="+std::to_string(draw.skinPalette.size())+" offset="+std::to_string(draw.skinJointOffset)
                    +" joints="+std::to_string(draw.skinJointCount));
                Log(LogLevel::Error,text.GetText("D3D12.MeshDrawsInvalid"));return false;
            }
        }
        else if(!draw.skinPalette.empty()||draw.skinJointCount)
        {
            Log(LogLevel::Error,"静态绘制带调色板: size="+std::to_string(draw.skinPalette.size())+" joints="+std::to_string(draw.skinJointCount));
            Log(LogLevel::Error,text.GetText("D3D12.MeshDrawsInvalid"));return false;
        }
        if(flare::rhi::UsesLitLayout(active.prepared->program->source.kind))
        {
            const auto& p=draw.pbr;const auto length=flare::LengthSquared(p.directionToLight);
            const auto validTexture=[](const D3D12Texture& t){return t.resource&&t.views&&t.views->Count()==2&&!t.cube&&t.arrayLayers==1
                &&(t.format==flare::rhi::TextureFormat::Rgba8||t.format==flare::rhi::TextureFormat::Bc1Unorm
                    ||t.format==flare::rhi::TextureFormat::Bc1UnormSrgb||t.format==flare::rhi::TextureFormat::Bc5Unorm
                    ||t.format==flare::rhi::TextureFormat::Bc4Unorm||t.format==flare::rhi::TextureFormat::Bc7Unorm||t.format==flare::rhi::TextureFormat::Bc7UnormSrgb);};
            if(draw.textureIsData||!validTexture(mesh->pbrTextures[0])||!validTexture(mesh->pbrTextures[1])
                ||!flare::IsFinite(p.cameraPosition)||!flare::IsFinite(p.directionToLight)||!flare::IsFinite(length)||length<1e-12f||!flare::IsFinite(p.radiance)
                ||p.radiance.x<0||p.radiance.y<0||p.radiance.z<0||p.radiance.x>10000||p.radiance.y>10000||p.radiance.z>10000
                ||!std::isfinite(p.metallic)||p.metallic<0||p.metallic>1||!std::isfinite(p.roughness)||p.roughness<0||p.roughness>1
                ||!std::isfinite(p.normalScale)||p.normalScale<-8||p.normalScale>8)
            {Log(LogLevel::Error,text.GetText("D3D12.MeshDrawsInvalid"));return false;}
        }
    }
    return true;
}

void PrepareD3D12MeshConstants(const D3D12MeshPipeline& pipeline,std::span<const D3D12MeshDraw> draws,
    const flare::Mat4& viewProjection,D3D12FrameContext& frame)
{
    for(size_t index=0;index<draws.size();++index)
    {
        const auto& draw=draws[index];const auto& program=draw.pipeline?*draw.pipeline:pipeline;const auto& slice=frame.drawConstants[index];
        std::memset(slice.cpu,0,static_cast<size_t>(slice.size));
        if(flare::rhi::UsesLitLayout(program.prepared->program->source.kind))
        {
            const auto& p=draw.pbr;
            const flare::Vec3 x{draw.world(0,0),draw.world(1,0),draw.world(2,0)},y{draw.world(0,1),draw.world(1,1),draw.world(2,1)},z{draw.world(0,2),draw.world(1,2),draw.world(2,2)};
            const float sign=flare::Dot(flare::Cross(x,y),z)<0?-1.0f:1.0f;
            const flare::rhi::PbrDrawConstants constants{viewProjection*draw.world,draw.world,flare::Transpose(*flare::Inverse(draw.world)),draw.tint,
                {p.cameraPosition.x,p.cameraPosition.y,p.cameraPosition.z,1},{p.directionToLight.x,p.directionToLight.y,p.directionToLight.z,0},
                {p.radiance.x,p.radiance.y,p.radiance.z,0},{p.metallic,p.roughness,p.normalScale,sign},
                {p.normalMapping?1.0f:0.0f,draw.unlit?1.0f:0.0f,p.doubleSided?1.0f:0.0f,p.lodDither!=0.0f?p.lodDither:(p.receivesShadow?1.0f:0.0f)},
                draw.look,{static_cast<float>(draw.skinJointOffset),static_cast<float>(draw.skinJointCount),p.multiscatter?1.f:0.f,p.specularAA?1.f:0.f}};
            std::memcpy(slice.cpu,&constants,sizeof(constants));
        }
        else
        {
            const MeshConstants constants{viewProjection*draw.world,flare::Transpose(*flare::Inverse(draw.world)),draw.tint,
                {draw.unlit?1.0f:0.0f,draw.impostorView,draw.impostorNext,draw.impostorBlend}};
            std::memcpy(slice.cpu,&constants,sizeof(constants));
        }
    }
}
void RecordD3D12Meshes(const D3D12MeshPipeline& pipeline,std::span<const D3D12MeshDraw> draws,
    const flare::Mat4& viewProjection,D3D12FrameContext& frame,ID3D12GraphicsCommandList& commands,UINT width,UINT height,
    bool cacheBindings,flare::rhi::DrawBindingStatistics* statistics,std::span<const D3D12IndirectCommand> indirect,UINT firstDraw)
{
    (void)viewProjection; // 已在 Prepare 中打包，所有 Pass 只读同一切片。
    flare::rhi::DrawBindingStatistics binds;
    const D3D12_VIEWPORT viewport{0,0,float(width),float(height),0,1};
    const D3D12_RECT scissor{0,0,static_cast<LONG>(width),static_cast<LONG>(height)};
    const D3D12MeshPipeline* activePipeline=nullptr;
    const auto bindPipeline=[&](const D3D12MeshPipeline& program)
    {
        activePipeline=&program;
        commands.SetPipelineState(program.pipeline.Get());++binds.pipelines;
        commands.SetGraphicsRootSignature(program.rootSignature.Get());++binds.rootSignatures;
        if(flare::rhi::IsForward(program.prepared->program->source.kind))
        {commands.SetGraphicsRootConstantBufferView(2,frame.forwardConstants.Gpu());++binds.constants;}
        if(flare::rhi::IsShadowFilterLayout(program.prepared->program->source.layoutVersion))
        {
            const bool cluster=flare::rhi::IsClustered(program.prepared->program->source.kind),skin=program.prepared->program->source.skinnedVertex;
            commands.SetGraphicsRootConstantBufferView(skin?(cluster?6u:5u):(cluster?4u:3u),frame.shadowFilterConstants.Gpu());++binds.constants;
        }
        commands.RSSetViewports(1,&viewport);commands.RSSetScissorRects(1,&scissor);
        commands.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    };
    bindPipeline(pipeline);
    if(!draws.empty()){ID3D12DescriptorHeap* heaps[]{frame.descriptors.Heap()};commands.SetDescriptorHeaps(1,heaps);}
    size_t index=0;bool firstBinding=true;D3D12_GPU_DESCRIPTOR_HANDLE lastTable{};D3D12_VERTEX_BUFFER_VIEW lastVertex{};D3D12_INDEX_BUFFER_VIEW lastIndex{};
    for(const auto& draw:draws)
    {
        // 同帧可混合默认/用户 PBR：只有 Pipeline 变化时重设 PSO 与根签名，并强制重绑根参数。
        const auto& program=draw.pipeline?*draw.pipeline:pipeline;
        if(activePipeline!=&program){bindPipeline(program);firstBinding=true;lastTable={};lastVertex={};lastIndex={};}
        const auto& slice=frame.drawConstants[firstDraw+index++];
        if(program.prepared->materialRootParameter!=UINT_MAX)
        {commands.SetGraphicsRootConstantBufferView(program.prepared->materialRootParameter,frame.materialConstants[firstDraw+index-1].Gpu());++binds.constants;}
        if(program.prepared->shadingFrameParameter!=UINT_MAX)
        {
            commands.SetGraphicsRootConstantBufferView(program.prepared->shadingFrameParameter,frame.shadingConstants.Gpu());
            commands.SetGraphicsRootConstantBufferView(program.prepared->shadingMaterialParameter,frame.shadingMaterials[firstDraw+index-1].Gpu());binds.constants+=2;
            commands.SetGraphicsRootConstantBufferView(program.prepared->localLightParameter,frame.shading->localLightConstants.Gpu());++binds.constants;
            auto table=frame.descriptors.Draw(firstDraw+static_cast<UINT>(index-1));const auto kind=program.prepared->program->source.kind;
            const UINT materialCount=flare::rhi::IsEnvironment(kind)?7:flare::rhi::IsShadowed(kind)?4:3;
            table.ptr+=UINT64(materialCount+(flare::rhi::IsClustered(kind)?4:0))*frame.shadingDescriptorIncrement;
            commands.SetGraphicsRootDescriptorTable(program.prepared->shadingTexturesParameter,table);++binds.descriptorTables;
        }
        if(flare::rhi::IsClustered(program.prepared->program->source.kind))
        {
            auto table=frame.descriptors.Draw(firstDraw+static_cast<UINT>(index-1));
            const auto kind=program.prepared->program->source.kind;
            const UINT materialCount=flare::rhi::IsEnvironment(kind)?7:flare::rhi::IsShadowed(kind)?4:3;
            table.ptr+=UINT64(materialCount)*frame.clusterDescriptorIncrement;
            commands.SetGraphicsRootDescriptorTable(program.prepared->program->source.skinnedVertex?5:3,table);
        }
        commands.SetGraphicsRootConstantBufferView(0,slice.Gpu());++binds.constants;
        frame.constantCursor+=static_cast<UINT>(slice.size);
        const auto table=frame.descriptors.Draw(firstDraw+static_cast<UINT>(index-1));const auto& vertex=draw.mesh->vertexView;const auto& indices=draw.mesh->indexView;
        if(!cacheBindings||firstBinding||table.ptr!=lastTable.ptr){commands.SetGraphicsRootDescriptorTable(1,table);++binds.descriptorTables;lastTable=table;}
        if(!cacheBindings||firstBinding||vertex.BufferLocation!=lastVertex.BufferLocation||vertex.SizeInBytes!=lastVertex.SizeInBytes||vertex.StrideInBytes!=lastVertex.StrideInBytes)
        {commands.IASetVertexBuffers(0,1,&vertex);++binds.vertexBuffers;lastVertex=vertex;}
        if(!cacheBindings||firstBinding||indices.BufferLocation!=lastIndex.BufferLocation||indices.SizeInBytes!=lastIndex.SizeInBytes||indices.Format!=lastIndex.Format)
        {commands.IASetIndexBuffer(&indices);++binds.indexBuffers;lastIndex=indices;}
        firstBinding=false;
        if(!draw.instances.empty()||!indirect.empty())
        {
            // G06.10：Forward 实例化的实例 SRV 在 b1 之后的参数 3；非 Forward 仍在参数 2。
            const UINT instanceParameter=flare::rhi::IsInstanced(program.prepared->program->source.kind)
                &&flare::rhi::IsForward(program.prepared->program->source.kind)?3u:2u;
            commands.SetGraphicsRootShaderResourceView(instanceParameter,frame.instanceSlices[firstDraw+index-1].Gpu());++binds.instanceBuffers;
        }
        // W03：蒙皮管线的调色板在参数 4（Visibility ALL）；字节偏移由 b0 的 skin.x 给出。
        if(program.prepared->program->source.skinnedVertex)
        {
            const auto& skin=frame.skinSlices[firstDraw+index-1];
            if(!skin.resource)return;
            commands.SetGraphicsRootShaderResourceView(4,skin.Gpu());++binds.instanceBuffers;
        }
        if(!indirect.empty())commands.ExecuteIndirect(indirect[index-1].signature,1,indirect[index-1].arguments,0,indirect[index-1].arguments,D3D12Indirect::commandCountOffset);
        else commands.DrawIndexedInstanced(draw.mesh->indexCount,draw.instances.empty()?1u:static_cast<UINT>(draw.instances.size()),0,0,0);
    }
    if(statistics)*statistics=binds;
}
