#include "PlatformLayer/Graphics/D3D12/D3D12ShaderProgram.h"
#include "PlatformLayer/Graphics/D3D12/D3D12AmbientOcclusion.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Meshlet.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Particles.h"
#include "PlatformLayer/Graphics/D3D12/D3D12RayTracing.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Log.h"
#include "CoreLayer/Utf.h"
#include <iostream>
#include <string_view>
#include <array>

int wmain(int argc,wchar_t** argv)
{
    Localization text;std::string error;
    // 启动期语言表损坏时的独立回退，不依赖尚未可用的键。
    if(!text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error))
    {std::cerr<<"ShaderBuilder could not load localization: "<<error<<'\n';return 1;}
    const auto requestedMode=argc>1?std::wstring_view(argv[1]):std::wstring_view{};
    if(requestedMode==L"ao-compute")
    {
        if(argc!=4||!WriteD3D12AmbientOcclusionShaders(argv[2],argv[3],text))
        {Log(LogLevel::Error,text.GetText("Shader.WriteFailed"));return 1;}
        Log(LogLevel::Info,text.GetText("Shader.BuildComplete"));return 0;
    }
    if(requestedMode==L"particle")
    {
        if(argc!=4||!WriteD3D12ParticleShader(argv[2],argv[3],text))
        {Log(LogLevel::Error,text.GetText("Shader.WriteFailed"));return 1;}
        Log(LogLevel::Info,text.GetText("Shader.BuildComplete"));return 0;
    }
    if(requestedMode==L"meshlet-ms")
    {
        if(argc!=4||!WriteD3D12MeshletShader(argv[2],argv[3],text))
        {Log(LogLevel::Error,text.GetText("Shader.WriteFailed"));return 1;}
        Log(LogLevel::Info,text.GetText("Shader.BuildComplete"));return 0;
    }
    if(requestedMode==L"raytracing")
    {
        if(argc!=4||!WriteD3D12RayTracingShaders(argv[2],argv[3],text))
        {Log(LogLevel::Error,text.GetText("Shader.WriteFailed"));return 1;}
        Log(LogLevel::Info,text.GetText("Shader.BuildComplete"));return 0;
    }
    const bool surface=requestedMode.starts_with(L"surface-");
    const auto surfaceMode=surface?requestedMode.substr(8):requestedMode;
    const bool shading=surfaceMode.starts_with(L"shading-");
    const auto shadingMode=shading?surfaceMode.substr(8):surfaceMode;
    const bool pcss=shadingMode.starts_with(L"pcss-");
    const auto mode=pcss?shadingMode.substr(5):shadingMode;
    const bool depthNormals=mode==L"depth-normals"||mode==L"depth-normals-basic";
    const std::array clusterModes{
        std::pair{L"clustered-forward",flare::rhi::PipelineKind::ClusteredForwardMesh},
        std::pair{L"clustered-shadowed",flare::rhi::PipelineKind::ClusteredShadowedMesh},
        std::pair{L"clustered-environment",flare::rhi::PipelineKind::ClusteredEnvironmentMesh},
        std::pair{L"clustered-basiclit",flare::rhi::PipelineKind::ClusteredBasicLitMesh},
        std::pair{L"clustered-cascaded-shadowed",flare::rhi::PipelineKind::ClusteredCascadedShadowedMesh},
        std::pair{L"clustered-cascaded-environment",flare::rhi::PipelineKind::ClusteredCascadedEnvironmentMesh},
        std::pair{L"clustered-cascaded-basiclit",flare::rhi::PipelineKind::ClusteredCascadedBasicLitMesh},
        std::pair{L"clustered-skinned-forward",flare::rhi::PipelineKind::ClusteredForwardMesh},
        std::pair{L"clustered-skinned-shadowed",flare::rhi::PipelineKind::ClusteredShadowedMesh},
        std::pair{L"clustered-skinned-environment",flare::rhi::PipelineKind::ClusteredEnvironmentMesh},
        std::pair{L"clustered-cascaded-skinned-forward",flare::rhi::PipelineKind::ClusteredCascadedShadowedMesh},
        std::pair{L"clustered-cascaded-skinned-environment",flare::rhi::PipelineKind::ClusteredCascadedEnvironmentMesh}};
    bool clustered=false;auto clusterKind=flare::rhi::PipelineKind::Triangle;
    for(const auto& [name,kind]:clusterModes)if(mode==name){clustered=true;clusterKind=kind;break;}
    const bool userSurface=mode==L"user-pbr"||mode==L"user-forward"||mode==L"user-shadowed"||mode==L"user-environment";
    // W03：蒙皮变体使用 31 号绑定布局 + v3 顶点布局；宏与运行时 PipelineDescription 完全一致，
    // 否则源码键不同会导致产物被拒绝并回退到运行时编译。
    const bool skinned=(clustered&&mode.find(L"skinned")!=std::wstring_view::npos)||mode==L"skinned-forward"||mode==L"skinned-environment"||mode==L"skinned-shadowdepth"||mode==L"cascaded-skinned-forward"||mode==L"cascaded-skinned-environment";
    const bool cascaded=clustered||mode==L"cascaded-shadowed"||mode==L"cascaded-environment"||mode==L"cascaded-basiclit"||mode==L"cascaded-skinned-forward"||mode==L"cascaded-output"||mode==L"cascaded-skinned-environment";
    if(argc<4||(!depthNormals&&!userSurface&&!cascaded&&mode!=L"mesh"&&mode!=L"triangle"&&mode!=L"pbr"&&mode!=L"forward"&&mode!=L"output"&&mode!=L"sky"
        &&mode!=L"shadowed"&&mode!=L"shadowdepth"&&mode!=L"shadowoutput"
        &&mode!=L"environment"&&mode!=L"environmentsky"&&mode!=L"pbr-instanced"
        &&mode!=L"pbr-forward-instanced"
        &&mode!=L"skinned-forward"&&mode!=L"skinned-environment"&&mode!=L"skinned-shadowdepth"
        &&mode!=L"bloomextract"&&mode!=L"bloomblur"&&mode!=L"tonemap"&&mode!=L"fxaa"&&mode!=L"overlay"&&mode!=L"gui"&&mode!=L"user-pbr"&&mode!=L"basiclit"))
    {Log(LogLevel::Error,text.GetText("ShaderBuilder.Usage"));return 1;}
    flare::rhi::PipelineDescription description;
    const bool userPbr=userSurface;
    description.kind=mode==L"pbr"?flare::rhi::PipelineKind::PbrMesh:mode==L"mesh"?flare::rhi::PipelineKind::TexturedMesh:flare::rhi::PipelineKind::Triangle;
    if(userPbr)description.kind=flare::rhi::PipelineKind::PbrMesh;
    if(depthNormals){description.kind=flare::rhi::PipelineKind::PbrMesh;description.macros.push_back({"SE_DEPTH_NORMALS","1"});}
    if(mode==L"user-forward")description.kind=flare::rhi::PipelineKind::ForwardMesh;
    if(mode==L"user-shadowed")description.kind=flare::rhi::PipelineKind::ShadowedMesh;
    if(mode==L"user-environment")description.kind=flare::rhi::PipelineKind::EnvironmentMesh;
    if(userSurface&&mode!=L"user-pbr")description.macros.push_back({"SE_FORWARD","1"});
    if(mode==L"user-shadowed"||mode==L"user-environment")description.macros.push_back({"SE_SHADOWS","1"});
    if(mode==L"user-environment")description.macros.push_back({"SE_ENVIRONMENT","1"});
    if(mode==L"forward")description.kind=flare::rhi::PipelineKind::ForwardMesh;
    if(mode==L"output")description.kind=flare::rhi::PipelineKind::FinalOutput;
    if(mode==L"sky")description.kind=flare::rhi::PipelineKind::Sky;
    if(mode==L"basiclit")description.kind=flare::rhi::PipelineKind::BasicLitMesh;
    if(mode==L"shadowed")description.kind=flare::rhi::PipelineKind::ShadowedMesh;
    if(mode==L"shadowdepth")description.kind=flare::rhi::PipelineKind::ShadowDepth;
    if(mode==L"skinned-forward")description.kind=flare::rhi::PipelineKind::ShadowedMesh;
    if(mode==L"skinned-shadowdepth")description.kind=flare::rhi::PipelineKind::ShadowDepth;
    if(mode==L"shadowoutput")description.kind=flare::rhi::PipelineKind::ShadowOutput;
    if(mode==L"environment"||mode==L"skinned-environment")description.kind=flare::rhi::PipelineKind::EnvironmentMesh;
    if(mode==L"environmentsky")description.kind=flare::rhi::PipelineKind::EnvironmentSky;
    if(mode==L"bloomextract")description.kind=flare::rhi::PipelineKind::BloomExtract;
    if(mode==L"bloomblur")description.kind=flare::rhi::PipelineKind::BloomBlur;
    if(mode==L"tonemap")description.kind=flare::rhi::PipelineKind::ToneMap;
    if(mode==L"fxaa")description.kind=flare::rhi::PipelineKind::Fxaa;
    if(mode==L"overlay")description.kind=flare::rhi::PipelineKind::UiOverlay;
    if(mode==L"pbr-instanced")description.kind=flare::rhi::PipelineKind::PbrInstancedMesh;
    if(mode==L"pbr-forward-instanced")description.kind=flare::rhi::PipelineKind::PbrForwardInstancedMesh;
    if(mode==L"gui")description.kind=flare::rhi::PipelineKind::Gui;
    if(mode==L"cascaded-shadowed"||mode==L"cascaded-skinned-forward")description.kind=flare::rhi::PipelineKind::CascadedShadowedMesh;
    if(mode==L"cascaded-environment"||mode==L"cascaded-skinned-environment")description.kind=flare::rhi::PipelineKind::CascadedEnvironmentMesh;
    if(mode==L"cascaded-basiclit")description.kind=flare::rhi::PipelineKind::CascadedBasicLitMesh;
    if(mode==L"cascaded-output")description.kind=flare::rhi::PipelineKind::CascadedShadowOutput;
    if(clustered)description.kind=clusterKind;
    description.bindingLayoutVersion=flare::rhi::BindingLayout(description.kind);
    if(skinned)
    {
        description.bindingLayoutVersion=clustered?flare::rhi::SkinnedClusteredLayoutVersion:cascaded?flare::rhi::SkinnedCascadedLayoutVersion:flare::rhi::SkinnedLayoutVersion;
        description.vertexLayoutVersion=flare::rhi::VertexLayoutVersionSkinned;
        description.macros.push_back({"SE_SKINNED","1"});
    }
    if(pcss)
    {
        description.bindingLayoutVersion=flare::rhi::ShadowFilterBindingLayout(description.kind,skinned);
        if(!flare::rhi::ValidShadowFilterBindingLayout(description.kind,description.bindingLayoutVersion,skinned))
        {Log(LogLevel::Error,text.GetText("Shader.SourceInvalid"));return 1;}
    }
    if(surface)description.materialLayoutVersion=flare::rhi::MaterialSurfaceLayoutVersion;
    if(shading)description.shadingLayoutVersion=flare::rhi::ShadingExtensionLayoutVersion;
    description.shader=argv[2];
    for(int index=4;index<argc;++index)
    {
        std::u16string units;for(const wchar_t c:std::wstring_view(argv[index]))units+=static_cast<char16_t>(c);
        std::u32string decoded;std::string macro;
        if(!flare::DecodeUtf16(units,decoded)||!flare::EncodeUtf8(decoded,macro)){Log(LogLevel::Error,text.GetText("Shader.SourceInvalid"));return 1;}
        const auto equals=macro.find('=');description.macros.push_back({macro.substr(0,equals),equals==std::string::npos?"1":macro.substr(equals+1)});
    }
    D3D12ShaderSource source;if(!ReadD3D12ShaderSource(description,source,text))return 1;
    const auto program=CompileD3D12ShaderProgram(std::move(source),text);
    if(!program||!(userPbr?WriteD3D12UserPbrPackage(*program,argv[3],text):WriteD3D12ShaderProgram(*program,argv[3],text)))return 1;
    Log(LogLevel::Info,text.GetText("Shader.BuildComplete")+": "+program->source.key);return 0;
}
