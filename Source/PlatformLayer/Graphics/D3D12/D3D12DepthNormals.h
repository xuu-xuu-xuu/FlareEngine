#pragma once
#include "D3D12Mesh.h"
#include "D3D12ShaderProgram.h"
#include <unordered_map>
struct D3D12FrameContent;
class D3D12StateRecording;
struct D3D12DepthNormalTarget
{
    Microsoft::WRL::ComPtr<ID3D12Resource> depth,normal,material;
    std::shared_ptr<D3D12DescriptorRange> dsv,rtv,views;
    UINT width=0,height=0;
    UINT64 allocatedBytes=0;
};
struct D3D12DepthNormalPipeline
{
    D3D12MeshPipeline mesh;
    std::shared_ptr<const D3D12PreparedPipeline> main;
    std::shared_ptr<const D3D12ShaderProgram> pixel;
};
struct D3D12DepthNormalFrame
{
    std::shared_ptr<D3D12DepthNormalTarget> target;
    std::vector<std::shared_ptr<const D3D12DepthNormalPipeline>> pipelines;
    flare::rhi::DepthNormalStatistics statistics;
};
class D3D12DepthNormals
{
public:
    bool Prepare(D3D12Presentation& presentation,D3D12FrameContext& frame,const D3D12FrameContent& content,
        UINT width,UINT height,const Localization& text);
    bool Record(D3D12Presentation& presentation,D3D12FrameContext& frame,const D3D12FrameContent& content,
        D3D12StateRecording& states,const Localization& text) const;
    D3D12ShaderLoadStatistics ShaderStatistics() const{return shaderStatistics_;}
    UINT64 PipelineCreations() const{return pipelineCreations_;}
private:
    std::array<std::shared_ptr<const D3D12ShaderProgram>,4> pixels_;
    std::array<Microsoft::WRL::ComPtr<ID3DBlob>,4> indoorPixels_;
    std::unordered_map<std::string,Microsoft::WRL::ComPtr<ID3DBlob>> userPixels_;
    D3D12ShaderLoadStatistics shaderStatistics_;
    UINT64 pipelineCreations_=0;
    std::unordered_map<std::string,std::weak_ptr<const D3D12DepthNormalPipeline>> pipelines_;
};
