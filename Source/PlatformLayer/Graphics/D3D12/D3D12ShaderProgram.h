#pragma once
#include "PlatformLayer/Graphics/Rhi.h"
#include <d3dcompiler.h>
#include <wrl/client.h>

class Localization;
struct D3D12ShaderDependency {std::string path,hash;};
struct D3D12ShaderSource
{
    std::filesystem::path path;
    flare::rhi::PipelineKind kind=flare::rhi::PipelineKind::TexturedMesh;
    UINT flags=0;
    UINT layoutVersion=flare::rhi::MaterialLayoutVersion;
    UINT materialLayoutVersion=0;
    UINT shadingLayoutVersion=0;
    bool depthNormals=false; // 39 号输出契约；独立于 b4 材质与既有绑定布局。
    bool compressedVertex=false;
    // W03：顶点布局 v3（JOINTS0/WEIGHTS0）与 t8 调色板；由 vertexLayoutVersion 推导，
    // 参与反射校验但不进入元数据（布局版本与宏已在键里）。
    bool skinnedVertex=false;
    std::vector<flare::rhi::ShaderMacro> macros;
    std::vector<D3D12ShaderDependency> dependencies;
    std::string key,preprocessed;
};
struct D3D12ShaderProgram
{
    D3D12ShaderSource source;
    Microsoft::WRL::ComPtr<ID3DBlob> vertex,pixel;
    std::string bytecodeKey;
};
struct D3D12ShaderLoadStatistics {UINT64 artifactLoads=0,artifactRejections=0,compilations=0;};
UINT D3D12ShaderCompileFlags();
bool ReadD3D12ShaderSource(const flare::rhi::PipelineDescription& description,D3D12ShaderSource& output,const Localization& text);
std::shared_ptr<const D3D12ShaderProgram> CompileD3D12ShaderProgram(D3D12ShaderSource source,const Localization& text);
std::shared_ptr<const D3D12ShaderProgram> LoadD3D12ShaderProgram(D3D12ShaderSource source,D3D12ShaderLoadStatistics& stats,const Localization& text);
bool WriteD3D12ShaderProgram(const D3D12ShaderProgram& program,const std::filesystem::path& artifact,const Localization& text);
// 固定表面接口包：保留旧 PBR v1，并支持 Forward/Shadowed/Environment 的 HDR v2。
// 各通道独立单文件原子发布；保留反射，去除源码调试/私有/内嵌 Root Signature 块。
std::string D3D12UserPbrInterfaceDescription();
std::string D3D12UserPbrInterfaceHash();
bool WriteD3D12UserPbrPackage(const D3D12ShaderProgram& program,const std::filesystem::path& artifact,const Localization& text);
std::shared_ptr<const D3D12ShaderProgram> LoadD3D12UserPbrPackage(const flare::rhi::PipelineDescription& description,
    D3D12ShaderLoadStatistics& stats,const Localization& text);
