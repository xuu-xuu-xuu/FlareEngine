#include "PlatformLayer/Graphics/D3D12/D3D12ShaderProgram.h"
#include "PlatformLayer/Graphics/D3D12/D3D12AmbientOcclusion.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Meshlet.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Particles.h"
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
    const bool surface=requestedMode.starts_with(L"surface-");
    const auto surfaceMode=surface?requestedMode.substr(8):requestedMode;
    const bool shading=surfaceMode.starts_with(L"shading-");
    const auto shadingMode=shading?surfaceMode.substr(8):surfaceMode;
    const bool pcss=shadingMode.starts_with(L"pcss-");
    const auto mode=pcss?shadingMode.substr(5):shadingMode;
    const bool depthNormals=mode==L"depth-normals"||mode==L"depth-normals-basic";
    const std::array clusterModes{
        std::pair{L"clustered-forward",se::rhi::PipelineKind::ClusteredForwardMesh},
        std::pair{L"clustered-shadowed",se::rhi::PipelineKind::ClusteredShadowedMesh},
        std::pair{L"clustered-environment",se::rhi::PipelineKind::ClusteredEnvironmentMesh},
        std::pair{L"clustered-basiclit",se::rhi::PipelineKind::ClusteredBasicLitMesh},
        std::pair{L"clustered-cascaded-shadowed",se::rhi::PipelineKind::ClusteredCascadedShadowedMesh},
        std::pair{L"clustered-cascaded-environment",se::rhi::PipelineKind::ClusteredCascadedEnvironmentMesh},
        std::pair{L"clustered-cascaded-basiclit",se::rhi::PipelineKind::ClusteredCascadedBasicLitMesh},
        std::pair{L"clustered-skinned-forward",se::rhi::PipelineKind::ClusteredForwardMesh},
        std::pair{L"clustered-skinned-shadowed",se::rhi::PipelineKind::ClusteredShadowedMesh},
        std::pair{L"clustered-skinned-environment",se::rhi::PipelineKind::ClusteredEnvironmentMesh},
        std::pair{L"clustered-cascaded-skinned-forward",se::rhi::PipelineKind::ClusteredCascadedShadowedMesh},
        std::pair{L"clustered-cascaded-skinned-environment",se::rhi::PipelineKind::ClusteredCascadedEnvironmentMesh}};
    bool clustered=false;auto clusterKind=se::rhi::PipelineKind::Triangle;
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
    se::rhi::PipelineDescription description;
    const bool userPbr=userSurface;
    description.kind=mode==L"pbr"?se::rhi::PipelineKind::PbrMesh:mode==L"mesh"?se::rhi::PipelineKind::TexturedMesh:se::rhi::PipelineKind::Triangle;
    if(userPbr)description.kind=se::rhi::PipelineKind::PbrMesh;
    if(depthNormals){description.kind=se::rhi::PipelineKind::PbrMesh;description.macros.push_back({"SE_DEPTH_NORMALS","1"});}
    if(mode==L"user-forward")description.kind=se::rhi::PipelineKind::ForwardMesh;
    if(mode==L"user-shadowed")description.kind=se::rhi::PipelineKind::ShadowedMesh;
    if(mode==L"user-environment")description.kind=se::rhi::PipelineKind::EnvironmentMesh;
    if(userSurface&&mode!=L"user-pbr")description.macros.push_back({"SE_FORWARD","1"});
    if(mode==L"user-shadowed"||mode==L"user-environment")description.macros.push_back({"SE_SHADOWS","1"});
    if(mode==L"user-environment")description.macros.push_back({"SE_ENVIRONMENT","1"});
    if(mode==L"forward")description.kind=se::rhi::PipelineKind::ForwardMesh;
    if(mode==L"output")description.kind=se::rhi::PipelineKind::FinalOutput;
    if(mode==L"sky")description.kind=se::rhi::PipelineKind::Sky;
    if(mode==L"basiclit")description.kind=se::rhi::PipelineKind::BasicLitMesh;
    if(mode==L"shadowed")description.kind=se::rhi::PipelineKind::ShadowedMesh;
    if(mode==L"shadowdepth")description.kind=se::rhi::PipelineKind::ShadowDepth;
    if(mode==L"skinned-forward")description.kind=se::rhi::PipelineKind::ShadowedMesh;
    if(mode==L"skinned-shadowdepth")description.kind=se::rhi::PipelineKind::ShadowDepth;
    if(mode==L"shadowoutput")description.kind=se::rhi::PipelineKind::ShadowOutput;
    if(mode==L"environment"||mode==L"skinned-environment")description.kind=se::rhi::PipelineKind::EnvironmentMesh;
    if(mode==L"environmentsky")description.kind=se::rhi::PipelineKind::EnvironmentSky;
    if(mode==L"bloomextract")description.kind=se::rhi::PipelineKind::BloomExtract;
    if(mode==L"bloomblur")description.kind=se::rhi::PipelineKind::BloomBlur;
    if(mode==L"tonemap")description.kind=se::rhi::PipelineKind::ToneMap;
    if(mode==L"fxaa")description.kind=se::rhi::PipelineKind::Fxaa;
    if(mode==L"overlay")description.kind=se::rhi::PipelineKind::UiOverlay;
    if(mode==L"pbr-instanced")description.kind=se::rhi::PipelineKind::PbrInstancedMesh;
    if(mode==L"pbr-forward-instanced")description.kind=se::rhi::PipelineKind::PbrForwardInstancedMesh;
    if(mode==L"gui")description.kind=se::rhi::PipelineKind::Gui;
    if(mode==L"cascaded-shadowed"||mode==L"cascaded-skinned-forward")description.kind=se::rhi::PipelineKind::CascadedShadowedMesh;
    if(mode==L"cascaded-environment"||mode==L"cascaded-skinned-environment")description.kind=se::rhi::PipelineKind::CascadedEnvironmentMesh;
    if(mode==L"cascaded-basiclit")description.kind=se::rhi::PipelineKind::CascadedBasicLitMesh;
    if(mode==L"cascaded-output")description.kind=se::rhi::PipelineKind::CascadedShadowOutput;
    if(clustered)description.kind=clusterKind;
    description.bindingLayoutVersion=se::rhi::BindingLayout(description.kind);
    if(skinned)
    {
        description.bindingLayoutVersion=clustered?se::rhi::SkinnedClusteredLayoutVersion:cascaded?se::rhi::SkinnedCascadedLayoutVersion:se::rhi::SkinnedLayoutVersion;
        description.vertexLayoutVersion=se::rhi::VertexLayoutVersionSkinned;
        description.macros.push_back({"SE_SKINNED","1"});
    }
    if(pcss)
    {
        description.bindingLayoutVersion=se::rhi::ShadowFilterBindingLayout(description.kind,skinned);
        if(!se::rhi::ValidShadowFilterBindingLayout(description.kind,description.bindingLayoutVersion,skinned))
        {Log(LogLevel::Error,text.GetText("Shader.SourceInvalid"));return 1;}
    }
    if(surface)description.materialLayoutVersion=se::rhi::MaterialSurfaceLayoutVersion;
    if(shading)description.shadingLayoutVersion=se::rhi::ShadingExtensionLayoutVersion;
    description.shader=argv[2];
    for(int index=4;index<argc;++index)
    {
        std::u16string units;for(const wchar_t c:std::wstring_view(argv[index]))units+=static_cast<char16_t>(c);
        std::u32string decoded;std::string macro;
        if(!se::DecodeUtf16(units,decoded)||!se::EncodeUtf8(decoded,macro)){Log(LogLevel::Error,text.GetText("Shader.SourceInvalid"));return 1;}
        const auto equals=macro.find('=');description.macros.push_back({macro.substr(0,equals),equals==std::string::npos?"1":macro.substr(equals+1)});
    }
    D3D12ShaderSource source;if(!ReadD3D12ShaderSource(description,source,text))return 1;
    const auto program=CompileD3D12ShaderProgram(std::move(source),text);
    if(!program||!(userPbr?WriteD3D12UserPbrPackage(*program,argv[3],text):WriteD3D12ShaderProgram(*program,argv[3],text)))return 1;
    Log(LogLevel::Info,text.GetText("Shader.BuildComplete")+": "+program->source.key);return 0;
}
