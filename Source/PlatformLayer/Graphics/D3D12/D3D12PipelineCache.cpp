#include "D3D12PipelineCache.h"
#include "D3D12Descriptors.h"
#include "D3D12PipelineLibraryStorage.h"
#include "D3D12Triangle.h"
#include "D3D12Result.h"
#include "PlatformLayer/Graphics/MaterialLayout.h"
#include "CoreLayer/Hash.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Log.h"
#include <d3d12sdklayers.h>
#include <algorithm>
#include <thread>
#include <unordered_map>
#include <vector>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace rhi=flare::rhi;
using Microsoft::WRL::ComPtr;
struct D3D12PipelineCache::Impl
{
    ID3D12Device* device;
    const std::thread::id owner=std::this_thread::get_id();
    std::unordered_map<std::string,std::weak_ptr<const D3D12ShaderProgram>> programs;
    std::unordered_map<std::string,std::weak_ptr<const D3D12PreparedPipeline>> pipelines;
    std::vector<std::weak_ptr<const D3D12PreparedPipeline>> livePipelines;
    D3D12PipelineCacheStatistics stats;
    ComPtr<ID3D12Device1> device1;
    ComPtr<ID3D12PipelineLibrary> library;
    std::filesystem::path libraryPath;
    bool libraryDirty=false;
    explicit Impl(ID3D12Device& value):device(&value){}
};
D3D12PipelineCache::D3D12PipelineCache(ID3D12Device& device):impl_(std::make_unique<Impl>(device))
{
#ifndef NDEBUG
    return; // Debug 层与 PSO 库同时启用会产生校验告警；持久库只在 Release 生效。
#endif
    auto& p=*impl_;
    // 验证程序也可能在 Release 进程中显式启用调试层；按实际设备判断，
    // 避免把持久库未命中的正常回退记录为 GPU 调试错误。
    ComPtr<ID3D12InfoQueue> debugQueue;
    if(SUCCEEDED(device.QueryInterface(IID_PPV_ARGS(debugQueue.GetAddressOf()))))return;
    if(FAILED(device.QueryInterface(IID_PPV_ARGS(p.device1.GetAddressOf()))))return;
    std::error_code error;
    const auto directory=std::filesystem::current_path(error)/".cache"/"pso";
    if(error)return;
    std::filesystem::create_directories(directory,error);
    if(error)return;
    const auto luid=device.GetAdapterLuid();
    // G15.09：库文件名带格式版本，布局变更（如 29 号输出 LUT）后提升版本，使旧条目失效。
    // G02.04：根签名序列化哈希已进入 PSO 键；v3 同时让旧 v2 库整体失效，避免历史条目混用。
    // W06：逐绘制常量新增外观覆盖/蒙皮槽（PbrDrawConstants 288→320），旧库条目不再兼容，提升到 v4。
    // W03：根签名扩到 5 个参数（t8 调色板）、v3 顶点布局、31 号绑定布局与 Skinning.hlsli 进入
    // 着色器依赖；旧 v4 库条目的键与本版本不同，提升到 v5 直接作废旧库。
    static constexpr const char* libraryVersion="v6";
    std::ostringstream name;name<<"library-"<<libraryVersion<<"-"<<std::hex<<luid.HighPart<<"-"<<luid.LowPart<<".bin";
    p.libraryPath=directory/name.str();
    if(std::filesystem::exists(p.libraryPath,error)&&!error)
    {
        std::ifstream input(p.libraryPath,std::ios::binary);
        auto blob=std::make_shared<std::vector<char>>(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>());
        if(!blob->empty()&&SUCCEEDED(p.device1->CreatePipelineLibrary(blob->data(),blob->size(),IID_PPV_ARGS(p.library.GetAddressOf()))))
        {
            if(SUCCEEDED(RetainD3D12PipelineLibraryStorage(*p.library.Get(),blob)))p.stats.libraryLoaded=true;
            else p.library.Reset(); // 缓冲仍活着时先释放库，随后回退空库。
        }
    }
    if(!p.library)p.device1->CreatePipelineLibrary(nullptr,0,IID_PPV_ARGS(p.library.GetAddressOf()));
}
D3D12PipelineCache::~D3D12PipelineCache()
{
    LogCheckpoint("GPU.PipelineCache.Release.Begin");
    struct Finish{std::unique_ptr<Impl>& value;~Finish(){value.reset();LogCheckpoint("GPU.PipelineCache.Release.End");}} finish{impl_};
#ifndef NDEBUG
    return;
#endif
    auto& p=*impl_;
    // 从磁盘加载的库再次 Serialize 在本机驱动上会访问越界（首个从空库建立的进程正常）。
    // 交付包首次运行写盘、后续运行只读；这样既保留热身收益，也避免退出时崩溃。
    if(!p.library||!p.libraryDirty||p.libraryPath.empty()||p.stats.libraryLoaded)return;
    try
    {
        LogCheckpoint("GPU.PipelineCache.Serialize.Begin");
        struct Serialized{~Serialized(){LogCheckpoint("GPU.PipelineCache.Serialize.End");}} serialized;
        const SIZE_T size=p.library->GetSerializedSize();
        if(!size)return;
        std::vector<char> blob(size);
        if(FAILED(p.library->Serialize(blob.data(),blob.size())))return;
        std::ofstream output(p.libraryPath,std::ios::binary|std::ios::trunc);
        output.write(blob.data(),static_cast<std::streamsize>(blob.size()));
    }
    catch(...){}
}
std::shared_ptr<const D3D12PreparedPipeline> D3D12PipelineCache::Prepare(const rhi::PipelineDescription& description,const Localization& text)
{
    auto& p=*impl_;if(p.owner!=std::this_thread::get_id()||!rhi::ValidMaterialSurfacePipeline(description)||!rhi::ValidShadingExtensionPipeline(description)
        ||std::any_of(description.macros.begin(),description.macros.end(),[](const auto& macro){return macro.name=="SE_DEPTH_NORMALS";}))return {};
    // W03：31 号蒙皮布局只对可蒙皮技术开放，且必须与 v3 顶点布局成对出现。
    const bool filterLayout=rhi::ValidShadowFilterBindingLayout(description.kind,description.bindingLayoutVersion,description.vertexLayoutVersion==rhi::VertexLayoutVersionSkinned);
    const bool skinnedLayout=((description.bindingLayoutVersion==(rhi::IsClustered(description.kind)?rhi::SkinnedClusteredLayoutVersion:rhi::IsCascaded(description.kind)?rhi::SkinnedCascadedLayoutVersion:rhi::SkinnedLayoutVersion))
        ||(filterLayout&&description.vertexLayoutVersion==rhi::VertexLayoutVersionSkinned))&&rhi::SupportsSkinning(description.kind);
    const bool skinnedVertex=description.vertexLayoutVersion==rhi::VertexLayoutVersionSkinned;
    if((description.bindingLayoutVersion!=rhi::BindingLayout(description.kind)
            &&!(rhi::IsOutput(description.kind)&&description.bindingLayoutVersion==rhi::OutputLutLayoutVersion)
            &&!skinnedLayout&&!filterLayout)
        ||skinnedLayout!=skinnedVertex
        ||!rhi::ValidPipelineKind(description.kind)
        ||(description.shaderInput!=rhi::ShaderInput::Source&&description.shaderInput!=rhi::ShaderInput::Artifact)
        ||(description.depth!=rhi::DepthMode::Default&&description.depth!=rhi::DepthMode::Disabled&&description.depth!=rhi::DepthMode::ReadWriteLess&&description.depth!=rhi::DepthMode::ReadOnlyLessEqual&&description.depth!=rhi::DepthMode::ReadOnlyEqual&&description.depth!=rhi::DepthMode::ReadOnlyGreater)
        ||(description.blend!=rhi::BlendMode::Opaque&&description.blend!=rhi::BlendMode::Alpha&&description.blend!=rhi::BlendMode::Additive)
        ||(description.colorFormat!=rhi::ColorFormat::Rgba8Unorm&&description.colorFormat!=rhi::ColorFormat::Rgba16Float&&description.colorFormat!=rhi::ColorFormat::Rgba8Srgb)
        ||(description.vertexLayoutVersion!=rhi::VertexLayoutVersionLegacy&&description.vertexLayoutVersion!=rhi::VertexLayoutVersionCompressed
            &&description.vertexLayoutVersion!=rhi::VertexLayoutVersionSkinned))return {};
    std::erase_if(p.programs,[](const auto& item){return item.second.expired();});std::erase_if(p.pipelines,[](const auto& item){return item.second.expired();});
    std::erase_if(p.livePipelines,[](const auto& item){return item.expired();});
    std::shared_ptr<const D3D12ShaderProgram> program;
    if(description.shaderInput==rhi::ShaderInput::Artifact)
    {
        // 每次准备先验证当前文件；缓存不能掩盖被替换/损坏的产物，普通Draw不走这里。
        const auto candidate=LoadD3D12UserPbrPackage(description,p.stats.shaders,text);if(!candidate)return {};
        const auto programKey="artifact:"+candidate->source.key+":"+candidate->bytecodeKey;
        const auto found=p.programs.find(programKey);if(found!=p.programs.end())program=found->second.lock();
        if(program)++p.stats.programHits;
        else {program=candidate;p.programs.insert_or_assign(programKey,program);}
    }
    else
    {
        D3D12ShaderSource source;if(!ReadD3D12ShaderSource(description,source,text))return {};
        const auto foundProgram=p.programs.find(source.key);if(foundProgram!=p.programs.end())program=foundProgram->second.lock();
        if(program)++p.stats.programHits;
        else
        {
            program=LoadD3D12ShaderProgram(std::move(source),p.stats.shaders,text);if(!program)return {};
            p.programs.insert_or_assign(program->source.key,program);
        }
    }
    const bool pbr=rhi::UsesLitLayout(description.kind),forward=rhi::IsForward(description.kind),output=rhi::IsOutput(description.kind),shadow=rhi::IsShadowed(description.kind),depthOnly=description.kind==rhi::PipelineKind::ShadowDepth;
    const bool environment=rhi::IsEnvironment(description.kind),environmentSky=description.kind==rhi::PipelineKind::EnvironmentSky;
    const bool post=rhi::IsPostProcess(description.kind),overlay=description.kind==rhi::PipelineKind::UiOverlay;
    const bool instanced=rhi::IsInstanced(description.kind);
    // G06.10：Forward 实例化同时需要 b1 前向常量与 t7 实例 SRV，根签名扩到 4 个参数。
    const bool forwardInstanced=description.kind==rhi::PipelineKind::PbrForwardInstancedMesh;
    const bool gui=description.kind==rhi::PipelineKind::Gui;
    const bool fullscreen=output||rhi::IsSky(description.kind)||post;
    const bool mesh=pbr||description.kind==rhi::PipelineKind::TexturedMesh;
    const bool compressed=description.vertexLayoutVersion==rhi::VertexLayoutVersionCompressed;
    // W03：蒙皮管线在受光家族追加参数 4 的 t8 调色板 SRV；阴影深度单独用 b0 常量 + 参数 1 的 t8。
    const bool skinned=skinnedLayout&&skinnedVertex;
    const bool surface=rhi::IsMaterialSurfaceLayout(description.materialLayoutVersion),surfaceDepth=surface&&depthOnly;
    const auto depth=description.depth==rhi::DepthMode::Default?(mesh||depthOnly?rhi::DepthMode::ReadWriteLess:rhi::DepthMode::Disabled):description.depth;
    // G02.04：先构建根签名描述并序列化，再把序列化字节的 SHA-256 纳入 PSO 键。
    // 只靠 kind/layout/格式无法区分根参数/描述符表/静态采样器不同的技术；旧键会让
    // 后创建者覆盖同键条目，下一次创建者加载到根签名不匹配的 PSO。
    D3D12_DESCRIPTOR_RANGE range{};range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;range.NumDescriptors=environment?7:shadow?4:(description.kind==rhi::PipelineKind::ShadowOutput||description.kind==rhi::PipelineKind::CascadedShadowOutput)?2:pbr?3:1;range.OffsetInDescriptorsFromTableStart=D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    // G15：输出 LUT 变体在 t2 追加 strip，根签名表固定扩到 t0..t2 三槽：
    // ShadowOutput 为场景/阴影预览/LUT，FinalOutput 的 t1 不读取但仍占位以保持寄存器连续。
    if(rhi::IsOutput(description.kind)&&description.bindingLayoutVersion==rhi::OutputLutLayoutVersion)
        range.NumDescriptors=3;
    if(post)range.NumDescriptors=description.bindingLayoutVersion==rhi::ToneMapLayoutVersion?4u:2u;
    D3D12_DESCRIPTOR_RANGE clusterRange{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,4,9,0,D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
    D3D12_ROOT_PARAMETER parameters[16]{};
    parameters[0].ParameterType=mesh?D3D12_ROOT_PARAMETER_TYPE_CBV:D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].ShaderVisibility=mesh||environmentSky||overlay?D3D12_SHADER_VISIBILITY_ALL:fullscreen?D3D12_SHADER_VISIBILITY_PIXEL:D3D12_SHADER_VISIBILITY_VERTEX;
    if(!mesh)parameters[0].Constants={0,0,description.kind==rhi::PipelineKind::Sky?44u:post?12u:environmentSky?20u:fullscreen?4u:16u};
    if(gui)parameters[0].Constants={0,0,4};
    const bool userSurf=(description.materialLayoutVersion==rhi::UserSurfaceMaterialLayoutVersion);
    parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[1].DescriptorTable={1,&range};parameters[1].ShaderVisibility=userSurf?D3D12_SHADER_VISIBILITY_ALL:D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;parameters[2].Descriptor={1,0};parameters[2].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    if(instanced&&!forwardInstanced){parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[2].Descriptor={7,0};parameters[2].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;}
    if(forwardInstanced){parameters[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[3].Descriptor={7,0};parameters[3].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;}
    // W03：受光家族把调色板放在新参数 4（ALL）；阴影深度没有 b1/贴图表，调色板直接占参数 1。
    if(skinned)
    {
        const UINT bonesParameter=mesh?4u:1u;
        parameters[bonesParameter].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameters[bonesParameter].Descriptor={8,0};
        parameters[bonesParameter].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=description.anisotropicSampling?D3D12_FILTER_ANISOTROPIC:D3D12_FILTER_MIN_MAG_MIP_POINT;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxAnisotropy=description.anisotropicSampling?16u:1u;sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;sampler.BorderColor=D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    sampler.MaxLOD=D3D12_FLOAT32_MAX;sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC samplers[]{sampler,sampler,sampler};
    if(userSurf)samplers[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    samplers[1].ShaderRegister=1;samplers[1].Filter=D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;samplers[1].ComparisonFunc=D3D12_COMPARISON_FUNC_LESS_EQUAL;
    samplers[1].AddressU=samplers[1].AddressV=samplers[1].AddressW=D3D12_TEXTURE_ADDRESS_MODE_BORDER;samplers[1].BorderColor=D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    samplers[2].ShaderRegister=2;samplers[2].Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[2].AddressU=samplers[2].AddressV=samplers[2].AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    if(post){samplers[0]=samplers[2];samplers[0].ShaderRegister=0;}
    D3D12_ROOT_SIGNATURE_DESC root{};root.NumParameters=forwardInstanced?4u:instanced?3u:overlay?1u:post?2u:forward?3u:mesh||output||environmentSky?2u:1u;root.pParameters=parameters;
    if(skinned)root.NumParameters=mesh?5u:2u;
    if(rhi::IsClustered(description.kind))
    {
        const UINT slot=skinned?5u:3u;parameters[slot].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[slot].DescriptorTable={1,&clusterRange};parameters[slot].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        root.NumParameters=slot+1;
    }
    if(filterLayout)
    {
        const UINT slot=skinned?(rhi::IsClustered(description.kind)?6u:5u):(rhi::IsClustered(description.kind)?4u:3u);
        parameters[slot].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;parameters[slot].Descriptor={3,0};
        parameters[slot].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;root.NumParameters=slot+1;
    }
    UINT materialRootParameter=UINT_MAX;
    if(surfaceDepth)
    {
        const UINT table=skinned?2u:1u;parameters[table].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[table].DescriptorTable={1,&range};parameters[table].ShaderVisibility=userSurf?D3D12_SHADER_VISIBILITY_ALL:D3D12_SHADER_VISIBILITY_PIXEL;root.NumParameters=table+1;
    }
    if(surface)
    {
        materialRootParameter=root.NumParameters++;parameters[materialRootParameter].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[materialRootParameter].Descriptor={4,0};
        parameters[materialRootParameter].ShaderVisibility=description.materialLayoutVersion==rhi::UserSurfaceMaterialLayoutVersion
            ?D3D12_SHADER_VISIBILITY_ALL:D3D12_SHADER_VISIBILITY_PIXEL;
    }
    UINT shadingFrameParameter=UINT_MAX,shadingMaterialParameter=UINT_MAX,shadingTexturesParameter=UINT_MAX;
    UINT localLightParameter=UINT_MAX;
    D3D12_DESCRIPTOR_RANGE shadingRange{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,D3D12ShadingTextureCount,13,0,D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
    if(description.shadingLayoutVersion)
    {
        shadingFrameParameter=root.NumParameters++;shadingMaterialParameter=root.NumParameters++;shadingTexturesParameter=root.NumParameters++;
        parameters[shadingFrameParameter].ParameterType=parameters[shadingMaterialParameter].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[shadingFrameParameter].Descriptor={5,0};parameters[shadingMaterialParameter].Descriptor={6,0};
        parameters[shadingFrameParameter].ShaderVisibility=parameters[shadingMaterialParameter].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[shadingTexturesParameter].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[shadingTexturesParameter].DescriptorTable={1,&shadingRange};parameters[shadingTexturesParameter].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        localLightParameter=root.NumParameters++;
        parameters[localLightParameter].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[localLightParameter].Descriptor={7,0};parameters[localLightParameter].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    }
    // 蒙皮材质管线把参数 3 留给实例 SRV，但非实例化时它是空表；补一个不冲突的 t7 单槽表，
    // 避免调试层报“空根描述符表”。该寄存器不在任何着色器里读取。
    // 注意：不能用参数 1 的 range（t0..t2），两段描述符范围重叠会让根签名序列化返回 E_INVALIDARG。
    D3D12_DESCRIPTOR_RANGE spareRange{};
    spareRange.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;spareRange.NumDescriptors=1;spareRange.BaseShaderRegister=7;
    spareRange.RegisterSpace=0;spareRange.OffsetInDescriptorsFromTableStart=D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    if(skinned&&mesh)
    {
        parameters[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[3].DescriptorTable={1,&spareRange};
        parameters[3].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    }
    root.NumStaticSamplers=environment?3u:shadow?2u:mesh||environmentSky?1u:0u;root.pStaticSamplers=environmentSky?&samplers[2]:mesh?samplers:nullptr;root.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    if(surfaceDepth){root.NumStaticSamplers=1;root.pStaticSamplers=samplers;}
    if(post){root.NumStaticSamplers=description.kind==rhi::PipelineKind::ToneMap||description.kind==rhi::PipelineKind::Fxaa?1u:0u;root.pStaticSamplers=samplers;}
    if(gui){root.NumParameters=2;root.NumStaticSamplers=1;samplers[0]=samplers[2];samplers[0].ShaderRegister=0;root.pStaticSamplers=samplers;}
    ComPtr<ID3DBlob> serialized,diagnostics;
    if(!CheckGraphicsResult(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,serialized.GetAddressOf(),diagnostics.GetAddressOf()),text,"D3D12.SerializeRootSignatureFailed"))return {};
    const auto rootHash=flare::Sha256(std::span<const std::uint8_t>(
        static_cast<const std::uint8_t*>(serialized->GetBufferPointer()),serialized->GetBufferSize()));
    if(rootHash.empty())return {};
    // 键已包含着色器源码键（= 宏 + 依赖 + 布局的 SHA-256）、绑定布局与顶点布局版本，
    // 因此蒙皮/非蒙皮、压缩/参考顶点、不同 SE_* 宏组合不会共用同一条目。
    const auto key=flare::Sha256(program->source.key+":"+program->bytecodeKey+":"+std::to_string(static_cast<int>(description.kind))
        +":"+std::to_string(description.bindingLayoutVersion)+":"+std::to_string(description.vertexLayoutVersion)+":"+std::to_string(static_cast<int>(depth))+":"+std::to_string(static_cast<int>(description.blend))
        +":"+std::to_string(static_cast<int>(description.colorFormat))+":root:"+rootHash+":triangle-list:cull-none:one-sample:d32-if-depth:v1"
        +(surface?":material:"+std::to_string(description.materialLayoutVersion)+":alpha:"+std::to_string(static_cast<unsigned>(description.alphaMode)):std::string{})
        +(description.shadingLayoutVersion?":shading:"+std::to_string(description.shadingLayoutVersion):std::string{}));
    if(key.empty())return {};
    const std::wstring libraryName(key.begin(),key.end());
    if(description.cachePrepared){const auto found=p.pipelines.find(key);if(found!=p.pipelines.end())if(auto cached=found->second.lock()){++p.stats.pipelineHits;return cached;}}
    ++p.stats.pipelineMisses;
    auto prepared=std::make_shared<D3D12PreparedPipeline>();prepared->program=std::move(program);prepared->key=key;
    prepared->materialRootParameter=materialRootParameter;prepared->alphaMode=description.alphaMode;
    prepared->shadingFrameParameter=shadingFrameParameter;prepared->shadingMaterialParameter=shadingMaterialParameter;prepared->shadingTexturesParameter=shadingTexturesParameter;
    prepared->localLightParameter=localLightParameter;
    prepared->depth=depth;prepared->blend=description.blend;prepared->colorFormat=description.colorFormat;
    if(!CheckGraphicsResult(p.device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(prepared->rootSignature.GetAddressOf())),text,"D3D12.CreateRootSignatureFailed"))return {};
    ++p.stats.rootCreations;
    const D3D12_INPUT_ELEMENT_DESC meshInputs[]{
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,offsetof(rhi::Vertex,position),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,offsetof(rhi::Vertex,normal),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,offsetof(rhi::Vertex,uv),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TANGENT",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,offsetof(rhi::Vertex,tangent),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
    const D3D12_INPUT_ELEMENT_DESC compressedInputs[]{
        {"POSITION",0,DXGI_FORMAT_R16G16B16A16_UNORM,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"NORMAL",0,DXGI_FORMAT_R8G8_UNORM,0,6,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R16G16_FLOAT,0,8,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TANGENT",0,DXGI_FORMAT_R8G8B8A8_UNORM,0,12,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
    // W03：v3 蒙皮顶点（80 字节）。JOINTS0 在 48、WEIGHTS0 在 64，偏移与 rhi::SkinnedVertex 一致。
    const D3D12_INPUT_ELEMENT_DESC skinnedInputs[]{
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,offsetof(rhi::SkinnedVertex,base.position),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,offsetof(rhi::SkinnedVertex,base.normal),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,offsetof(rhi::SkinnedVertex,base.uv),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TANGENT",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,offsetof(rhi::SkinnedVertex,base.tangent),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"JOINTS",0,DXGI_FORMAT_R16G16B16A16_UINT,0,offsetof(rhi::SkinnedVertex,joints),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"WEIGHTS",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,offsetof(rhi::SkinnedVertex,weights),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
    const D3D12_INPUT_ELEMENT_DESC skinnedDepthInputs[]{
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,offsetof(rhi::SkinnedVertex,base.position),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"JOINTS",0,DXGI_FORMAT_R16G16B16A16_UINT,0,offsetof(rhi::SkinnedVertex,joints),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"WEIGHTS",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,offsetof(rhi::SkinnedVertex,weights),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
    const D3D12_INPUT_ELEMENT_DESC triangleInputs[]{
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,offsetof(D3D12TriangleVertex,position),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,offsetof(D3D12TriangleVertex,color),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
    const D3D12_INPUT_ELEMENT_DESC guiInputs[]{
        {"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,offsetof(flare::GuiVertex,position),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,offsetof(flare::GuiVertex,uv),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"COLOR",0,DXGI_FORMAT_R8G8B8A8_UNORM,0,offsetof(flare::GuiVertex,color),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
    const D3D12_INPUT_ELEMENT_DESC surfaceDepthInputs[]{
        {"POSITION",0,compressed?DXGI_FORMAT_R16G16B16A16_UNORM:DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,compressed?DXGI_FORMAT_R16G16_FLOAT:DXGI_FORMAT_R32G32_FLOAT,0,compressed?8u:24u,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"JOINTS",0,DXGI_FORMAT_R16G16B16A16_UINT,0,48,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"WEIGHTS",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,64,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};pipeline.pRootSignature=prepared->rootSignature.Get();
    pipeline.VS={prepared->program->vertex->GetBufferPointer(),prepared->program->vertex->GetBufferSize()};
    pipeline.PS={prepared->program->pixel->GetBufferPointer(),prepared->program->pixel->GetBufferSize()};
    if(depthOnly&&!surfaceDepth)pipeline.PS={};
    auto& blend=pipeline.BlendState.RenderTarget[0];
    const bool alphaBlend=description.blend==rhi::BlendMode::Alpha,additiveBlend=description.blend==rhi::BlendMode::Additive;
    blend.BlendEnable=alphaBlend||additiveBlend;
    blend.SrcBlend=blend.BlendEnable?D3D12_BLEND_SRC_ALPHA:D3D12_BLEND_ONE;
    blend.DestBlend=additiveBlend?D3D12_BLEND_ONE:(alphaBlend?D3D12_BLEND_INV_SRC_ALPHA:D3D12_BLEND_ZERO);
    blend.SrcBlendAlpha=D3D12_BLEND_ONE;
    blend.DestBlendAlpha=additiveBlend?D3D12_BLEND_ONE:(alphaBlend?D3D12_BLEND_INV_SRC_ALPHA:D3D12_BLEND_ZERO);
    blend.BlendOp=blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;blend.LogicOp=D3D12_LOGIC_OP_NOOP;blend.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    pipeline.SampleMask=UINT_MAX;pipeline.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pipeline.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pipeline.RasterizerState.DepthClipEnable=TRUE;
    pipeline.RasterizerState.FrontCounterClockwise=pbr||surfaceDepth?TRUE:FALSE;
    pipeline.DepthStencilState.DepthEnable=depth!=rhi::DepthMode::Disabled;
    pipeline.DepthStencilState.DepthWriteMask=depth==rhi::DepthMode::ReadWriteLess?D3D12_DEPTH_WRITE_MASK_ALL:D3D12_DEPTH_WRITE_MASK_ZERO;
    pipeline.DepthStencilState.DepthFunc=depth==rhi::DepthMode::ReadOnlyEqual?D3D12_COMPARISON_FUNC_EQUAL:depth==rhi::DepthMode::ReadOnlyLessEqual?D3D12_COMPARISON_FUNC_LESS_EQUAL:depth==rhi::DepthMode::ReadOnlyGreater?D3D12_COMPARISON_FUNC_GREATER:depth==rhi::DepthMode::ReadWriteLess?D3D12_COMPARISON_FUNC_LESS:D3D12_COMPARISON_FUNC_ALWAYS;
    pipeline.DepthStencilState.FrontFace=pipeline.DepthStencilState.BackFace={D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_COMPARISON_FUNC_ALWAYS};
    pipeline.InputLayout=fullscreen?D3D12_INPUT_LAYOUT_DESC{}:depthOnly?(skinned?D3D12_INPUT_LAYOUT_DESC{skinnedDepthInputs,3}:compressed?D3D12_INPUT_LAYOUT_DESC{compressedInputs,1}:D3D12_INPUT_LAYOUT_DESC{meshInputs,1}):mesh?(skinned?D3D12_INPUT_LAYOUT_DESC{skinnedInputs,6}:compressed?D3D12_INPUT_LAYOUT_DESC{compressedInputs,4}:D3D12_INPUT_LAYOUT_DESC{meshInputs,pbr?4u:3u}):D3D12_INPUT_LAYOUT_DESC{triangleInputs,2};pipeline.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    if(surfaceDepth)pipeline.InputLayout={surfaceDepthInputs,skinned?4u:2u};
    if(gui)pipeline.InputLayout={guiInputs,3};
    pipeline.NumRenderTargets=depthOnly?0:1;pipeline.RTVFormats[0]=depthOnly?DXGI_FORMAT_UNKNOWN:description.colorFormat==rhi::ColorFormat::Rgba8Unorm?DXGI_FORMAT_R8G8B8A8_UNORM:description.colorFormat==rhi::ColorFormat::Rgba8Srgb?DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:DXGI_FORMAT_R16G16B16A16_FLOAT;
    pipeline.DSVFormat=mesh||pipeline.DepthStencilState.DepthEnable?DXGI_FORMAT_D32_FLOAT:DXGI_FORMAT_UNKNOWN;pipeline.SampleDesc={1,0};
    if(pipeline.InputLayout.NumElements)prepared->inputElements.assign(pipeline.InputLayout.pInputElementDescs,pipeline.InputLayout.pInputElementDescs+pipeline.InputLayout.NumElements);
    prepared->description=pipeline;prepared->description.InputLayout={prepared->inputElements.data(),static_cast<UINT>(prepared->inputElements.size())};
    // G02.04：本机 Release 实测受光照/阴影/环境的 HDR 管线库条目经 LoadGraphicsPipeline
    // 加载后，即使 GetCachedBlob 与新建 PSO 完全一致，也会使后续采样错 1 像素。
    // 这些 kind 跳过库加载并新建 PSO；其余 kind 继续命中持久库。
    // GUI 的同类问题已由真实像素回归复现：旧库加载后场景 sRGB 采样变为
    // 79/118/154，重新创建为正确的 20/46/82。保留进程内缓存，GUI 不读写持久库。
    // ToneMap 同样在历史库中复现边缘像素 161/期望 163；独立冷库和新库复用均正确。
    // 不复用这个不可信条目，保留源码/字节码缓存和进程内 PSO 共享。
    const bool libraryLoadAllowed=!rhi::IsClustered(description.kind)&&!rhi::IsCascaded(description.kind)&&description.kind!=rhi::PipelineKind::ShadowedMesh
        &&description.kind!=rhi::PipelineKind::EnvironmentMesh
        &&description.kind!=rhi::PipelineKind::EnvironmentSky
        &&description.kind!=rhi::PipelineKind::Gui
        &&description.kind!=rhi::PipelineKind::ToneMap;
    if(p.library&&libraryLoadAllowed&&!prepared->pipeline
        &&SUCCEEDED(p.library->LoadGraphicsPipeline(libraryName.c_str(),&pipeline,IID_PPV_ARGS(prepared->pipeline.GetAddressOf()))))
        ++p.stats.libraryHits;
    if(!prepared->pipeline)
    {
        if(!CheckGraphicsResult(p.device->CreateGraphicsPipelineState(&pipeline,IID_PPV_ARGS(prepared->pipeline.GetAddressOf())),text,"D3D12.CreatePipelineFailed"))return {};
        ++p.stats.pipelineCreations;
        if(p.library&&description.kind!=rhi::PipelineKind::Gui&&description.kind!=rhi::PipelineKind::ToneMap&&SUCCEEDED(p.library->StorePipeline(libraryName.c_str(),prepared->pipeline.Get()))){++p.stats.libraryStores;p.libraryDirty=true;}
    }
    prepared->rootSignature->SetName(L"FlareEngine.PreparedRootSignature");prepared->pipeline->SetName(L"FlareEngine.PreparedPipeline");
    p.livePipelines.push_back(prepared);if(description.cachePrepared)p.pipelines.insert_or_assign(key,prepared);return prepared;
}
D3D12PipelineCacheStatistics D3D12PipelineCache::Stats() const
{
    auto result=impl_->stats;
    for(const auto& [_,value]:impl_->programs)if(!value.expired())++result.livePrograms;
    for(const auto& value:impl_->livePipelines)if(!value.expired())++result.livePipelines;return result;
}
