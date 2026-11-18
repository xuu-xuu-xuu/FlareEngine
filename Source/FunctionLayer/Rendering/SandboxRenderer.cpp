#include "SandboxRenderer.h"
#include "OcclusionHud.h"
#include "TemporalAA.h"
#include "PlatformLayer/Graphics/SceneSampling.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "FunctionLayer/Application/FrameData.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Log.h"
#include <utility>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <algorithm>
#include "ResourceLayer/Assets/Primitives.h"
#include "ResourceLayer/Assets/AssetStore.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include <vector>
#include "CoreLayer/Profiler.h"
#include "ForwardRenderer.h"
#include "ShadowSystem.h"
#include "ResourceLayer/Assets/ImageWriter.h"
#include "ResourceLayer/Assets/ImpostorBake.h"
#include "RenderQueue.h"
#include "PipelineWarmup.h"
#include "PhysicsDebugDraw.h"

namespace
{
bool EnvironmentScene(SandboxScene scene){return scene==SandboxScene::Environment||scene==SandboxScene::PostProcess;}
bool ForwardScene(SandboxScene scene){return scene==SandboxScene::Lighting||scene==SandboxScene::Shadows||scene==SandboxScene::BasicLit||EnvironmentScene(scene);}
bool MaterialScene(SandboxScene scene){return scene==SandboxScene::Materials||scene==SandboxScene::Benchmark;}
bool HlodSceneMode(SandboxScene scene){return scene==SandboxScene::Hlod;}
bool PbrScene(SandboxScene scene){return MaterialScene(scene)||scene==SandboxScene::Scene||HlodSceneMode(scene);}
bool BasicLitScene(SandboxScene scene){return scene==SandboxScene::BasicLit;}
std::uint64_t ComputeIndoorSceneToken(SandboxScene scene,const se::FrameData* data)
{
    std::uint64_t token=0;
    const auto mix=[&](std::uint64_t v){token^=v+0x9e3779b97f4a7c15ull+(token<<6)+(token>>2);};
    mix(static_cast<std::uint64_t>(scene));
    if(data)
    {
        mix(data->cameraBinding.value_or(0));
        mix(data->meshes.size());
        mix(data->editorGuiEnabled?1ull:0ull);
    }
    return token;
}
bool RequiresMaterialCoverage(const se::FrameData& frame)
{
    if(!frame.assets)return false;
    std::unordered_set<se::AssetId> materials,models;
    std::vector<se::AssetDiagnostic> ignored;
    for(const auto& instance:frame.meshes)
    {
        for(const auto& id:instance.mesh.materials)if(!id.empty()&&materials.insert(id).second)
        {
            const auto* preview=frame.materialPreview&&frame.materialPreview->id==id?&frame.materialPreview->document:nullptr;
            const auto material=preview?std::shared_ptr<const se::MaterialDocument>{}:frame.assets->LoadMaterial(id,ignored);
            if(preview?preview->alphaMode!=se::MaterialAlphaMode::Opaque:material&&material->alphaMode!=se::MaterialAlphaMode::Opaque)return true;
        }
        if(!se::IsBuiltinMesh(instance.mesh.meshAsset)&&models.insert(instance.mesh.meshAsset).second)
        {
            const auto model=frame.assets->LoadModel(instance.mesh.meshAsset,ignored);
            if(model&&std::any_of(model->materials.begin(),model->materials.end(),[](const auto& material){return material.alphaMode!=se::MaterialAlphaMode::Opaque;}))return true;
        }
    }
    return false;
}
}

std::filesystem::path SandboxRenderer::ResourceDirectory() const
{
    if(!resourceDirectory_.empty())return resourceDirectory_;
    const auto executable=GetExecutableDirectory();return executable.empty()?std::filesystem::path{}:executable/"Resources";
}
std::span<const std::byte> SandboxRenderer::BuildSkinPalette(const std::vector<se::Mat4>& palette,std::size_t jointCount,
    std::uint32_t& outJointCount)
{
    outJointCount=static_cast<std::uint32_t>(palette.empty()?jointCount:palette.size());
    if(!outJointCount)return {};
    skinPaletteStorage_.emplace_back(static_cast<std::size_t>(outJointCount)*48);
    auto& bytes=skinPaletteStorage_.back();
    for(std::uint32_t joint=0;joint<outJointCount;++joint)
    {
        const auto& matrix=palette.empty()?se::Mat4{}:palette[joint];
        // 行主序 3×4 与 Shaders/Skinning.hlsli 的 LoadSkinMatrix 一一对应。
        const float rows[12]{matrix(0,0),matrix(0,1),matrix(0,2),matrix(0,3),
            matrix(1,0),matrix(1,1),matrix(1,2),matrix(1,3),
            matrix(2,0),matrix(2,1),matrix(2,2),matrix(2,3)};
        std::memcpy(bytes.data()+static_cast<std::size_t>(joint)*48,rows,sizeof(rows));
    }
    return {bytes.data(),bytes.size()};
}
bool SandboxRenderer::Initialize(const RenderSurface& surface, const Localization& localization)
{
    const auto resources = ResourceDirectory();
    if(scene_!=SandboxScene::Clear&&resources.empty())return false;
    if(device_)return false;
    // G15.09：输出 LUT 变体由最终输出管线承载：编辑器 BasicLit 用 ShadowOutput，Lighting 演示用 FinalOutput。
    outputLutScene_=BasicLitScene(scene_)||scene_==SandboxScene::Lighting;
    captured_=false;
    // G13：Frame Graph 实验开关。默认关闭；DOT/JSON 只在首帧写出，便于审查 Pass/资源/别名。
    wchar_t frameGraphValue[8]{};
    const auto frameGraphLength=GetEnvironmentVariableW(L"SELFENGINE_FRAMEGRAPH",frameGraphValue,8);
    frameGraphEnabled_=frameGraphLength>0&&!(frameGraphLength==1&&frameGraphValue[0]==L'0');
    if(frameGraphEnabled_)
    {
        wchar_t frameGraphPath[1024]{};
        if(GetEnvironmentVariableW(L"SELFENGINE_FRAMEGRAPH_DOT",frameGraphPath,1024)>0)frameGraphDotPath_=frameGraphPath;
        frameGraphPath[0]=L'\0';
        if(GetEnvironmentVariableW(L"SELFENGINE_FRAMEGRAPH_JSON",frameGraphPath,1024)>0)frameGraphJsonPath_=frameGraphPath;
    }
    se::rhi::Result result;
    device_=se::rhi::Device::CreateD3D12({surface.window,surface.width,surface.height},localization,result);
    if(!device_)return false;
    if(scene_==SandboxScene::Triangle)
        return bool(device_->CreatePipeline({se::rhi::PipelineKind::Triangle,resources/"Shaders/Triangle.hlsl",1},pipeline_));
    if(scene_==SandboxScene::TexturedCube||scene_==SandboxScene::Scene||MaterialScene(scene_)||HlodSceneMode(scene_)||ForwardScene(scene_))
    {
        const bool basic=BasicLitScene(scene_),environment=EnvironmentScene(scene_),shadow=basic||scene_==SandboxScene::Shadows||environment,forward=ForwardScene(scene_),pbr=PbrScene(scene_)||forward;
        // G04.05：Materials 场景默认 BC1 颜色 + BC5 法线；SELFENGINE_BC1_DATA=1 打开数据 BC1（A/B），
        // SELFENGINE_LEGACY_BC5=1 只关法线 BC5，SELFENGINE_LEGACY_TEXTURE=1 全部回 RGBA8。
        const bool bc1Textures=scene_==SandboxScene::Materials&&GetEnvironmentVariableW(L"SELFENGINE_LEGACY_TEXTURE",nullptr,0)==0;
        const bool bc1DataTextures=bc1Textures&&GetEnvironmentVariableW(L"SELFENGINE_BC1_DATA",nullptr,0)!=0;
        const bool bc5Normals=bc1Textures&&GetEnvironmentVariableW(L"SELFENGINE_LEGACY_BC5",nullptr,0)==0;
        se::rhi::PipelineDescription description{basic?se::rhi::PipelineKind::BasicLitMesh:environment?se::rhi::PipelineKind::EnvironmentMesh:shadow?se::rhi::PipelineKind::ShadowedMesh:forward?se::rhi::PipelineKind::ForwardMesh:pbr?se::rhi::PipelineKind::PbrMesh:se::rhi::PipelineKind::TexturedMesh,
            resources/(basic?"Shaders/BasicLit.hlsl":environment?"Shaders/EnvironmentForward.hlsl":shadow?"Shaders/ShadowedForward.hlsl":forward?"Shaders/Forward.hlsl":pbr?"Shaders/Pbr.hlsl":"Shaders/TexturedMesh.hlsl"),basic?4u:environment?5u:shadow?4u:forward?3u:pbr?2u:1u};
        if(forward)description.colorFormat=se::rhi::ColorFormat::Rgba16Float;
        if(scene_==SandboxScene::Materials&&bc5Normals&&!forward)description.macros.push_back({"SE_NORMAL_BC5","1"});
        if(!device_->CreatePipeline(description,pipeline_))return false;
        // G15：SELFENGINE_TONEMAP_ACES=1 使用 ACES fitted ToneMap 变体；默认仍为 R 系列 Reinhard。
        wchar_t acesValue[8]{};
        const auto acesLength=GetEnvironmentVariableW(L"SELFENGINE_TONEMAP_ACES",acesValue,8);
        const bool acesToneMap=acesLength>0&&!(acesLength==1&&acesValue[0]==L'0');
        // G15：GPU 自动曝光与 33³ LUT 为显式实验开关；缺省保持既有路径。
        wchar_t exposureValue[8]{};
        const auto exposureLength=GetEnvironmentVariableW(L"SELFENGINE_AUTO_EXPOSURE",exposureValue,8);
        autoExposureEnabled_=exposureLength>0&&!(exposureLength==1&&exposureValue[0]==L'0');
        wchar_t lutValue[8]{};
        const auto lutLength=GetEnvironmentVariableW(L"SELFENGINE_LUT",lutValue,8);
        lutEnabled_=lutLength>0&&!(lutLength==1&&lutValue[0]==L'0');
        // G15：显式 LUT 文件优先。文件暂时不可读时仍启用 LUT（退回 LGG 生成），
        // 之后文件出现/被修复时由逐帧轮询自动接上。
        wchar_t lutFileValue[1024]{};
        if(GetEnvironmentVariableW(L"SELFENGINE_LUT_FILE",lutFileValue,1024)>0)
        {
            lutEnabled_=true;
            std::string error;
            if(lutFile_.Open(lutFileValue,error))
                Log(LogLevel::Info,localization.GetText("Rendering.LutLoaded")+": "
                    +std::to_string(lutFile_.Status().size)+"^3, revision "+std::to_string(lutFile_.Status().revision));
            else Log(LogLevel::Warning,localization.GetText(error.empty()?"Rendering.LutAssetInvalid":error));
            reportedLutFailures_=lutFile_.Status().failures;
            // G15.09：Lighting 的 FinalOutput 没有编辑器选择，直接以该文件为 strip 来源。
            // BasicLit 仍只跟随编辑器 `colorLutPath`，避免环境变量覆盖视口选择。
            if(scene_==SandboxScene::Lighting&&lutFile_.Ready())UpdateLutStrip(localization);
        }
        // G14：SELFENGINE_TAA=1 启用亚像素抖动、速度缓冲与历史解析。
        wchar_t taaValue[8]{};
        const auto taaLength=GetEnvironmentVariableW(L"SELFENGINE_TAA",taaValue,8);
        taaEnabled_=taaLength>0&&!(taaLength==1&&taaValue[0]==L'0');
        // G06.08：实例化遮挡批次为显式开关；只在单批次场景生效，其它帧回退普通绘制。
        wchar_t occlusionValue[8]{};
        const auto occlusionLength=GetEnvironmentVariableW(L"SELFENGINE_OCCLUSION",occlusionValue,8);
        occlusionEnabled_=occlusionLength>0&&!(occlusionLength==1&&occlusionValue[0]==L'0');
        // G06.09：HUD 依赖遮挡批次；遮挡关闭时即使设置 HUD 也不启用回读或覆盖层。
        wchar_t occlusionHudValue[8]{};
        const auto occlusionHudLength=GetEnvironmentVariableW(L"SELFENGINE_OCCLUSION_HUD",occlusionHudValue,8);
        occlusionHudEnabled_=occlusionEnabled_&&occlusionHudLength>0
            &&!(occlusionHudLength==1&&occlusionHudValue[0]==L'0');
        // G06.11：遮挡 GPU 分段时间；默认关闭，避免为纯绘制路径创建查询堆。
        wchar_t occlusionTimingValue[8]{};
        const auto occlusionTimingLength=GetEnvironmentVariableW(L"SELFENGINE_OCCLUSION_TIMING",occlusionTimingValue,8);
        occlusionTimingEnabled_=occlusionEnabled_&&occlusionTimingLength>0
            &&!(occlusionTimingLength==1&&occlusionTimingValue[0]==L'0');
        occlusionCommitted_=false;
        previousViewProjectionValid_=false;previousWorlds_.clear();
        indoorHistoryValid_=false;lastSceneWidth_=0;lastSceneHeight_=0;lastIndoorSceneToken_=0;
        if((scene_==SandboxScene::PostProcess||basic)&&!post_.Initialize(*device_,resources/"Shaders",acesToneMap))return false;
        featurePostAces_=acesToneMap;
        if(basic)
        {
            se::rhi::PipelineDescription fullPbr{se::rhi::PipelineKind::ShadowedMesh,resources/"Shaders/ShadowedForward.hlsl",4};
            fullPbr.colorFormat=se::rhi::ColorFormat::Rgba16Float;
            if(!device_->CreatePipeline(fullPbr,featurePbrPipeline_))return false;
            se::rhi::PipelineDescription material{se::rhi::PipelineKind::EnvironmentMesh,resources/"Shaders/EnvironmentForward.hlsl",5};
            material.colorFormat=se::rhi::ColorFormat::Rgba16Float;
            se::rhi::PipelineDescription sky{se::rhi::PipelineKind::EnvironmentSky,resources/"Shaders/EnvironmentSky.hlsl",5};
            sky.colorFormat=se::rhi::ColorFormat::Rgba16Float;
            if(!device_->CreatePipeline(material,featureEnvironmentPipeline_)||!device_->CreatePipeline(sky,featureEnvironmentSky_))return false;
            if(!device_->CreatePipeline({se::rhi::PipelineKind::FinalOutput,resources/"Shaders/FinalOutput.hlsl",3},featureForwardOutput_))return false;
        }
        if(forward)
        {
            se::rhi::PipelineDescription output{shadow?se::rhi::PipelineKind::ShadowOutput:se::rhi::PipelineKind::FinalOutput,
                resources/(shadow?"Shaders/ShadowOutput.hlsl":"Shaders/FinalOutput.hlsl"),shadow?4u:3u};
            // G15：输出 LUT 布局（t2 strip）；禁用时绑定 1×1 占位并由 Shader 跳过。
            // G15.09：Lighting 的非阴影 FinalOutput 也使用同一 29 布局，t1 在 FinalOutput 中不读取。
            if(outputLutScene_)
            {
                output.bindingLayoutVersion=se::rhi::OutputLutLayoutVersion;
                output.macros.push_back({"SE_OUTPUT_LUT","1"});
                const std::vector<std::uint8_t> white{255,255,255,255};
                if(!device_->CreateTexture({1,1},white,lutDummy_))return false;
            }
            if(!device_->CreatePipeline(output,outputPipeline_))return false;
        }
        if(shadow&&!device_->CreatePipeline({se::rhi::PipelineKind::ShadowDepth,resources/"Shaders/ShadowDepth.hlsl",4},shadowPipeline_))return false;
        // W03：蒙皮变体。前向家族用 ShadowedMesh + 31 号绑定布局 + v3 顶点布局；
        // 阴影深度用 ShadowDepth + 31 号布局（b0 常量 + t8 调色板）。
        // 环境材质的蒙皮受光管线按需加载，投射共享 SkinnedShadowDepth。
        if(shadow)
        {
            se::rhi::PipelineDescription skinned{se::rhi::PipelineKind::ShadowedMesh,resources/"Shaders/SkinnedForward.hlsl",
                se::rhi::SkinnedLayoutVersion};
            skinned.vertexLayoutVersion=se::rhi::VertexLayoutVersionSkinned;
            skinned.colorFormat=se::rhi::ColorFormat::Rgba16Float;
            skinned.macros.push_back({"SE_SKINNED","1"});
            se::rhi::PipelineDescription skinnedShadow{se::rhi::PipelineKind::ShadowDepth,resources/"Shaders/SkinnedShadowDepth.hlsl",
                se::rhi::SkinnedLayoutVersion};
            skinnedShadow.vertexLayoutVersion=se::rhi::VertexLayoutVersionSkinned;
            skinnedShadow.macros.push_back({"SE_SKINNED","1"});
            if((!environment&&!device_->CreatePipeline(skinned,skinnedPipeline_))||!device_->CreatePipeline(skinnedShadow,skinnedShadowPipeline_))return false;
        }
        if(forward)
        {
            se::rhi::PipelineDescription sky{environment?se::rhi::PipelineKind::EnvironmentSky:se::rhi::PipelineKind::Sky,resources/(environment?"Shaders/EnvironmentSky.hlsl":"Shaders/Sky.hlsl"),environment?5u:9u};sky.colorFormat=se::rhi::ColorFormat::Rgba16Float;
            if(!device_->CreatePipeline(sky,skyPipeline_))return false;
        }
        if(scene_==SandboxScene::Materials)
        {
            models_.EnableBc1Textures(bc1Textures,GetExecutableDirectory()/"Cache/Textures",bc1DataTextures);
            models_.SetBc5Normals(bc5Normals);
            // G04.06：SELFENGINE_TEXTURE_BUDGET_KB 以 KB 覆盖默认 512 MiB，便于做降级 A/B。
            wchar_t budget[16]{};
            if(GetEnvironmentVariableW(L"SELFENGINE_TEXTURE_BUDGET_KB",budget,16)>0)
            {
                const auto value=std::wcstoull(budget,nullptr,10);
                if(value)models_.SetTextureBudget(static_cast<std::uint64_t>(value)*1024);
            }
        }
        // G05.07/G05.08：SELFENGINE_LOD=1 时 Materials 的场景模型带 4 级 LOD（默认单级，画面不变）。
        // 按变量值判断，SELFENGINE_LOD=0 必须真正关闭，供远景 A/B 使用。
        wchar_t lodValue[8]{};
        const auto lodLength=GetEnvironmentVariableW(L"SELFENGINE_LOD",lodValue,8);
        const bool lodScene=scene_==SandboxScene::Materials||HlodSceneMode(scene_);
        lodEnabled_=lodScene&&lodLength>0&&!(lodLength==1&&lodValue[0]==L'0');
        forcedLodLevel_=UINT32_MAX;
        if(lodEnabled_)
        {
            wchar_t forcedValue[8]{};
            const auto forcedLength=GetEnvironmentVariableW(L"SELFENGINE_LOD_LEVEL",forcedValue,8);
            if(forcedLength==1&&forcedValue[0]>=L'0'&&forcedValue[0]<=L'3')
                forcedLodLevel_=static_cast<std::uint32_t>(forcedValue[0]-L'0');
        }
        // G05.10：SELFENGINE_HLOD=1 时 Hlod 场景按 64 m 单元用代理替换远景实例。
        // G05.11：可选从 `.sehlod` 加载、把烘焙结果写盘，并按相机半径限制驻留。
        hlodEnabled_=false;hlodLevel_=0;
        hlodFilePath_.clear();hlodSavePath_.clear();hlodFileLoaded_=false;hlodSaved_=false;hlodResidentRadius_=0;
        hlodAsync_=false;hlodLazyProxies_=false;hlodUploadBudget_=0;hlodVramBudget_=0;
        if(HlodSceneMode(scene_))
        {
            wchar_t hlodValue[8]{};
            const auto hlodLength=GetEnvironmentVariableW(L"SELFENGINE_HLOD",hlodValue,8);
            hlodEnabled_=hlodLength>0&&!(hlodLength==1&&hlodValue[0]==L'0');
            if(hlodEnabled_)
            {
                wchar_t levelValue[8]{};
                const auto levelLength=GetEnvironmentVariableW(L"SELFENGINE_HLOD_LEVEL",levelValue,8);
                if(levelLength==1&&levelValue[0]>=L'0'&&levelValue[0]<=L'3')
                    hlodLevel_=static_cast<std::uint32_t>(levelValue[0]-L'0');
                wchar_t fileValue[1024]{};
                if(GetEnvironmentVariableW(L"SELFENGINE_HLOD_FILE",fileValue,1024)>0)hlodFilePath_=fileValue;
                wchar_t saveValue[1024]{};
                if(GetEnvironmentVariableW(L"SELFENGINE_HLOD_SAVE",saveValue,1024)>0)hlodSavePath_=saveValue;
                wchar_t radiusValue[32]{};
                if(GetEnvironmentVariableW(L"SELFENGINE_HLOD_RADIUS",radiusValue,32)>0)
                {
                    const auto parsed=std::wcstod(radiusValue,nullptr);
                    if(std::isfinite(parsed)&&parsed>0)hlodResidentRadius_=static_cast<float>(parsed);
                }
                if(!hlodFilePath_.empty())
                {
                    // G05.13：异步读取；失败回退同步读取。显存预算与每帧上传上限为 0 时保持全量同步行为。
                    wchar_t asyncValue[8]{};
                    const auto asyncLength=GetEnvironmentVariableW(L"SELFENGINE_HLOD_ASYNC",asyncValue,8);
                    hlodAsync_=asyncLength>0&&!(asyncLength==1&&asyncValue[0]==L'0');
                    wchar_t budgetValue[32]{};
                    if(GetEnvironmentVariableW(L"SELFENGINE_HLOD_VRAM_MB",budgetValue,32)>0)
                    {
                        const auto parsed=std::wcstoull(budgetValue,nullptr,10);
                        if(parsed>0)hlodVramBudget_=static_cast<std::uint64_t>(parsed)*1024*1024;
                    }
                    wchar_t uploadValue[32]{};
                    if(GetEnvironmentVariableW(L"SELFENGINE_HLOD_UPLOAD_BUDGET",uploadValue,32)>0)
                    {
                        const auto parsed=std::wcstoull(uploadValue,nullptr,10);
                        if(parsed>0)hlodUploadBudget_=static_cast<std::uint32_t>(parsed);
                    }
                    hlodLazyProxies_=hlodAsync_||hlodVramBudget_>0||hlodUploadBudget_>0;
                    std::uint32_t asyncDelayMs=0;
                    wchar_t delayValue[32]{};
                    if(GetEnvironmentVariableW(L"SELFENGINE_HLOD_ASYNC_DELAY_MS",delayValue,32)>0)
                    {
                        const auto parsed=std::wcstoull(delayValue,nullptr,10);
                        if(parsed>0)asyncDelayMs=static_cast<std::uint32_t>(parsed);
                    }
                    std::string readError;
                    if(hlodAsync_)
                    {
                        if(hlodStreamer_.Start(hlodFilePath_,readError,asyncDelayMs))
                            Log(LogLevel::Info,localization.GetText("Rendering.HlodStreamStarted"));
                        else
                        {
                            hlodAsync_=false;
                            Log(LogLevel::Warning,localization.GetText(readError.empty()?"Assets.ReadFailed":readError));
                        }
                    }
                    if(!hlodAsync_)
                    {
                        hlodFileLoaded_=hlodStream_.Load(hlodFilePath_,readError);
                        if(!hlodFileLoaded_)
                            Log(LogLevel::Warning,localization.GetText(readError.empty()?"Assets.ReadFailed":readError));
                        else Log(LogLevel::Info,localization.GetText("Rendering.HlodLoaded")+": "
                            +std::to_string(hlodStream_.Scene().cells.size()));
                    }
                }
            }
            // G05.12：SELFENGINE_IMPOSTOR=1 时烘焙 16 视角图集并创建朝向相机的四边形管线。
                wchar_t impostorValue[8]{};
                const auto impostorLength=GetEnvironmentVariableW(L"SELFENGINE_IMPOSTOR",impostorValue,8);
                impostorEnabled_=impostorLength>0&&!(impostorLength==1&&impostorValue[0]==L'0');
                if(impostorEnabled_)
                {
                    wchar_t distanceValue[32]{};
                    if(GetEnvironmentVariableW(L"SELFENGINE_IMPOSTOR_DISTANCE",distanceValue,32)>0)
                    {
                        const auto parsed=std::wcstod(distanceValue,nullptr);
                        if(std::isfinite(parsed)&&parsed>0)impostorDistance_=static_cast<float>(parsed);
                    }
                    se::ImpostorBakeSettings bakeSettings;bakeSettings.tileSize=64;bakeSettings.color={0.85f,0.35f,0.08f};
                    se::ImpostorAtlas atlas;std::vector<se::AssetDiagnostic> bakeDiagnostics;
                    const auto sphereMesh=se::MakeUnitSphere();
                    if(!se::BakeImpostorAtlas(sphereMesh,se::Mat4{},bakeSettings,atlas,bakeDiagnostics))
                    {
                        for(const auto& diagnostic:bakeDiagnostics)Log(LogLevel::Error,localization.GetText(diagnostic.key)+": "+diagnostic.detail);
                        impostorEnabled_=false;
                    }
                    else
                    {
                        se::rhi::TextureDescription atlasDescription{atlas.Width(),atlas.Height()};
                        atlasDescription.name="HlodImpostorAtlas";
                        if(!device_->CreateTexture(atlasDescription,atlas.rgba,impostorTexture_)
                            ||!device_->CreatePipeline({se::rhi::PipelineKind::TexturedMesh,resources/"Shaders/Impostor.hlsl",se::rhi::MaterialLayoutVersion},impostorPipeline_))
                            impostorEnabled_=false;
                    }
                    if(impostorEnabled_)
                    {
                        auto quadModel=std::make_shared<se::StaticModel>();
                        se::MeshData quad;quad.name="ImpostorQuad";
                        quad.vertices={{{-0.5f,-0.5f,0},{0,0,1},{0,1},{1,0,0,1}},{{0.5f,-0.5f,0},{0,0,1},{1,1},{1,0,0,1}},
                            {{0.5f,0.5f,0},{0,0,1},{1,0},{1,0,0,1}},{{-0.5f,0.5f,0},{0,0,1},{0,0},{1,0,0,1}}};
                        quad.indices={0,1,2,0,2,3};quad.material=0;quadModel->meshes.push_back(std::move(quad));
                        quadModel->materials.emplace_back();
                        se::ModelNode node;node.name="ImpostorQuad";node.meshes={0};quadModel->nodes.push_back(std::move(node));
                        std::vector<se::AssetDiagnostic> quadDiagnostics;
                        if(!models_.Upload(quadModel,{},resources,*device_,impostorQuad_,quadDiagnostics,false,0))impostorEnabled_=false;
                    }
                }
            }
        const std::uint32_t lodUploads=(lodEnabled_||(hlodEnabled_&&hlodLevel_>0))?4u:0u;
        const bool compressVertices=scene_==SandboxScene::Materials&&GetEnvironmentVariableW(L"SELFENGINE_LEGACY_VERTEX",nullptr,0)==0;
        if(scene_==SandboxScene::Materials)
        {
            se::rhi::PipelineDescription compressed{se::rhi::PipelineKind::PbrMesh,resources/"Shaders/Pbr.hlsl",se::rhi::PbrMaterialLayoutVersion};
            compressed.vertexLayoutVersion=se::rhi::VertexLayoutVersionCompressed;
            compressed.macros.push_back({"SE_VERTEX_COMPRESSED","1"});
            if(bc5Normals)compressed.macros.push_back({"SE_NORMAL_BC5","1"});
            if(!device_->CreatePipeline(compressed,compressedPipeline_))return false;
        }
        if(pbr||forward)
        {
            // G02：实例化管线在初始化阶段预热，避免首次启用实例化时编译。
            se::rhi::PipelineDescription instanced{se::rhi::PipelineKind::PbrInstancedMesh,resources/"Shaders/PbrInstanced.hlsl",se::rhi::InstancingLayoutVersion};
            se::rhi::Pipeline created;
            const auto warmup=se::WarmupPipelines(*device_,std::span<const se::rhi::PipelineDescription>(&instanced,1),std::span<se::rhi::Pipeline>(&created,1));
            if(warmup.failed||!created)return false;
            instancedPipeline_=std::move(created);
            Log(LogLevel::Debug,localization.GetText("Rendering.PsoWarmup")+": 1/1, "+std::to_string(warmup.milliseconds)+" ms");
        }
        auto model=std::make_shared<se::StaticModel>();
        auto mesh=se::MakeUnitCube();mesh.material=0;model->meshes.push_back(std::move(mesh));
        model->materials.emplace_back();
        if(!pbr||scene_==SandboxScene::Scene){model->materials[0].baseColorTexture=0;model->textures.push_back({"checker.png",{}, {},false});}
        se::ModelNode node;node.name="UnitCube";node.meshes={0};model->nodes.push_back(std::move(node));
        std::vector<se::AssetDiagnostic> diagnostics;
        if(!models_.Upload(model,resources/"Textures/builtin",resources,*device_,cube_,diagnostics,compressVertices,lodUploads))
        {
            for(const auto& diagnostic:diagnostics)Log(LogLevel::Error,localization.GetText(diagnostic.key)+": "+diagnostic.detail);
            return false;
        }
        if(pbr||scene_==SandboxScene::Scene)
        {
            auto sphere=std::make_shared<se::StaticModel>(*model);sphere->meshes[0]=se::MakeUnitSphere();sphere->meshes[0].material=0;
            if(!models_.Upload(sphere,pbr&&scene_!=SandboxScene::Scene?std::filesystem::path{}:resources/"Textures/builtin",resources,*device_,sphere_,diagnostics,compressVertices,lodUploads))return false;
        }
    }
    return true;
}

RenderResult SandboxRenderer::Render(const RenderFrame& frame, const Localization& localization)
{
    drawStatistics_={frame.data?frame.data->meshes.size():0,0,0};
    frameTriangles_=0;
    const auto telemetryStart=std::chrono::steady_clock::now();
    telemetry_.BeginFrame(frame.updateIndex,Statistics());
    std::optional<se::CpuScope> preparation;preparation.emplace(frame.profiler,"CPU.CommandPrepare");
    camera_.reset();
    if (!frame.surface.width || !frame.surface.height) return RenderResult::Occluded;
    const bool coverageFrame=frame.data&&RequiresMaterialCoverage(*frame.data);
    const bool materialShading=frame.data&&se::RequiresMaterialShading(*frame.data);
    const bool localLighting=frame.data&&std::any_of(frame.data->lights.begin(),frame.data->lights.end(),[](const auto& instance){
        return instance.light.type==se::LightType::Spot||(instance.light.type==se::LightType::Point&&instance.light.castsShadow);
    });
    const bool cascadeRequested=frame.data&&frame.data->shadow.cascades;
    const bool clusterRequested=frame.data&&frame.data->clusteredLighting;
    bool customCascadeUnsupported=false;
    if(frame.data&&frame.data->assets&&(cascadeRequested||clusterRequested||BasicLitScene(scene_)))
        for(const auto& object:frame.data->meshes)for(const auto& id:object.mesh.materials)if(!id.empty())
        {
            std::vector<se::AssetDiagnostic> ignored;
            const auto* preview=frame.data->materialPreview&&frame.data->materialPreview->id==id?&frame.data->materialPreview->document:nullptr;
            const auto material=preview?std::shared_ptr<const se::MaterialDocument>{}:frame.data->assets->LoadMaterial(id,ignored);
            customCascadeUnsupported=customCascadeUnsupported||(preview?!preview->shader.empty():material&&!material->shader.empty());
        }
    const bool indoorLighting=frame.data&&frame.data->indoor.Enabled();
    // 已编译的用户 Shader 沿用其布局；需要材质扩展的表面仍使用引擎的对应技术。
    const bool shadingRequested=frame.data&&(materialShading||localLighting||!frame.data->rectangleLights.empty()
        ||(!customCascadeUnsupported&&(indoorLighting||frame.data->multiscatter||frame.data->gtao||frame.data->contactShadows
            ||frame.data->diffuseSh||frame.data->localDiffuseSh)));
    const bool shadingFrame=shadingRequested&&ForwardScene(scene_);
    const bool bc7Color=frame.data&&frame.data->bc7Color;
    const bool meshletSubmission=frame.data&&frame.data->meshletCulling;
    if(bc7Color!=models_.Bc7ColorsEnabled()||meshletSubmission!=meshletSubmission_)
    {
        if(!device_->WaitIdle())return RenderResult::Failed;
        models_.EnableBc7Colors(bc7Color,frame.data&&frame.data->assets?frame.data->assets->CacheRoot()/"textures":std::filesystem::path{});
        models_.SetMeshletSubmissionEnabled(meshletSubmission);meshletSubmission_=meshletSubmission;
        // 派生代理也可能强持上个编码版本，切换后重新建立代理及驻留账本。
        hlodProxies_.clear();hlodOverrides_.clear();hlodProxyActive_.clear();
        hlodProxyCache_.Reset();hlodBaked_=false;featureHlodSignature_.clear();
        instances_.Clear();
        lodTransitions_.clear();
        std::vector<se::AssetDiagnostic> diagnostics;
        const auto reloadBuiltin=[&](std::shared_ptr<const se::StaticRenderModel>& value,bool sphere)
        {
            if(!value)return true;
            auto source=value->source;const bool compressed=value->compressed;
            const auto levels=value->meshes.empty()||!value->meshes[0].lod?0u:static_cast<std::uint32_t>(value->meshes[0].lod->levels.size());
            std::shared_ptr<const se::StaticRenderModel> candidate;
            const auto path=sphere&&scene_!=SandboxScene::Scene?std::filesystem::path{}:ResourceDirectory()/"Textures/builtin";
            if(!models_.Upload(std::move(source),path,ResourceDirectory(),*device_,candidate,diagnostics,compressed,levels))return false;
            value=std::move(candidate);return true;
        };
        if(!reloadBuiltin(cube_,false)||!reloadBuiltin(sphere_,true))return RenderResult::Failed;
    }
    const bool requestedPost=frame.data&&frame.data->post.has_value();
    const bool sceneInstancing=frame.data&&frame.data->instancing&&!coverageFrame&&!requestedPost&&!shadingFrame
        &&!frame.data->depthNormals&&!frame.data->gpuDriven&&!frame.data->meshletCulling&&!frame.data->gpuDrivenHiZ;
    if(frame.data&&frame.data->instancing&&!sceneInstancing&&reportedDiagnostics_.insert("Rendering.ShadingInstancingFallback").second)
        Log(LogLevel::Warning,localization.GetText("Rendering.ShadingInstancingFallback"));
    if(coverageFrame&&frame.data&&(frame.data->instancing||frame.data->modelHlod)
        &&reportedDiagnostics_.insert("Rendering.MaterialCoverageFallback").second)
        Log(LogLevel::Warning,localization.GetText("Rendering.MaterialCoverageFallback"));
    const bool postFrame=scene_==SandboxScene::PostProcess||(BasicLitScene(scene_)&&frame.data&&frame.data->sceneRenderSettings&&frame.data->post.has_value());
    std::shared_ptr<const se::RenderEnvironment> defaultEnvironment;
    if(BasicLitScene(scene_))
    {
        if(frame.data&&frame.data->environment.useEngineDefault&&frame.data->environment.asset.empty()
            &&!sceneInstancing&&!customCascadeUnsupported)
        {
            std::vector<se::AssetDiagnostic> diagnostics;
            defaultEnvironment=environments_.EngineDefault(ResourceDirectory(),*device_,diagnostics);
            for(const auto& diagnostic:diagnostics)if(reportedDiagnostics_.insert(diagnostic.key+":"+diagnostic.detail).second)
                Log(LogLevel::Warning,localization.GetText(diagnostic.key)+": "+diagnostic.detail);
        }
        // 编辑期间实例姿态、材质或资源改变后失效代理；相机变化不触发重烘焙。
        const bool hlod=frame.data&&frame.data->modelHlod&&!coverageFrame&&!shadingFrame;
        std::string signature;
        if(hlod)for(const auto& mesh:frame.data->meshes)
        {
            signature.append(reinterpret_cast<const char*>(&mesh.entity),sizeof(mesh.entity));
            signature.append(reinterpret_cast<const char*>(mesh.world.values.data()),sizeof(float)*16);
            signature+=mesh.mesh.meshAsset;signature+='\0';for(const auto& id:mesh.mesh.materials){signature+=id;signature+='\0';}
            if(frame.data->assets){const auto revision=[&](const auto& id){const auto path=frame.data->assets->Resolve(id);std::error_code ec;const auto stamp=std::filesystem::last_write_time(path,ec);if(!ec){const auto ticks=stamp.time_since_epoch().count();signature.append(reinterpret_cast<const char*>(&ticks),sizeof(ticks));}};revision(mesh.mesh.meshAsset);for(const auto& id:mesh.mesh.materials)revision(id);}
        }
        if(hlod!=hlodEnabled_||signature!=featureHlodSignature_)
        {
            if(!device_->WaitIdle())return RenderResult::Failed;
            hlodBaked_=hlodFailed_=false;hlodInstanceCount_=0;hlodCells_.clear();hlodProxies_.clear();hlodOverrides_.clear();hlodProxyActive_.clear();hlodCandidateCell_.clear();hlodProxyBytes_.clear();hlodProxyCache_.Reset();
            featureHlodSignature_=std::move(signature);hlodEnabled_=hlod;
        }
        hlodResidentRadius_=frame.data?frame.data->hlodRadius:0;
        const bool pbr=frame.data&&(frame.data->fullPbr||materialShading||localLighting||indoorLighting||frame.data->multiscatter
            ||frame.data->diffuseSh||frame.data->localDiffuseSh||!frame.data->rectangleLights.empty());
        if(pbr!=featureFullPbr_){std::swap(featureEnvironment_?featureEnvironmentPipeline_:pipeline_,featurePbrPipeline_);featureFullPbr_=pbr;previousViewProjectionValid_=false;indoorHistoryValid_=false;}
        const bool instanced=sceneInstancing;
        if(instanced!=featureForward_){std::swap(outputPipeline_,featureForwardOutput_);featureForward_=instanced;previousViewProjectionValid_=false;indoorHistoryValid_=false;}
        const bool environment=frame.data&&(defaultEnvironment||!frame.data->environment.asset.empty()||frame.data->diffuseSh||frame.data->localDiffuseSh)&&!instanced;
        if(environment!=featureEnvironment_)
        {std::swap(pipeline_,featureEnvironmentPipeline_);std::swap(skyPipeline_,featureEnvironmentSky_);featureEnvironment_=environment;previousViewProjectionValid_=false;indoorHistoryValid_=false;}
        const bool taa=postFrame&&frame.data->post->taa;
        if(taa!=taaEnabled_){previousViewProjectionValid_=false;indoorHistoryValid_=false;previousWorlds_.clear();}
        taaEnabled_=taa;autoExposureEnabled_=postFrame&&frame.data->post->autoExposure;lutEnabled_=postFrame&&frame.data->post->lutEnabled;
        if(frame.data&&frame.data->sceneRenderSettings){occlusionEnabled_=frame.data->occlusion;occlusionHudEnabled_=frame.data->occlusionCounters;occlusionTimingEnabled_=frame.data->occlusionTiming;frameGraphEnabled_=frame.data->frameGraph;}
        else {occlusionEnabled_=occlusionHudEnabled_=occlusionTimingEnabled_=frameGraphEnabled_=false;}
        const bool aces=frame.data&&frame.data->sceneRenderSettings&&frame.data->acesToneMap;
        if(aces!=featurePostAces_)
        {
            se::PostProcessRenderer candidate;
            if(!candidate.Initialize(*device_,ResourceDirectory()/"Shaders",aces)||!device_->WaitIdle())return RenderResult::Failed;
            post_=std::move(candidate);
            featurePostAces_=aces;
        }
        const bool lod=frame.data&&frame.data->modelLod;
        if(lod!=lodEnabled_)
        {
            if(!device_->WaitIdle())return RenderResult::Failed;
            models_.SetLodLevels(lod?4:0);lodEnabled_=lod;lodTransitions_.clear();
        }
    }
    if (frame.data)
    {
        se::CameraViewport viewport{ 0, 0, static_cast<float>(frame.surface.width), static_cast<float>(frame.surface.height) };
        const auto scene = frame.data->sceneViewport;
        if (scene.width != 0 || scene.height != 0) viewport = { scene.x, scene.y, scene.width, scene.height };
        auto selected = se::SelectCamera(frame.data->cameras, viewport, frame.updateIndex, frame.data->cameraBinding);
        auto error = selected.error;
        if (error == se::CameraError::NoPrimary && frame.data->cameras.empty()) error = se::CameraError::None;
        if (error != se::CameraError::None && error != lastCameraError_)
            Log(LogLevel::Warning, localization.GetText(se::CameraErrorKey(error)));
        if (selected.multiplePrimary && !lastMultiplePrimary_)
            Log(LogLevel::Warning, localization.GetText("Camera.MultiplePrimary"));
        if (selected.rejectedPrimaryCount && !lastRejectedPrimary_)
            Log(LogLevel::Warning, localization.GetText("Camera.RejectedPrimary") + ": " + std::to_string(selected.rejectedPrimaryCount));
        lastCameraError_ = error;
        lastMultiplePrimary_ = selected.multiplePrimary;
        lastRejectedPrimary_ = selected.rejectedPrimaryCount != 0;
        camera_ = std::move(selected.snapshot);
        if(frame.data->editorCamera)camera_=frame.data->editorCamera;
        if (camera_ && scene_!=SandboxScene::Clear && !controlsReported_)
        {
            Log(LogLevel::Info, localization.GetText(frame.data->editorGuiEnabled?"EditorUi.CameraControls":scene_==SandboxScene::Benchmark?"Sample.BenchmarkControls":scene_==SandboxScene::PostProcess?"PostProcess.DemoControls":scene_==SandboxScene::Environment?"Environment.DemoControls":scene_==SandboxScene::Shadows?"Shadows.DemoControls":scene_==SandboxScene::Lighting?"Lighting.DemoControls":scene_==SandboxScene::Hlod?"Hlod.DemoControls":scene_==SandboxScene::Materials?"Materials.DemoControls":"Camera.DemoControls"));
            controlsReported_ = true;
        }
    }
    if(!device_)return RenderResult::Failed;
    // G15：编辑器 BasicLit 的 LUT 选择随 FrameData 到达；路径变化时重开资产并重建 strip。
    if(BasicLitScene(scene_)&&frame.data&&frame.data->colorLutPath!=editorLutPath_)
    {
        editorLutPath_=frame.data->colorLutPath;
        lutStripReady_=false;lutStrip_.reset();lutStripRevision_=0;
        if(editorLutPath_.empty())lutFile_.Close();
        else
        {
            std::string error;
            if(lutFile_.Open(editorLutPath_,error))
            {
                Log(LogLevel::Info,localization.GetText("Rendering.LutLoaded")+": "
                    +std::to_string(lutFile_.Status().size)+"^3, revision "+std::to_string(lutFile_.Status().revision));
                UpdateLutStrip(localization);
            }
            else Log(LogLevel::Warning,localization.GetText(error.empty()?"Rendering.LutAssetInvalid":error));
            reportedLutFailures_=lutFile_.Status().failures;
        }
    }
    // G15：`.selut` 热重载。只在文件选择存在时检查时间戳；成功/失败都只记录一次，
    // 失败时继续使用上一版 LUT，不把画面切回未调色路径。
    if(!lutFile_.Path().empty())
    {
        const auto previousRevision=lutFile_.Status().revision;
        lutFile_.Poll();
        const auto& status=lutFile_.Status();
        if(status.revision!=previousRevision)
        {
            Log(LogLevel::Info,localization.GetText("Rendering.LutReloaded")+": revision="
                +std::to_string(status.revision)+", "+std::to_string(status.size)+"^3");
            if(BasicLitScene(scene_)||scene_==SandboxScene::Lighting)UpdateLutStrip(localization);
        }
        else if(status.failures!=reportedLutFailures_)
        {
            reportedLutFailures_=status.failures;
            Log(LogLevel::Warning,localization.GetText("Rendering.LutReloadFailed")+": "
                +localization.GetText(status.error.empty()?"Rendering.LutAssetInvalid":status.error)
                +(status.detail.empty()?"":", "+status.detail));
        }
    }
    const bool cascadedFrame=cascadeRequested&&camera_&&!sceneInstancing&&!customCascadeUnsupported
        &&(BasicLitScene(scene_)||scene_==SandboxScene::Shadows||EnvironmentScene(scene_));
    if(cascadeRequested&&frame.data->shadow.enabled&&!cascadedFrame&&reportedDiagnostics_.insert("Rendering.CsmUnavailable").second)
        Log(LogLevel::Warning,localization.GetText("Rendering.CsmUnavailable"));
    if(cascadedFrame&&!PrepareCascadedPipelines())return RenderResult::Failed;
    const bool clusteredFrame=clusterRequested&&!sceneInstancing&&!customCascadeUnsupported&&camera_&&ForwardScene(scene_);
    const std::size_t clusterGroup=cascadedFrame?2:scene_==SandboxScene::Lighting?0:1;
    if(clusterRequested&&!clusteredFrame&&reportedDiagnostics_.insert("Rendering.ClusterUnavailable").second)
        Log(LogLevel::Warning,localization.GetText("Rendering.ClusterUnavailable"));
    if(clusteredFrame&&!PrepareClusteredPipelines(clusterGroup))return RenderResult::Failed;
    const bool shadowFilterRequested=frame.data&&frame.data->shadow.filter.pcss;
    const bool shadowFilterFrame=shadowFilterRequested&&cascadedFrame&&frame.data->shadow.enabled&&frame.data->shadow.pcf;
    if(shadowFilterRequested&&frame.data->shadow.enabled&&!shadowFilterFrame&&reportedDiagnostics_.insert("Rendering.ShadowFilterUnavailable").second)
        Log(LogLevel::Warning,localization.GetText("Rendering.ShadowFilterUnavailable"));
    if(shadowFilterFrame&&!PrepareShadowFilterPipelines(clusteredFrame))return RenderResult::Failed;
    const auto& filterPipelines=shadowFilterPipelines_[clusteredFrame?1:0];
    const auto& clusterPipelines=clusteredPipelines_[clusterGroup];
    const auto clusteredKind=clusterGroup==0?se::rhi::PipelineKind::ClusteredForwardMesh:
        featureEnvironment_||EnvironmentScene(scene_)?(cascadedFrame?se::rhi::PipelineKind::ClusteredCascadedEnvironmentMesh:se::rhi::PipelineKind::ClusteredEnvironmentMesh):
        BasicLitScene(scene_)&&!featureFullPbr_?(cascadedFrame?se::rhi::PipelineKind::ClusteredCascadedBasicLitMesh:se::rhi::PipelineKind::ClusteredBasicLitMesh):
        cascadedFrame?se::rhi::PipelineKind::ClusteredCascadedShadowedMesh:se::rhi::PipelineKind::ClusteredShadowedMesh;
    models_.SetMaterialPipelineKind(clusteredFrame?clusteredKind:cascadedFrame?(featureEnvironment_||EnvironmentScene(scene_)?se::rhi::PipelineKind::CascadedEnvironmentMesh:
        BasicLitScene(scene_)&&!featureFullPbr_?se::rhi::PipelineKind::CascadedBasicLitMesh:se::rhi::PipelineKind::CascadedShadowedMesh):
        featureEnvironment_||EnvironmentScene(scene_)?se::rhi::PipelineKind::EnvironmentMesh:BasicLitScene(scene_)||scene_==SandboxScene::Shadows?se::rhi::PipelineKind::ShadowedMesh:ForwardScene(scene_)?se::rhi::PipelineKind::ForwardMesh:se::rhi::PipelineKind::PbrMesh);
    models_.SetCustomMaterialPipelinesEnabled(!sceneInstancing&&!cascadedFrame&&!clusteredFrame&&!coverageFrame&&!shadingFrame);
    const bool instancing=sceneInstancing;
    // G06.10：Forward 实例化本轮只接 Lighting（无阴影/环境的 FinalOutput HDR 路径）。
    const bool forwardInstancing=instancing&&(scene_==SandboxScene::Lighting||BasicLitScene(scene_));
    if(instancing&&!MaterialScene(scene_)&&!forwardInstancing){Log(LogLevel::Error,localization.GetText("Rhi.Unsupported"));return RenderResult::Failed;}
    if(instancing&&!instancedPipeline_)
        if(!device_->CreatePipeline({se::rhi::PipelineKind::PbrInstancedMesh,ResourceDirectory()/"Shaders/PbrInstanced.hlsl",se::rhi::InstancingLayoutVersion},instancedPipeline_))return RenderResult::Failed;
    if(forwardInstancing&&!forwardInstancedPipeline_)
    {
        se::rhi::PipelineDescription description{se::rhi::PipelineKind::PbrForwardInstancedMesh,
            ResourceDirectory()/"Shaders/PbrForwardInstanced.hlsl",se::rhi::ForwardInstancingLayoutVersion};
        description.colorFormat=se::rhi::ColorFormat::Rgba16Float;
        if(!device_->CreatePipeline(description,forwardInstancedPipeline_))return RenderResult::Failed;
    }
    // G06.08：遮挡主 PSO 只着色、不写深度；prepass 已把候选深度写入同一目标。
    if(instancing&&occlusionEnabled_&&!forwardInstancing&&!occlusionPipeline_)
    {
        se::rhi::PipelineDescription description{se::rhi::PipelineKind::PbrInstancedMesh,
            ResourceDirectory()/"Shaders/PbrInstanced.hlsl",se::rhi::InstancingLayoutVersion};
        description.depth=se::rhi::DepthMode::ReadOnlyLessEqual;
        if(!device_->CreatePipeline(description,occlusionPipeline_))
        {
            Log(LogLevel::Warning,localization.GetText("Rhi.Unsupported"));
            occlusionEnabled_=false;
        }
    }
    if(forwardInstancing&&occlusionEnabled_&&!forwardOcclusionPipeline_)
    {
        se::rhi::PipelineDescription description{se::rhi::PipelineKind::PbrForwardInstancedMesh,
            ResourceDirectory()/"Shaders/PbrForwardInstanced.hlsl",se::rhi::ForwardInstancingLayoutVersion};
        description.colorFormat=se::rhi::ColorFormat::Rgba16Float;
        description.depth=se::rhi::DepthMode::ReadOnlyLessEqual;
        if(!device_->CreatePipeline(description,forwardOcclusionPipeline_))
        {
            Log(LogLevel::Warning,localization.GetText("Rhi.Unsupported"));
            occlusionEnabled_=false;
        }
    }
    device_->SetCpuProfiler(frame.profiler);
    struct ProfilerBinding
    {
        se::rhi::Device& device;
        ~ProfilerBinding(){device.SetCpuProfiler(nullptr);}
    } binding{*device_};
    if(!device_->EnableGpuTiming(frame.profiler&&frame.profiler->Enabled()))return RenderResult::Failed;
    auto commands=device_->BeginCommands(frame.updateIndex);
    if(!commands)return RenderResult::Failed;
    const bool forward=ForwardScene(scene_);
    const bool editor=scene_==SandboxScene::BasicLit&&frame.data&&frame.data->editorGuiEnabled;
    se::rhi::Result result;
    if(editor)
    {
        const auto scaleX=frame.data->editorGui.framebufferScale.x,scaleY=frame.data->editorGui.framebufferScale.y;
        const auto width=static_cast<std::uint32_t>(std::max(1l,std::lround(frame.data->sceneViewport.width*(scaleX>0?scaleX:1))));
        const auto height=static_cast<std::uint32_t>(std::max(1l,std::lround(frame.data->sceneViewport.height*(scaleY>0?scaleY:1))));
        if(!editorTarget_)editorTarget_=std::make_unique<se::EditorViewportTargetResource>(*device_,width,height);
        std::string targetError;
        if(!editorTarget_->Resize(width,height,targetError)){Log(LogLevel::Error,localization.GetText(targetError));return RenderResult::Failed;}
        if(!editorGui_)editorGui_=std::make_unique<se::GuiCompositor>();
        se::rhi::FrameComposition composition;composition.scene=editorTarget_->Targets();
        result=commands->ConfigureComposition(composition);
        if(result)result=commands->ConfigureSceneOverlay(frame.data->editorSceneLines,ResourceDirectory()/"Shaders/SceneOverlay.hlsl",scaleX>0?scaleX:1,frame.data->editorGridEnabled);
        if(result)result=editorGui_->Configure(*device_,*commands,*frame.data,editorTarget_->LdrColor(),frame.surface.width,frame.surface.height,ResourceDirectory()/"Shaders");
    }
    else if(frame.data&&frame.data->editorGuiEnabled)result=gui_.Configure(*device_,*commands,*frame.data,frame.surface.width,frame.surface.height,ResourceDirectory()/"Shaders");
    if(result&&frame.data&&!frame.data->referenceUiOverlays)
    {
        std::vector<se::AssetDiagnostic> diagnostics;
        // G06.09：HUD 只读已完成回读；与遮挡批次同条件收集视觉项，再由 GameUI 在场景后绘制。
        std::vector<se::UiVisual> hudVisuals;
        if(occlusionHudEnabled_&&occlusionCommitted_&&instancing&&camera_&&!lodEnabled_&&!hlodEnabled_&&!impostorEnabled_
            &&!frame.data->editorGuiEnabled&&frame.data->debugLines.empty())
            hudVisuals=se::BuildOcclusionHud(device_->Stats().occlusion,localization,
                frame.surface.width,frame.surface.height);
        result=gameUi_.Configure(*device_,*commands,*frame.data,frame.surface.width,frame.surface.height,ResourceDirectory()/"Shaders",diagnostics,hudVisuals);
        for(const auto& diagnostic:diagnostics)if(reportedDiagnostics_.insert(diagnostic.key+":"+diagnostic.detail).second)
            Log(LogLevel::Warning,localization.GetText(diagnostic.key)+": "+diagnostic.detail);
    }
    if(result&&!forward){result=commands->BeginPass({se::rhi::PassKind::Clear,se::rhi::ResourceUse::ColorWrite,se::rhi::ResourceUse::DepthWrite,"Clear"});if(result)result=commands->EndPass();}
    const auto viewProjectionBase=camera_?camera_->viewProjection:se::Mat4{};
    const auto framebufferScale=editor?frame.data->editorGui.framebufferScale:se::Vec2{1,1};
    const auto sceneWidth=editor?static_cast<std::uint32_t>(std::max(1l,std::lround(frame.data->sceneViewport.width*(framebufferScale.x>0?framebufferScale.x:1)))):frame.surface.width;
    const auto sceneHeight=editor?static_cast<std::uint32_t>(std::max(1l,std::lround(frame.data->sceneViewport.height*(framebufferScale.y>0?framebufferScale.y:1)))):frame.surface.height;
    const auto geometryScale=se::rhi::SceneRasterScale(postFrame&&frame.data&&frame.data->post&&frame.data->post->supersample4x,sceneWidth,sceneHeight);
    const bool taaFrame=taaEnabled_&&postFrame&&frame.surface.width&&frame.surface.height;
    const auto viewProjection=taaFrame?se::ApplyProjectionJitter(viewProjectionBase,
        se::HaltonJitter(static_cast<std::uint32_t>(frame.updateIndex)),sceneWidth*geometryScale,sceneHeight*geometryScale):viewProjectionBase;
    if(result&&forward)result=commands->ConfigureView(viewProjection);
    if(result&&forward&&frame.data&&!frame.data->particles.batches.empty())
    {
        auto particles=frame.data->particles;
        if(camera_)
        {
            const auto& inv=camera_->inverseView;
            particles.cameraRight={inv(0,0),inv(1,0),inv(2,0)};
            particles.cameraUp={inv(0,1),inv(1,1),inv(2,1)};
        }
        result=commands->ConfigureParticles(particles,ResourceDirectory()/"Shaders/ParticleBillboard.hlsl");
    }
    const se::rhi::DepthNormalSettings geometrySettings{frame.data&&camera_&&(frame.data->depthNormals||frame.data->gtao||frame.data->contactShadows||frame.data->gpuDrivenHiZ||frame.data->indoor.Enabled())};
    std::optional<se::rhi::ScreenSpaceOcclusionSettings> screenSettings;
    if(frame.data&&camera_&&(frame.data->gtao||frame.data->contactShadows))
    {
        screenSettings.emplace();screenSettings->gtao=frame.data->gtao;screenSettings->contactShadows=frame.data->contactShadows;
        screenSettings->view=camera_->view;screenSettings->clipFromView=viewProjection*camera_->inverseView;
        screenSettings->radius=frame.data->aoRadius;screenSettings->intensity=frame.data->aoIntensity;screenSettings->contactLength=frame.data->contactLength;
    }
    if(result&&scene_==SandboxScene::Triangle&&(!frame.data||frame.data->cameras.empty()||camera_))
    {
        result=commands->BeginPass({se::rhi::PassKind::Triangle,se::rhi::ResourceUse::ColorWrite,se::rhi::ResourceUse::DepthWrite,"Triangle"});
        if(result)result=commands->Triangle(*pipeline_,viewProjection);
        if(result)result=commands->EndPass();
        if(result)drawStatistics_.draws=1;
    }
    if(result&&(scene_==SandboxScene::TexturedCube||scene_==SandboxScene::Scene||MaterialScene(scene_)||HlodSceneMode(scene_)||forward))
    {
        std::vector<se::rhi::DrawIndexed> meshes,casters,transparent;
        const auto opaquePipeline=shadowFilterFrame?(featureEnvironment_||EnvironmentScene(scene_)?*filterPipelines.environment:
            BasicLitScene(scene_)&&!featureFullPbr_?*filterPipelines.basic:*filterPipelines.pbr):
            clusteredFrame?(clusterGroup==0?*clusterPipelines.pbr:
            featureEnvironment_||EnvironmentScene(scene_)?*clusterPipelines.environment:
            BasicLitScene(scene_)&&!featureFullPbr_?*clusterPipelines.basic:*clusterPipelines.pbr):
            cascadedFrame?(featureEnvironment_||EnvironmentScene(scene_)?*cascadedEnvironment_:
            BasicLitScene(scene_)&&!featureFullPbr_?*cascadedBasic_:*cascadedPbr_):
            instancing?(forwardInstancing?*forwardInstancedPipeline_:*instancedPipeline_):*pipeline_;
        struct Candidate {const se::MeshInstance* instance;std::shared_ptr<const se::StaticRenderModel> model;bool visible=true,missing=false;};
        std::vector<Candidate> candidates;
        // G06.08：只有单批次、无 LOD/HLOD/Impostor/调试线的帧才收集遮挡包围盒；
        // 包围盒必须与 meshes/batches 的顺序一一对应。
        const bool occlusionWanted=occlusionEnabled_&&instancing&&!lodEnabled_&&!hlodEnabled_&&!impostorEnabled_
            &&frame.data&&frame.data->debugLines.empty();
        std::vector<se::Aabb> meshBounds;bool occlusionBoundsValid=true;
        bounds_.BeginFrame();
        std::vector<se::AssetId> needed,neededMaterials;
        if(frame.data)for(const auto& mesh:frame.data->meshes)
        {
            if(!se::IsBuiltinMesh(mesh.mesh.meshAsset))needed.push_back(mesh.mesh.meshAsset);
            for(const auto& id:mesh.mesh.materials)if(!id.empty())neededMaterials.push_back(id);
        }
        se::Vec3 lightDirection{0.3f,0.5f,0.8f},radiance=scene_==SandboxScene::Scene?se::Vec3{3,3,3}:se::Vec3{};
        if(scene_!=SandboxScene::Scene&&frame.data&&!frame.data->lights.empty())
        {
            const auto& light=frame.data->lights.front();lightDirection=se::Normalize(se::TransformVector(light.world,{0,0,1}));
            radiance=light.light.color*light.light.intensity;
        }
        const auto report=[&](const auto& diagnostics){for(const auto& diagnostic:diagnostics)
            if(reportedDiagnostics_.insert(diagnostic.key+":"+diagnostic.detail).second)
                Log(diagnostic.warning?LogLevel::Warning:LogLevel::Error,localization.GetText(diagnostic.key)+": "+diagnostic.detail);};
        if(scene_==SandboxScene::Scene&&frame.data)
        {
            std::vector<se::AssetDiagnostic> diagnostics;const auto lighting=se::PrepareForwardLighting(frame.data->lights,1,{},diagnostics);report(diagnostics);
            if(lighting.directionalEnabled){lightDirection=lighting.directionToLight;radiance=lighting.directionalRadiance;}
        }
        std::unordered_map<se::AssetId,std::shared_ptr<const se::StaticRenderModel>> frameModels;
        std::unordered_map<se::AssetId,std::shared_ptr<const se::PbrRenderMaterial>> frameMaterials;
        if(camera_&&frame.data)for(const auto& mesh:frame.data->meshes)
        {
            std::shared_ptr<const se::StaticRenderModel> model;
            if(mesh.mesh.meshAsset==se::BuiltinCubeAssetId)model=cube_;
            else if(mesh.mesh.meshAsset==se::BuiltinSphereAssetId)model=sphere_;
            else if((scene_==SandboxScene::Scene||MaterialScene(scene_)||forward)&&frame.data->assets)
            {
                auto found=frameModels.find(mesh.mesh.meshAsset);
                if(found==frameModels.end())
                {
                    std::vector<se::AssetDiagnostic> diagnostics;
                    auto loaded=models_.Load(*frame.data->assets,mesh.mesh.meshAsset,*device_,diagnostics);
                    found=frameModels.emplace(mesh.mesh.meshAsset,std::move(loaded)).first;
                    report(diagnostics);
                }
                model=found->second;
            }
            const bool missing=!model;
            if(missing&&scene_==SandboxScene::Scene)model=cube_;
            if(!model)continue;
            candidates.push_back({&mesh,std::move(model),true,missing});
        }
        if(camera_&&frame.data&&frame.data->frustumCulling)
        {
            se::CpuScope visibility(frame.profiler,"CPU.Visibility");const auto frustum=se::BuildFrustum(viewProjection);
            for(auto& candidate:candidates)
            {
                const auto box=bounds_.Resolve(candidate.instance->entity,candidate.instance->world,candidate.model);
                candidate.visible=!frustum||!box||se::Intersects(*frustum,*box);
                drawStatistics_.culled+=!candidate.visible;
            }
        }
        // G06.08：遮挡批次也需要逐实例包围盒；EndFrame 统一放到绘制列表构造之后。
        std::array<std::uint32_t,8> lodLevelCounts{};float lodMaximumBlend=0.0f,lodDistance=-1.0f;
        // G05.10：HLOD 运行时接入。按实例集合烘焙一次单元代理，随后逐单元做屏幕误差 + 迟滞选择；
        // 选中单元的实例不再单独绘制，改由单元代理覆盖（每个实例只属于一个单元）。
        std::vector<bool> proxyCandidate(candidates.size(),false),proxySelected;
        std::size_t proxyDrawCount=0,instanceDrawCount=0,residentCount=0,impostorDrawCount=0;
        if(hlodEnabled_&&camera_&&frame.data)
        {
            if(hlodAsync_&&hlodStreamer_.Running())
            {
                se::HlodScene streamed;std::string streamError;
                const auto streamState=hlodStreamer_.Poll(streamed,streamError);
                if(streamState==se::HlodStreamer::State::Ready)
                {
                    hlodStream_.Adopt(std::move(streamed));hlodFileLoaded_=true;hlodBaked_=false;
                    Log(LogLevel::Info,localization.GetText("Rendering.HlodStreamReady")+": "
                        +std::to_string(hlodStream_.Scene().cells.size()));
                }
                else if(streamState==se::HlodStreamer::State::Failed)
                {
                    hlodAsync_=false;hlodLazyProxies_=hlodVramBudget_>0||hlodUploadBudget_>0;
                    Log(LogLevel::Warning,localization.GetText(streamError.empty()?"Assets.ReadFailed":streamError));
                    std::string readError;
                    hlodFileLoaded_=hlodStream_.Load(hlodFilePath_,readError);
                    hlodBaked_=false;
                    if(hlodFileLoaded_)Log(LogLevel::Info,localization.GetText("Rendering.HlodLoaded")+": "
                        +std::to_string(hlodStream_.Scene().cells.size()));
                    else Log(LogLevel::Warning,localization.GetText(readError.empty()?"Assets.ReadFailed":readError));
                }
            }
            if(!hlodFailed_&&!(hlodAsync_&&hlodStreamer_.Running())&&(!hlodBaked_||hlodInstanceCount_!=candidates.size()))
            {
                hlodBaked_=false;
                se::HlodScene baked;bool haveScene=false,haveAssignments=false;
                std::vector<se::HlodCellCoordinate> assignments;
                std::vector<std::size_t> instanceCandidates;
                std::vector<se::AssetDiagnostic> diagnostics;
                std::unordered_map<std::string,std::shared_ptr<const se::PbrRenderMaterial>> embeddedMaterials;
                if(hlodFileLoaded_&&hlodStream_.Loaded())
                {
                    baked=hlodStream_.Scene();haveScene=true;
                }
                else
                {
                    std::vector<se::HlodInstance> instances;
                    for(std::size_t index=0;index<candidates.size();++index)
                    {
                        const auto& candidate=candidates[index];const auto& model=*candidate.model;
                        for(const auto& part:model.layout.parts)
                        {
                            if(part.mesh>=model.source->meshes.size()||part.mesh>=model.meshes.size())continue;
                            const auto& sourceMesh=model.source->meshes[part.mesh];
                            const auto& renderMesh=model.meshes[part.mesh];
                            const std::vector<std::uint32_t>* indices=&sourceMesh.indices;
                            float error=0;
                            if(renderMesh.lod&&hlodLevel_<renderMesh.lod->levels.size())
                            {
                                indices=&renderMesh.lod->levels[hlodLevel_].indices;
                                error=renderMesh.lod->levels[hlodLevel_].worldError;
                            }
                            std::string material;
                            if(sourceMesh.material>=0&&static_cast<std::size_t>(sourceMesh.material)<candidate.instance->mesh.materials.size())material=candidate.instance->mesh.materials[sourceMesh.material];
                            if(material.empty()&&renderMesh.material)
                            {material="embedded:"+std::to_string(candidate.instance->entity)+":"+std::to_string(part.mesh);embeddedMaterials.emplace(material,renderMesh.material);}
                            instances.push_back({&sourceMesh,indices,candidate.instance->world*part.modelFromMesh,error,std::move(material)});
                            instanceCandidates.push_back(index);
                        }
                    }
                    haveScene=se::BakeHlod(instances,se::HlodCellSize,baked,diagnostics,&assignments);
                    haveAssignments=haveScene;
                    if(haveScene&&!hlodSavePath_.empty()&&!hlodSaved_)
                    {
                        std::string saveError;
                        if(se::WriteHlodScene(hlodSavePath_,baked,saveError))
                        {
                            hlodSaved_=true;
                            Log(LogLevel::Info,localization.GetText("Rendering.HlodSaved")+": "
                                +std::to_string(baked.cells.size()));
                        }
                        else Log(LogLevel::Warning,localization.GetText(saveError.empty()?"Assets.WriteFailed":saveError));
                    }
                }
                if(haveScene)
                {
                    hlodCells_=baked.cells;hlodProxies_.clear();hlodOverrides_.clear();
                    hlodProxyActive_.assign(hlodCells_.size(),false);
                    hlodProxies_.assign(hlodCells_.size(),nullptr);hlodProxyBytes_.assign(hlodCells_.size(),0);
                    hlodProxyCache_.Configure(hlodVramBudget_,hlodUploadBudget_);
                    bool uploaded=true;
                    for(std::size_t cellIndex=0;cellIndex<hlodCells_.size();++cellIndex)
                    {
                        const auto& cell=hlodCells_[cellIndex];
                        auto proxy=std::make_shared<se::StaticModel>();
                        std::vector<std::shared_ptr<const se::PbrRenderMaterial>> overrides;
                        for(const auto& part:cell.parts)
                        {
                            se::MeshData partMesh=part.mesh;partMesh.material=static_cast<std::int32_t>(proxy->materials.size());
                            proxy->meshes.push_back(std::move(partMesh));proxy->materials.emplace_back();
                            std::shared_ptr<const se::PbrRenderMaterial> material;
                            if(const auto found=embeddedMaterials.find(part.material);found!=embeddedMaterials.end())material=found->second;
                            else if(!part.material.empty()&&frame.data->assets)
                                material=models_.LoadMaterial(*frame.data->assets,part.material,*device_,diagnostics);
                            overrides.push_back(std::move(material));
                        }
                        se::ModelNode node;node.name="HlodProxy";
                        for(std::uint32_t index=0;index<proxy->meshes.size();++index)node.meshes.push_back(index);
                        proxy->nodes.push_back(std::move(node));
                        hlodOverrides_.push_back(std::move(overrides));
                        std::uint64_t bytes=0;
                        for(const auto& part:cell.parts)
                            bytes+=UINT64{part.mesh.vertices.size()}*sizeof(se::MeshVertex)+UINT64{part.mesh.indices.size()}*4;
                        hlodProxyBytes_[cellIndex]=bytes;
                        if(hlodLazyProxies_)continue;
                        std::shared_ptr<const se::StaticRenderModel> render;
                        if(!models_.Upload(proxy,{},ResourceDirectory(),*device_,render,diagnostics,false,0)){uploaded=false;break;}
                        hlodProxies_[cellIndex]=std::move(render);
                        hlodProxyCache_.Insert(static_cast<std::uint32_t>(cellIndex),bytes);
                    }
                    if(!uploaded)
                    {
                        hlodProxies_.clear();hlodOverrides_.clear();hlodProxyBytes_.clear();hlodCells_.clear();hlodFailed_=true;
                        if(reportedDiagnostics_.insert("HLOD:upload").second)
                            Log(LogLevel::Warning,localization.GetText("Rendering.LodAssetInvalid"));
                    }
                    else
                    {
                        hlodCandidateCell_.assign(candidates.size(),-1);
                        if(haveAssignments)
                        {
                            std::vector<se::HlodCellCoordinate> candidateCoordinates(candidates.size());
                            std::vector<char> assigned(candidates.size(),0);
                            for(std::size_t instance=0;instance<assignments.size()&&instance<instanceCandidates.size();++instance)
                            {
                                const auto candidate=instanceCandidates[instance];
                                if(candidate>=candidates.size()||assigned[candidate])continue;
                                candidateCoordinates[candidate]=assignments[instance];assigned[candidate]=1;
                            }
                            for(std::size_t candidate=0;candidate<candidates.size();++candidate)
                            {
                                if(!assigned[candidate])continue;
                                for(std::size_t cell=0;cell<hlodCells_.size();++cell)
                                    if(hlodCells_[cell].coordinate==candidateCoordinates[candidate])
                                    {
                                        hlodCandidateCell_[candidate]=static_cast<std::int32_t>(cell);break;
                                    }
                            }
                        }
                        else
                        {
                            // 文件路径：文件单元按烘焙时的世界包围盒中心划分，这里用候选实例的首个部件中心匹配。
                            for(std::size_t candidate=0;candidate<candidates.size();++candidate)
                            {
                                const auto& model=*candidates[candidate].model;
                                if(model.layout.parts.empty())continue;
                                const auto& minimum=model.layout.minimum;const auto& maximum=model.layout.maximum;
                                const se::Vec3 localCenter{(minimum.x+maximum.x)*0.5f,(minimum.y+maximum.y)*0.5f,(minimum.z+maximum.z)*0.5f};
                                const auto world=model.layout.parts.front().modelFromMesh*candidates[candidate].instance->world;
                                const auto coordinate=se::HlodCellFor(se::TransformPoint(world,localCenter),se::HlodCellSize);
                                for(std::size_t cell=0;cell<hlodCells_.size();++cell)
                                    if(hlodCells_[cell].coordinate==coordinate)
                                    {
                                        hlodCandidateCell_[candidate]=static_cast<std::int32_t>(cell);break;
                                    }
                            }
                        }
                    }
                }
                else hlodFailed_=true;
                hlodInstanceCount_=candidates.size();hlodBaked_=true;
                if(!diagnostics.empty())report(diagnostics);
            }
            if(!hlodFailed_&&!hlodCells_.empty()&&hlodCandidateCell_.size()==candidates.size())
            {
                se::HlodView view;view.position=camera_->position;view.viewportHeight=static_cast<float>(frame.surface.height);
                // 驻留：radius<=0 全部驻留；否则平面距离（含单元半径）内才允许使用代理，其余回退实例。
                residentCount=hlodCells_.size();
                std::vector<char> resident(hlodCells_.size(),1);
                if(hlodResidentRadius_>0)
                {
                    residentCount=0;
                    for(std::size_t cell=0;cell<hlodCells_.size();++cell)
                    {
                        const auto& entry=hlodCells_[cell];
                        const float dx=entry.center.x-camera_->position.x,dz=entry.center.z-camera_->position.z;
                        const bool keep=std::sqrt(dx*dx+dz*dz)<=hlodResidentRadius_+entry.radius;
                        resident[cell]=keep?1:0;residentCount+=keep?1:0;
                    }
                }
                proxySelected.assign(hlodCells_.size(),false);
                for(std::size_t cell=0;cell<hlodCells_.size();++cell)
                {
                    if(!resident[cell])continue;
                    const bool current=cell<hlodProxyActive_.size()&&hlodProxyActive_[cell];
                    proxySelected[cell]=se::SelectHlodProxy(hlodCells_[cell],view,1.5f,current);
                }
                if(hlodLazyProxies_)
                {
                    // 每帧只上传预算内的选中单元；未就绪/超预算的单元本帧回退实例。
                    std::size_t uploadedNow=0,releasedNow=0,uploadFailures=0;
                    hlodProxyCache_.BeginFrame();
                    for(std::size_t cell=0;cell<hlodCells_.size();++cell)if(proxySelected[cell])hlodProxyCache_.Touch(static_cast<std::uint32_t>(cell));
                    for(std::size_t cell=0;cell<hlodCells_.size();++cell)
                    {
                        if(!proxySelected[cell]||hlodProxies_[cell])continue;
                        const auto bytes=hlodProxyBytes_[cell];
                        if(!hlodProxyCache_.CanUpload()||!hlodProxyCache_.Fits(bytes))
                        {proxySelected[cell]=false;continue;}
                        auto proxy=std::make_shared<se::StaticModel>();
                        for(const auto& part:hlodCells_[cell].parts)
                        {
                            se::MeshData partMesh=part.mesh;partMesh.material=static_cast<std::int32_t>(proxy->materials.size());
                            proxy->meshes.push_back(std::move(partMesh));proxy->materials.emplace_back();
                        }
                        se::ModelNode node;node.name="HlodProxy";
                        for(std::uint32_t index=0;index<proxy->meshes.size();++index)node.meshes.push_back(index);
                        proxy->nodes.push_back(std::move(node));
                        std::vector<se::AssetDiagnostic> uploadDiagnostics;
                        std::shared_ptr<const se::StaticRenderModel> render;
                        if(!models_.Upload(proxy,{},ResourceDirectory(),*device_,render,uploadDiagnostics,false,0))
                        {proxySelected[cell]=false;++uploadFailures;continue;}
                        hlodProxies_[cell]=std::move(render);
                        hlodProxyCache_.Insert(static_cast<std::uint32_t>(cell),bytes);
                        hlodProxyCache_.AddUpload(bytes);
                        ++uploadedNow;
                    }
                    std::uint32_t evicted=0;
                    while(hlodProxyCache_.OverBudget()&&hlodProxyCache_.SelectEviction(evicted))
                    {
                        hlodProxies_[evicted].reset();hlodProxyCache_.Erase(evicted);++releasedNow;
                    }
                    for(std::size_t cell=0;cell<hlodCells_.size();++cell)
                    {
                        if(resident[cell]||!hlodProxies_[cell])continue;
                        hlodProxies_[cell].reset();hlodProxyCache_.Erase(static_cast<std::uint32_t>(cell));++releasedNow;
                    }
                    if(uploadedNow||releasedNow||uploadFailures)
                        Log(LogLevel::Debug,localization.GetText("Rendering.HlodProxyBudget")+": uploaded="
                            +std::to_string(uploadedNow)+", released="+std::to_string(releasedNow)+", failures="
                            +std::to_string(uploadFailures)+", bytes="+std::to_string(hlodProxyCache_.Bytes())
                            +", budget="+std::to_string(hlodVramBudget_));
                }
                hlodProxyActive_=proxySelected;
                for(std::size_t candidate=0;candidate<candidates.size();++candidate)
                {
                    const auto cell=hlodCandidateCell_[candidate];
                    if(cell>=0&&static_cast<std::size_t>(cell)<proxySelected.size())
                        proxyCandidate[candidate]=proxySelected[static_cast<std::size_t>(cell)];
                }
            }
        }
        // W03：调色板字节在本帧内被绘制列表借用，只在帧开始时整体清空；
        // 逐候选清空会让先前绘制的 span 失效。
        skinPaletteStorage_.clear();
        for(std::size_t candidateIndex=0;candidateIndex<candidates.size();++candidateIndex)
        {
            const auto& candidate=candidates[candidateIndex];
            const auto& mesh=*candidate.instance;const auto& model=candidate.model;
            // W03：每演员每帧一份调色板（行主序 3×4，48 字节/关节）。演员没有姿态时
            // 写单位矩阵，蒙皮顶点缓冲仍以绑定姿势出现在原位置。
            const bool skinnedModel=std::any_of(model->meshes.begin(),model->meshes.end(),
                [](const auto& entry){return entry.skinned;});
            std::uint32_t skinJointCount=0;
            std::span<const std::byte> skinPalette;
            if(skinnedModel)skinPalette=BuildSkinPalette(mesh.skinPalette,model->source->joints.size(),skinJointCount);
            auto skinDrawPipeline=!instancing?(shadowFilterFrame?
                (featureEnvironment_||EnvironmentScene(scene_)?*filterPipelines.skinnedEnvironment:*filterPipelines.skinned):clusteredFrame?
                (featureEnvironment_||EnvironmentScene(scene_)?*clusterPipelines.skinnedEnvironment:*clusterPipelines.skinned):cascadedFrame?
                (featureEnvironment_||EnvironmentScene(scene_)?*cascadedSkinnedEnvironment_:*cascadedSkinned_):
                !featureEnvironment_&&skinnedPipeline_?*skinnedPipeline_:se::rhi::PipelineHandle{}):se::rhi::PipelineHandle{};
            if(skinnedModel&&!instancing&&!clusteredFrame&&!cascadedFrame&&(featureEnvironment_||EnvironmentScene(scene_)))
            {
                const auto environmentSkin=surfacePipelines_.Load(*device_,ResourceDirectory()/"Shaders",se::rhi::PipelineKind::EnvironmentMesh,
                    se::MaterialAlphaMode::Opaque,true,false,false,false,shadingFrame);
                if(!environmentSkin){Log(LogLevel::Error,localization.GetText("Rendering.MaterialSurfaceUnsupported"));return RenderResult::Failed;}
                skinDrawPipeline=*environmentSkin;
            }
            const auto skinShadowPipeline=skinnedShadowPipeline_?*skinnedShadowPipeline_:se::rhi::PipelineHandle{};
            // 阴影投射者包含主相机外物体，必须在可见性分支前解析同一份材质覆盖。
            std::vector<std::shared_ptr<const se::PbrRenderMaterial>> overrides;
            for(const auto& id:mesh.mesh.materials)
            {
                auto found=frameMaterials.find(id);
                if(found==frameMaterials.end())
                {
                    std::vector<se::AssetDiagnostic> diagnostics;
                    const auto* preview=frame.data->materialPreview&&frame.data->materialPreview->id==id?&frame.data->materialPreview->document:nullptr;
                    auto material=frame.data->assets&&!id.empty()?models_.LoadMaterial(*frame.data->assets,id,*device_,diagnostics,preview):nullptr;
                    if(material&&variants_.Add(material->featureMask))
                        Log(LogLevel::Debug,localization.GetText("Rendering.MaterialVariant")+": "+se::VariantId(material->featureMask));
                    found=frameMaterials.emplace(id,std::move(material)).first;report(diagnostics);
                }
                overrides.push_back(found->second);
            }
            if((scene_==SandboxScene::Shadows||BasicLitScene(scene_)||EnvironmentScene(scene_))&&mesh.mesh.castsShadow)
                se::AppendShadowModelDraws(*model,mesh.world,casters,0,skinPalette,skinJointCount,skinShadowPipeline,overrides);
            if(!candidate.visible)continue;
            if(candidateIndex<proxyCandidate.size()&&proxyCandidate[candidateIndex])continue;
            ++instanceDrawCount;
            // G05.12：远处实例改画 16 视角 Impostor 四边形；相邻视角在 Shader 内插值，alpha test 保留轮廓。
            if(impostorEnabled_&&!coverageFrame&&!shadingFrame&&camera_&&impostorQuad_&&impostorPipeline_)
            {
                const auto& minimum=model->layout.minimum;const auto& maximum=model->layout.maximum;
                const se::Vec3 localCenter{(minimum.x+maximum.x)*0.5f,(minimum.y+maximum.y)*0.5f,(minimum.z+maximum.z)*0.5f};
                const auto center=se::TransformPoint(mesh.world,localCenter);
                if(se::Length(camera_->position-center)>impostorDistance_)
                {
                    const auto azimuth=std::atan2(camera_->position.x-center.x,camera_->position.z-center.z);
                    std::uint32_t first=0,second=0;float blend=0;
                    se::ImpostorViewPair(azimuth,first,second,blend);
                    const auto extent=(std::max)({maximum.x-minimum.x,maximum.y-minimum.y,maximum.z-minimum.z});
                    const auto scaleX=se::Length(se::Vec3{mesh.world(0,0),mesh.world(1,0),mesh.world(2,0)});
                    const auto scaleY=se::Length(se::Vec3{mesh.world(0,1),mesh.world(1,1),mesh.world(2,1)});
                    const auto scaleZ=se::Length(se::Vec3{mesh.world(0,2),mesh.world(1,2),mesh.world(2,2)});
                    const float worldSize=extent*(std::max)({scaleX,scaleY,scaleZ})*1.05f;
                    const auto quadWorld=se::Translation(center)*se::Rotation(se::AxisAngle({0,1,0},azimuth))*se::Scaling({worldSize,worldSize,1});
                    const auto& quadMesh=impostorQuad_->meshes[0];
                    se::rhi::DrawIndexed impostor;
                    impostor.vertices=*quadMesh.vertices;impostor.indices=*quadMesh.indices;impostor.texture=*impostorTexture_;
                    impostor.pipeline=*impostorPipeline_;impostor.world=quadWorld;impostor.viewProjection=viewProjection;
                    impostor.indexCount=6;impostor.impostorView=float(first);impostor.impostorNext=float(second);impostor.impostorBlend=blend;
                    meshes.push_back(impostor);++impostorDrawCount;continue;
                }
            }
            if(!PbrScene(scene_)&&!forward){const auto first=meshes.size();se::AppendStaticModelDraws(*model,mesh.world,meshes,*pipeline_,viewProjection);drawStatistics_.visible+=meshes.size()>first;continue;}
            const auto first=meshes.size();
            auto drawPipeline=opaquePipeline;
            if(model->compressed&&compressedPipeline_)drawPipeline=*compressedPipeline_;
            std::uint32_t lodLevel=0;
            bool lodTransitionActive=false;float lodBlend=1.0f;std::uint32_t lodPrevious=0;
            if(lodEnabled_&&camera_&&!model->meshes.empty()&&model->meshes[0].lod)
            {
                se::LodView lodView;lodView.position=camera_->position;
                lodView.viewportHeight=static_cast<float>(frame.surface.height);
                const auto& minimum=model->layout.minimum;const auto& maximum=model->layout.maximum;
                const se::Vec3 localCenter{(minimum.x+maximum.x)*0.5f,(minimum.y+maximum.y)*0.5f,(minimum.z+maximum.z)*0.5f};
                auto& transition=lodTransitions_[{model.get(),mesh.entity}];
                const auto delta=std::clamp(static_cast<float>(frame.updateDeltaSeconds),0.0f,0.1f);
                if(forcedLodLevel_!=UINT32_MAX)
                {
                    const auto maximumLevel=static_cast<std::uint32_t>(model->meshes[0].lod->levels.size()-1);
                    lodLevel=(std::min)(forcedLodLevel_,maximumLevel);
                    transition={};
                }
                else lodLevel=se::UpdateModelLod(*model->meshes[0].lod,localCenter,mesh.world,lodView,1.5f,delta,transition);
                if(lodDistance<0.0f)lodDistance=se::Length(camera_->position-se::TransformPoint(mesh.world,localCenter));
                lodTransitionActive=transition.active;
                lodBlend=se::LodBlendWeight(transition);
                lodPrevious=transition.previous;
                if(lodLevel<lodLevelCounts.size())++lodLevelCounts[lodLevel];
                lodMaximumBlend=(std::max)(lodMaximumBlend,lodTransitionActive?lodBlend:0.0f);
            }
            // G05.08：过渡期间旧级与新级各画一次，Bayer 阈值把两组像素分开（互不重叠），
            // 避免两套 LOD 深度互相遮挡；blend 为 0 时只画旧级，为 1 时只画新级。
            const auto previous=previousWorlds_.find(mesh.entity);
            const se::Mat4* previousWorld=previous!=previousWorlds_.end()?&previous->second:nullptr;
            const auto look=se::Vec4{frame.data?frame.data->lookMode:0.0f,frame.data?frame.data->lookBlend:0.0f,3.0f,1.0f};
            const auto appendLod=[&](std::uint32_t level,float dither)
            {
                se::AppendPbrModelDraws(*model,mesh.world,overrides,meshes,drawPipeline,viewProjection,camera_->position,
                    forward?se::Vec3{0,0,1}:lightDirection,forward?se::Vec3{}:radiance,level,dither,previousWorld,look,
                    skinPalette,skinJointCount,skinDrawPipeline);
            };
            if(lodTransitionActive&&lodBlend<1.0f)
            {
                appendLod(lodPrevious,lodBlend>0.0f?-lodBlend:0.0f);
                if(lodBlend>0.0f)appendLod(lodLevel,lodBlend);
            }
            else appendLod(lodLevel,0.0f);
            if(editor&&mesh.entity==frame.data->editorSelected)
                for(auto i=first;i<meshes.size();++i)meshes[i].outline=true;
            drawStatistics_.visible+=meshes.size()>first;
            if(occlusionWanted&&meshes.size()>first)
            {
                const auto box=bounds_.Resolve(mesh.entity,mesh.world,model);
                if(!box)occlusionBoundsValid=false;
                else for(auto index=first;index<meshes.size();++index)meshBounds.push_back(*box);
            }
            for(auto index=first;index<meshes.size();++index){meshes[index].stableId=mesh.entity;meshes[index].castsShadow=mesh.mesh.castsShadow;meshes[index].pbr.receivesShadow=mesh.mesh.receivesShadow;
                if(candidate.missing){meshes[index].texture=meshes[index].metallicRoughnessTexture;meshes[index].tint={1,0,1,1};meshes[index].unlit=true;}}
        }
        bounds_.EndFrame();drawStatistics_.boundsUpdates=bounds_.Updates();
        // G14：本帧结束后把当前世界矩阵留作下一帧速度的“上一帧”。
        {
            std::unordered_map<se::StableEntityId,se::Mat4> currentWorlds;
            for(const auto& candidate:candidates)if(candidate.instance)currentWorlds[candidate.instance->entity]=candidate.instance->world;
            previousWorlds_=std::move(currentWorlds);
        }
        if(hlodEnabled_&&camera_&&frame.data)
        {
            for(std::size_t cell=0;cell<hlodProxies_.size()&&cell<proxySelected.size();++cell)
                if(proxySelected[cell]&&hlodProxies_[cell])
                {
                    se::AppendPbrModelDraws(*hlodProxies_[cell],se::Mat4{},hlodOverrides_[cell],meshes,opaquePipeline,viewProjection,
                        camera_->position,lightDirection,radiance,0,0);
                    ++proxyDrawCount;
                }
        }
        if(camera_&&frame.data&&!frame.data->debugLines.empty()&&(PbrScene(scene_)||forward))
        {
            result=se::AppendPhysicsDebugDraws(*cube_,frame.data->debugLines,opaquePipeline,viewProjection,meshes);
            if(!result){Log(LogLevel::Error,localization.GetText(result.Key()));return RenderResult::Failed;}
            drawStatistics_.debugLines=frame.data->debugLines.size();
        }
        // G04.06：压缩纹理驻留必须在记录绘制前更新 SRV；超预算时降级最久未用的最细 mip。
        if(models_.Bc1TexturesEnabled()||models_.Bc5NormalsEnabled()||models_.Bc7ColorsEnabled())
        {
            const auto residency=models_.UpdateTextureResidency(*device_);
            if(residency.degraded)
                Log(LogLevel::Warning,localization.GetText("Rendering.TextureBudgetExceeded")+": "
                    +std::to_string(residency.loadedBytes)+"/"+std::to_string(residency.budgetBytes)+" B");
        }
        // 在分桶之前统一设置逐绘制质量开关；实例批次保留同一帧设置。
        if(frame.data)for(auto& draw:meshes)
        {draw.pbr.multiscatter=frame.data->multiscatter;draw.pbr.specularAA=frame.data->specularAA;}
        if(coverageFrame||shadingFrame)
        {
            auto kind=clusteredFrame?clusteredKind:cascadedFrame?
                (featureEnvironment_||EnvironmentScene(scene_)?se::rhi::PipelineKind::CascadedEnvironmentMesh:
                 BasicLitScene(scene_)&&!featureFullPbr_?se::rhi::PipelineKind::CascadedBasicLitMesh:se::rhi::PipelineKind::CascadedShadowedMesh):
                featureEnvironment_||EnvironmentScene(scene_)?se::rhi::PipelineKind::EnvironmentMesh:
                BasicLitScene(scene_)&&!featureFullPbr_?se::rhi::PipelineKind::BasicLitMesh:
                BasicLitScene(scene_)||scene_==SandboxScene::Shadows?se::rhi::PipelineKind::ShadowedMesh:
                ForwardScene(scene_)?se::rhi::PipelineKind::ForwardMesh:se::rhi::PipelineKind::PbrMesh;
            const auto shaders=ResourceDirectory()/"Shaders";
            for(auto& draw:casters)if(draw.alphaMode==se::MaterialAlphaMode::Mask)
            {
                const auto pipeline=surfacePipelines_.Load(*device_,shaders,se::rhi::PipelineKind::ShadowDepth,
                    draw.alphaMode,!draw.skinPalette.empty());
                if(!pipeline){Log(LogLevel::Error,localization.GetText("Rendering.MaterialSurfaceUnsupported"));return RenderResult::Failed;}
                draw.pipeline=*pipeline;
            }
            std::vector<se::rhi::DrawIndexed> opaque;opaque.reserve(meshes.size());
            for(auto draw:meshes)
            {
                if(draw.alphaMode!=se::MaterialAlphaMode::Opaque||shadingFrame)
                {
                    const bool skin=!draw.skinPalette.empty();auto materialKind=kind;
                    // 与已有BasicLit蒙皮路径相同：蒙皮受光使用Full PBR技术。
                    if(skin&&se::rhi::IsBasicLit(materialKind))
                        materialKind=se::rhi::IsClustered(materialKind)?(se::rhi::IsCascaded(materialKind)?se::rhi::PipelineKind::ClusteredCascadedShadowedMesh:se::rhi::PipelineKind::ClusteredShadowedMesh):
                            se::rhi::IsCascaded(materialKind)?se::rhi::PipelineKind::CascadedShadowedMesh:se::rhi::PipelineKind::ShadowedMesh;
                    const auto pipeline=surfacePipelines_.Load(*device_,shaders,materialKind,draw.alphaMode,skin,shadowFilterFrame,
                        draw.compressedVertices,models_.Bc5NormalsEnabled()&&!forward,shadingFrame);
                    if(!pipeline){Log(LogLevel::Error,localization.GetText("Rendering.MaterialSurfaceUnsupported"));return RenderResult::Failed;}
                    draw.pipeline=*pipeline;
                }
                if(draw.alphaMode==se::MaterialAlphaMode::Blend){draw.castsShadow=false;transparent.push_back(draw);}
                else opaque.push_back(draw);
            }
            meshes=std::move(opaque);
            if(!transparent.empty()&&(!camera_||!se::SortTransparentDraws(transparent,camera_->view)))return RenderResult::Failed;
        }
        drawStatistics_.draws=meshes.size()+transparent.size();
        drawStatistics_.instances=meshes.size()+transparent.size();
        if(frame.data&&frame.data->sortOpaque&&!occlusionWanted){se::CpuScope sort(frame.profiler,"CPU.OpaqueSort");se::SortOpaqueDraws(meshes);}
        std::span<const se::PbrInstanceBatch> batches;
        if(instancing)
        {
            se::CpuScope batching(frame.profiler,"CPU.Instancing");
            // G06.10：包围盒随绘制进入批次；多批次遮挡按批次取局部 instance 下标。
            const std::span<const se::Aabb> batchBounds=occlusionWanted?std::span<const se::Aabb>(meshBounds):std::span<const se::Aabb>{};
            batches=instances_.Build(meshes,batchBounds);
            drawStatistics_.draws=drawStatistics_.instanceBatches=batches.size();drawStatistics_.instanceCapacity=instances_.InstanceCapacity();
        }
        // G06.08/G06.10：多批次实例化遮挡；每个批次有独立的局部包围盒与 ExecuteIndirect。
        bool occlusionFrame=false;
        std::vector<std::vector<se::VisibilityItem>> occlusionItems;
        std::vector<se::rhi::OcclusionBatch> occlusionBatches;
        se::VisibilityFrustum occlusionFrustum{};
        if(occlusionWanted&&occlusionBoundsValid&&!batches.empty())
        {
            const auto frustum=se::BuildFrustum(viewProjection);
            if(frustum&&se::QuantizeVisibilityFrustum(*frustum,occlusionFrustum))
            {
                bool valid=true;
                occlusionItems.resize(batches.size());
                for(std::size_t batch=0;batch<batches.size();++batch)
                {
                    if(batches[batch].instances.size()!=batches[batch].bounds.size()){valid=false;break;}
                    auto& items=occlusionItems[batch];
                    items.reserve(batches[batch].bounds.size());
                    for(std::size_t index=0;index<batches[batch].bounds.size();++index)
                        items.push_back({batches[batch].bounds[index].minimum,static_cast<std::uint32_t>(index),
                            batches[batch].bounds[index].maximum,0});
                }
                if(valid)
                {
                    occlusionBatches.reserve(batches.size());
                    for(auto& items:occlusionItems)occlusionBatches.push_back({items});
                    occlusionFrame=true;
                }
            }
        }
        {
            std::uint64_t triangles=0;
            if(instancing)for(const auto& batch:batches)triangles+=std::uint64_t(batch.draw.indexCount/3)*batch.instances.size();
            else for(const auto& mesh:meshes)triangles+=mesh.indexCount/3;
            frameTriangles_=triangles;
        }
        if(lodEnabled_&&camera_&&frame.data)
        {
            std::string levels;
            for(std::size_t level=0;level<lodLevelCounts.size();++level)if(lodLevelCounts[level])
                levels+=(levels.empty()?"":",")+std::to_string(level)+":"+std::to_string(lodLevelCounts[level]);
            const auto fixed=[](float value,const char* format){char buffer[32]{};std::snprintf(buffer,sizeof(buffer),format,value);return std::string(buffer);};
            Log(LogLevel::Debug,localization.GetText("Rendering.LodSelection")+": frame="+std::to_string(frame.updateIndex)
                +", dist="+fixed(lodDistance,"%.2f")+", level="+(levels.empty()?"-":levels)
                +", blend="+fixed(lodMaximumBlend,"%.3f")+", tris="+std::to_string(frameTriangles_));
        }
        if(hlodEnabled_&&camera_&&frame.data)
            Log(LogLevel::Debug,localization.GetText("Rendering.HlodCells")+": frame="+std::to_string(frame.updateIndex)
                +", cells="+std::to_string(hlodCells_.size())+", resident="+std::to_string(residentCount)
                +", proxies="+std::to_string(proxyDrawCount)
                +", instances="+std::to_string(instanceDrawCount)+", tris="+std::to_string(frameTriangles_));
        if(impostorEnabled_&&frame.data)
            Log(LogLevel::Debug,localization.GetText("Rendering.Impostor")+": frame="+std::to_string(frame.updateIndex)
                +", draws="+std::to_string(impostorDrawCount)+", tris="+std::to_string(frameTriangles_));
        const bool cacheBindings=frame.data&&frame.data->cacheOpaqueBindings;
        if(forward)
        {
            std::vector<se::AssetDiagnostic> diagnostics;
            auto lighting=se::PrepareForwardLighting(frame.data?std::span<const se::LightInstance>(frame.data->lights):std::span<const se::LightInstance>{},
                frame.data?frame.data->exposure:1,frame.data?frame.data->skyColor:se::Vec3{},diagnostics,clusteredFrame);
            if(!camera_){lighting.localLights.clear();lighting.shading.enabled=false;}
            // 无深度阴影 pass 的旧 Lighting 调试模式仍可显示聚光；正式编辑器与 Player 均有阴影 pass。
            if(scene_==SandboxScene::Lighting)
            {
                bool disabled=false;
                for(auto& light:lighting.localLights)if(light.castsShadow){light.castsShadow=false;disabled=true;}
                if(disabled)se::asset_io::Diagnostic(diagnostics,"Rendering.LocalShadowsUnavailable",{},true);
            }
            report(diagnostics);
            if(frame.data)
            {
                lighting.gpuDriven=frame.data->gpuDriven||frame.data->meshletCulling||frame.data->gpuDrivenHiZ;
                lighting.meshletCulling=frame.data->meshletCulling;
                lighting.gpuDrivenHiZ=frame.data->gpuDrivenHiZ;
                if(camera_&&frame.data->indoor.Enabled())
                {
                    const auto indoorSceneToken=ComputeIndoorSceneToken(scene_,frame.data);
                    lighting.indoor=frame.data->indoor;
                    lighting.indoor.view=camera_->view;
                    lighting.indoor.clipFromView=viewProjection*camera_->inverseView;
                    lighting.indoor.previousClipFromWorld=previousViewProjection_;
                    lighting.indoor.frameIndex=static_cast<std::uint32_t>(frame.updateIndex);
                    lighting.indoor.sceneToken=indoorSceneToken;
                    lighting.indoor.historyValid=indoorHistoryValid_&&previousViewProjectionValid_
                        &&lastSceneWidth_==sceneWidth&&lastSceneHeight_==sceneHeight
                        &&lastIndoorSceneToken_==indoorSceneToken;
                }
            }
            if(shadingFrame&&camera_&&frame.data)
            {
                auto environment=defaultEnvironment;
                if(frame.data->assets&&!frame.data->environment.asset.empty())
                    environment=environments_.Load(*frame.data->assets,frame.data->environment.asset,*device_,diagnostics);
                shadingResources_.Prepare(*frame.data,*camera_,environment.get(),*device_,ResourceDirectory(),lighting.shading,diagnostics);
                const auto available=[&](se::rhi::DrawIndexed& draw)
                {
                    if(lighting.shading.constants.probeOrigin.w<.5f)draw.layers.constants.sheen={};
                    if(lighting.shading.constants.probeDimensions.w<.5f)draw.layers.constants.subsurface.x=0;
                };
                for(auto& draw:meshes)available(draw);for(auto& draw:transparent)available(draw);
                lighting.shading.enabled=true;report(diagnostics);
            }
            if(clusteredFrame)
            {
                lighting.cluster.debug=frame.data->clusterDebug;
                lighting.cluster.counterReadback=true;
                lighting.cluster.view=camera_->view;
                lighting.cluster.clipFromView=viewProjection*camera_->inverseView;
                lighting.cluster.nearPlane=camera_->lens.nearPlane;lighting.cluster.farPlane=camera_->lens.farPlane;
            }
            if(frame.data&&frame.data->sky.enabled)
            {
                // W07：项目天空覆盖；太阳方向仍来自场景方向光，圆盘与品牌合成共用同一方向。
                lighting.skySettings.zenithColor=frame.data->sky.zenith;
                lighting.skySettings.horizonColor=frame.data->sky.horizon;
                lighting.skySettings.groundColor=frame.data->sky.ground;
                lighting.skySettings.sunColor=frame.data->sky.sunColor;
                lighting.skySettings.sunIntensity=frame.data->sky.sunIntensity;
                lighting.skySettings.sunAngularRadius=frame.data->sky.sunAngularRadius;
            }
            if(editor&&!frame.data->editorSkyEnabled)
            {
                lighting.skySettings.zenithColor=lighting.skySettings.horizonColor=lighting.skySettings.groundColor=frame.data->skyColor;
                lighting.skySettings.sunIntensity=0;
            }
            // G15：输出 LUT 布局需要 t2；无 LUT 时绑定 1×1 占位，Shader 通过尺寸跳过采样。
            // G15.09：Lighting 使用 SELFENGINE_LUT_FILE 烘出的 strip；BasicLit 只在编辑器有选择时使用 strip。
            if(outputLutScene_)
            {
                const bool useStrip=lutStripReady_&&lutStrip_
                    &&(scene_==SandboxScene::Lighting||!editorLutPath_.empty());
                if(useStrip)lighting.colorLut=*lutStrip_;
                else if(lutDummy_)lighting.colorLut=*lutDummy_;
            }
            if(forwardInstancing)
            {
                // G06.10：Forward 实例化与 HDR 输出。遮挡开启时 Opaque 前声明多批次，
                // 主 PSO 用 LESS_EQUAL 只着色；关闭时仍走同一实例管线写深度。
                bool occluded=false;
                result=commands->ConfigureForward(lighting,*outputPipeline_,*skyPipeline_);
                if(result)result=commands->BeginPass({se::rhi::PassKind::Clear,se::rhi::ResourceUse::ColorWrite,se::rhi::ResourceUse::DepthWrite,"Clear"});
                if(result)result=commands->EndPass();
                if(result&&occlusionFrame&&forwardOcclusionPipeline_)
                {
                    if(commands->ConfigureOcclusion(occlusionBatches,occlusionFrustum,{true,1e-6f,occlusionHudEnabled_,occlusionTimingEnabled_}))occluded=true;
                }
                if(result)result=commands->BeginPass({se::rhi::PassKind::Sky,se::rhi::ResourceUse::ColorWrite,se::rhi::ResourceUse::None,"Sky"});
                if(result)result=commands->EndPass();
                if(result)result=commands->BeginPass({se::rhi::PassKind::Opaque,se::rhi::ResourceUse::ColorWrite,se::rhi::ResourceUse::DepthWrite,"Opaque",
                    occluded?se::rhi::ResourceUse::DepthRead:se::rhi::ResourceUse::None,cacheBindings});
                for(std::size_t batch=0;result&&batch<batches.size();++batch)
                {
                    auto draw=batches[batch].draw;
                    if(occluded)draw.pipeline=*forwardOcclusionPipeline_;
                    result=commands->DrawInstanced(draw,batches[batch].instances);
                    if(result&&occluded)drawStatistics_.occludedInstances+=batches[batch].instances.size();
                }
                if(result)result=commands->EndPass();
                if(result)result=commands->BeginPass({se::rhi::PassKind::Output,se::rhi::ResourceUse::ColorWrite,se::rhi::ResourceUse::None,"Output",se::rhi::ResourceUse::ColorRead});
                if(result)result=commands->EndPass();
                if(result)result=commands->Close();
                occlusionCommitted_=occluded;
            }
            else if(scene_==SandboxScene::Shadows||BasicLitScene(scene_)||EnvironmentScene(scene_))
            {
                se::rhi::ShadowSettings shadows;
                auto environment=defaultEnvironment;
                std::optional<se::rhi::EnvironmentBindings> environmentBindings;
                if(EnvironmentScene(scene_)||featureEnvironment_)
                {
                    if(frame.data&&frame.data->assets&&!frame.data->environment.asset.empty())environment=environments_.Load(*frame.data->assets,frame.data->environment.asset,*device_,diagnostics);
                    if(!environment)environment=environments_.Fallback(*device_,diagnostics);
                    if(!environment)result={se::rhi::Error::BackendFailure};
                    else environmentBindings=se::PrepareEnvironmentBindings(*environment,camera_.value_or(se::CameraSnapshot{}),frame.data?frame.data->environment.intensity:0,frame.data?frame.data->environment.yaw:0,diagnostics);
                }
                auto view=frame.data?frame.data->shadow:se::DirectionalShadowView{};
                view.filter.pcss=shadowFilterFrame;
                if(!(cascadedFrame?se::PrepareDirectionalCascadeShadow(view,lighting,*camera_,shadows,diagnostics):
                    se::PrepareDirectionalShadow(view,lighting,shadows,diagnostics)))result={se::rhi::Error::InvalidArgument};
                else if(result)
                {
                    std::optional<se::rhi::PostProcessSettings> settings;std::vector<se::rhi::OverlayQuad> overlays;
                    const auto pipelines=post_.Bindings();
                    if(postFrame){settings=se::PreparePostProcessSettings(frame.data&&frame.data->post?*frame.data->post:se::rhi::PostProcessSettings{},diagnostics);
                        settings->autoExposure=autoExposureEnabled_;settings->lutEnabled=lutEnabled_||(!editorLutPath_.empty()&&lutFile_.Ready());
                        // 文件 LUT 与 LGG 生成互斥；revision 变化时由 D3D12 后处理上传一次。
                        if(settings->lutEnabled&&lutFile_.Ready())
                        {
                            settings->lutAsset=lutFile_.Values();
                            settings->lutRevision=lutFile_.Status().revision;
                        }
                        settings->exposureDeltaSeconds=static_cast<float>(std::clamp(frame.updateDeltaSeconds,0.0,0.5));
                        // 深度和覆盖用本帧抖动投影；历史颜色位于固定像素网格，速度只包含真实运动。
                        settings->taa=taaEnabled_;
                        if(taaEnabled_)
                        {
                            settings->taaCurrentClipFromWorld=viewProjectionBase;
                            settings->taaPreviousClipFromWorld=previousViewProjection_;
                            settings->taaHistoryValid=previousViewProjectionValid_
                                &&lastSurfaceWidth_==frame.surface.width&&lastSurfaceHeight_==frame.surface.height;
                        }
                        if(frame.data&&frame.data->referenceUiOverlays)overlays=se::PreparePostProcessOverlays(frame.data->ui,diagnostics);}
                    // 反射、漫反射继续采样 IBL，自定义天空和编辑器的天空开关只决定背景。
                    const bool useProceduralSky=featureEnvironment_&&frame.data&&(frame.data->sky.enabled||(frame.data->todLighting&&frame.data->timeOfDay)
                        ||(frame.data->editorGuiEnabled&&!frame.data->editorSkyEnabled));
                    const auto sky=useProceduralSky?*featureEnvironmentSky_:*skyPipeline_;
                    if(frameGraphEnabled_)
                        result=se::RecordShadowedFrameGraph(*commands,lighting,shadows,*shadowPipeline_,cascadedFrame?*cascadedOutput_:*outputPipeline_,sky,meshes,casters,environmentBindings?&*environmentBindings:nullptr,
                            settings?&*settings:nullptr,settings?&pipelines:nullptr,overlays,cacheBindings,frameGraph_,sceneWidth,sceneHeight,transparent,
                            geometrySettings.enabled?&geometrySettings:nullptr,screenSettings?&*screenSettings:nullptr);
                    else
                        result=se::RecordShadowedFrame(*commands,lighting,shadows,*shadowPipeline_,cascadedFrame?*cascadedOutput_:*outputPipeline_,sky,meshes,casters,environmentBindings?&*environmentBindings:nullptr,
                            settings?&*settings:nullptr,settings?&pipelines:nullptr,overlays,cacheBindings,transparent,
                            geometrySettings.enabled?&geometrySettings:nullptr,screenSettings?&*screenSettings:nullptr);
                }
                report(diagnostics);
            }
            else if(frameGraphEnabled_)
                result=se::RecordForwardFrameGraph(*commands,lighting,*outputPipeline_,*skyPipeline_,meshes,cacheBindings,
                    frameGraph_,sceneWidth,sceneHeight,transparent,geometrySettings.enabled?&geometrySettings:nullptr,screenSettings?&*screenSettings:nullptr);
            else result=se::RecordForwardFrame(*commands,lighting,*outputPipeline_,*skyPipeline_,meshes,cacheBindings,transparent,
                geometrySettings.enabled?&geometrySettings:nullptr,screenSettings?&*screenSettings:nullptr);
        }
        else
        {
            // G06.08/G06.10：多批次遮挡帧先声明批次，再逐批次 ExecuteIndirect；失败时回退普通实例绘制。
            bool occluded=false;
            if(result&&occlusionFrame&&occlusionPipeline_&&!batches.empty())
            {
                if(commands->ConfigureOcclusion(occlusionBatches,occlusionFrustum,{true,1e-6f,occlusionHudEnabled_,occlusionTimingEnabled_}))
                {
                    result=commands->BeginPass({se::rhi::PassKind::Opaque,se::rhi::ResourceUse::ColorWrite,se::rhi::ResourceUse::DepthWrite,"Opaque",se::rhi::ResourceUse::None,cacheBindings});
                    for(std::size_t batch=0;result&&batch<batches.size();++batch)
                    {
                        auto occlusionDraw=batches[batch].draw;occlusionDraw.pipeline=*occlusionPipeline_;
                        result=commands->DrawInstanced(occlusionDraw,batches[batch].instances);
                        if(result)drawStatistics_.occludedInstances+=batches[batch].instances.size();
                    }
                    if(result)result=commands->EndPass();
                    occluded=true;
                }
            }
            if(result&&!occluded)
            {
                result=commands->BeginPass({se::rhi::PassKind::Opaque,se::rhi::ResourceUse::ColorWrite,se::rhi::ResourceUse::DepthWrite,"Opaque",se::rhi::ResourceUse::None,cacheBindings});
                if(result&&instancing)for(const auto& batch:batches){result=commands->DrawInstanced(batch.draw,batch.instances);if(!result)break;}
                if(result&&!instancing)for(const auto& mesh:meshes){result=commands->Draw(mesh);if(!result)break;}
                if(result)result=commands->EndPass();
            }
            // HUD 只跟随真正提交的遮挡帧；多批次/声明失败回退后下一帧隐藏覆盖层。
            occlusionCommitted_=occluded;
        }
        if(result&&frameGraphEnabled_&&!frameGraphDumped_)
        {
            const auto graphStats=frameGraph_.Stats();
            if(graphStats.executedPasses>0)
            {
                frameGraphDumped_=true;
                bool dumped=true;
                if(!frameGraphDotPath_.empty())dumped=frameGraph_.WriteDot(frameGraphDotPath_)&&dumped;
                if(!frameGraphJsonPath_.empty())dumped=frameGraph_.WriteJson(frameGraphJsonPath_)&&dumped;
                char compileTime[32]{};
                std::snprintf(compileTime,sizeof(compileTime),"%.3f",graphStats.compileMilliseconds);
                Log(LogLevel::Debug,localization.GetText("Rendering.FrameGraph")+": passes="
                    +std::to_string(graphStats.executedPasses)+"/"+std::to_string(graphStats.declaredPasses)
                    +", culled="+std::to_string(graphStats.culledPasses)+", disabled="+std::to_string(graphStats.disabledPasses)
                    +", edges="+std::to_string(graphStats.edges)+", aliases="+std::to_string(graphStats.aliasGroups)
                    +", saved="+std::to_string(graphStats.savedBytes)+" B, compile="+compileTime+" ms");
                if(!dumped)Log(LogLevel::Warning,localization.GetText("Rendering.FrameGraphDumpFailed"));
            }
        }
        models_.Prune(frame.data?frame.data->assets:nullptr,needed,neededMaterials);
        if(models_.Bc1TexturesEnabled()&&reportedBc1Uploads_!=models_.CompressedUploads())
        {
            reportedBc1Uploads_=models_.CompressedUploads();
            Log(LogLevel::Debug,localization.GetText("Rendering.TextureBakeSummary")+": "
                +std::to_string(models_.CompressedUploads())+"/"+std::to_string(models_.BakedTextureCount())
                +", "+std::to_string(models_.BakedTextureBytes())+" B");
        }
    }
    if(result&&!forward)result=commands->Close();
    if(result)result=device_->Resize(frame.surface.width,frame.surface.height);
    const bool indoorActive=frame.data&&camera_&&frame.data->indoor.Enabled();
    if((taaFrame||indoorActive)&&result)
    {
        previousViewProjection_=viewProjectionBase;previousViewProjectionValid_=true;
        lastSurfaceWidth_=frame.surface.width;lastSurfaceHeight_=frame.surface.height;
    }
    if(indoorActive&&result)
    {
        indoorHistoryValid_=true;
        lastSceneWidth_=sceneWidth;lastSceneHeight_=sceneHeight;
        lastIndoorSceneToken_=ComputeIndoorSceneToken(scene_,frame.data);
    }
    else
    {
        indoorHistoryValid_=false;
    }
    preparation.reset();
    se::rhi::CompletionToken token;
    se::rhi::Image capture;const bool requested=!captureOutput_.empty()&&!captured_&&frame.updateIndex==captureFrame_;
    if(result)result=device_->Submit(*commands,token,requested?&capture:nullptr);
    if(result)
    {
        const auto indirect=device_->Stats().gpuDriven;
        const std::string key="Rendering.GpuDrivenHiZUnavailable";
        if(indirect.hzbRequested&&!indirect.hzbValid&&reportedDiagnostics_.insert(key).second)
            Log(LogLevel::Warning,localization.GetText(key));
    }
    if(clusteredFrame&&result)
    {
        const auto cluster=device_->Stats().clustered;
        if(cluster.valid)
        {
            const auto reportCluster=[&](const char* key,const std::string& shape)
            {
                if(reportedDiagnostics_.insert(std::string(key)+":"+shape).second)
                    Log(LogLevel::Warning,localization.GetText(key)+": source="+std::to_string(cluster.sourceFrame)+", "+shape);
            };
            if(cluster.counters[6])reportCluster("Rendering.ClusterGlobalFallback","lights="+std::to_string(cluster.counters[0])
                +", retained="+std::to_string(cluster.counters[7])+", requested="+std::to_string(cluster.counters[4])+", capacity="+std::to_string(cluster.counters[5]));
            if(cluster.counters[2])reportCluster("Rendering.ClusterLocalTruncation","clusters="+std::to_string(cluster.counters[2])
                +", dropped_references="+std::to_string(cluster.counters[3]));
        }
    }
    const float cpuMilliseconds=std::chrono::duration<float,std::milli>(std::chrono::steady_clock::now()-telemetryStart).count();
    telemetry_.EndFrame(Statistics(),static_cast<std::uint32_t>(drawStatistics_.draws),static_cast<std::uint32_t>(drawStatistics_.instances),static_cast<std::uint32_t>(frameTriangles_),cpuMilliseconds);
    auto completedTimings=device_->GpuTimingsSince(timingCursor_);
    for(const auto& timing:completedTimings)timingCursor_=std::max(timingCursor_,timing.submission);
    telemetry_.ObserveTimings(completedTimings);
    if(frame.updateIndex<=1)
    {
        const auto gpu=Statistics();
        Log(LogLevel::Debug,localization.GetText("Rendering.PsoLibraryHits")+": "+std::to_string(gpu.pipelineLibraryHits)+"/"+std::to_string(gpu.pipelineLibraryStores));
    }
    if(!profileOutput_.empty())
        if(const auto* stats=telemetry_.Latest();stats&&stats->frameIndex%60==0)
            Log(LogLevel::Info,localization.GetText("Rendering.TelemetrySummary")
                +": frame="+std::to_string(stats->frameIndex)
                +", cpu="+std::to_string(stats->cpuPrepareMilliseconds)
                +" ms, gpu="+std::to_string(stats->GpuTotalMilliseconds())
                +" ms, draws="+std::to_string(stats->drawCalls)
                +", tris="+std::to_string(stats->triangles)
                +", vram="+std::to_string(stats->vramUsedMiB)+" MiB");
    if(result&&requested)
    {
        std::vector<se::AssetDiagnostic> diagnostics;
        if(!se::WritePpmImage(captureOutput_,capture.width,capture.height,capture.rgba8,diagnostics))
        {for(const auto& diagnostic:diagnostics)Log(LogLevel::Error,localization.GetText(diagnostic.key)+": "+diagnostic.detail);result={se::rhi::Error::BackendFailure};}
        else captured_=true;
    }
    if(!result)
    {
        Log(LogLevel::Error,localization.GetText(result.Key()));camera_.reset();return RenderResult::Failed;
    }
    return device_->Occluded()?RenderResult::Occluded:RenderResult::Success;
}

EditorRenderView SandboxRenderer::QueryEditorView() const
{
    if(editorTarget_&&editorTarget_->Ready())
        return {camera_.has_value(),camera_?camera_->viewProjection.values:std::array<float,16>{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1},se::GuiCompositor::SceneToken,{}};
    if (!camera_) return {false,{},se::GuiRenderer::SceneToken,{}};
    return { true, camera_->viewProjection.values, se::GuiRenderer::SceneToken, {} };
}

bool SandboxRenderer::UpdateLutStrip(const Localization& localization)
{
    if(!device_||!lutFile_.Ready())return false;
    // G15.09：默认 Rgba16F strip，保留 >1 的显示线性值并消除暗部 8 位台阶；
    // SELFENGINE_LUT_8BIT=1 只用于与 G15.08 的 8 位路径做 A/B 对照。
    wchar_t legacyValue[8]{};
    const auto legacyLength=GetEnvironmentVariableW(L"SELFENGINE_LUT_8BIT",legacyValue,8);
    const bool highPrecision=!(legacyLength>0&&!(legacyLength==1&&legacyValue[0]==L'0'));
    const auto format=highPrecision?se::ColorLutStripFormat::Rgba16Float:se::ColorLutStripFormat::Rgba8;
    se::ColorLutStrip strip;
    if(!se::BakeColorLutStrip(lutFile_.Lut(),format,strip)||!strip.Valid())
    {
        Log(LogLevel::Error,localization.GetText("Rendering.LutAssetInvalid"));
        return false;
    }
    se::rhi::TextureDescription description{strip.width,strip.height};
    description.format=highPrecision?se::rhi::TextureFormat::Rgba16Float:se::rhi::TextureFormat::Rgba8;
    description.name="ColorLutStrip";
    const se::rhi::TextureSubresource subresource{strip.bytes,0};
    se::rhi::Texture next;
    if(!device_->CreateTextureSubresources(description,std::span(&subresource,1),next))
    {
        Log(LogLevel::Error,localization.GetText("Rendering.LutAssetInvalid"));
        return false;
    }
    lutStrip_=std::move(next);
    lutStripReady_=true;lutStrip16F_=highPrecision;
    lutStripRevision_=lutFile_.Status().revision;
    Log(LogLevel::Info,localization.GetText("Rendering.LutLoaded")+": strip "
        +std::to_string(lutFile_.Status().size)+"^3 "+(highPrecision?"Rgba16F":"Rgba8"));
    return true;
}
bool SandboxRenderer::PrepareShadowFilterPipelines(bool clustered)
{
    auto& stored=shadowFilterPipelines_[clustered?1:0];if(stored.pbr&&stored.skinned)return true;
    ClusterPipelines next;const auto shaders=ResourceDirectory()/"Shaders";
    const auto create=[&](se::rhi::PipelineKind kind,const char* file,se::rhi::Pipeline& target,bool skin=false)
    {
        se::rhi::PipelineDescription description{kind,shaders/file,se::rhi::ShadowFilterBindingLayout(kind,skin)};
        description.colorFormat=se::rhi::ColorFormat::Rgba16Float;
        if(skin){description.vertexLayoutVersion=se::rhi::VertexLayoutVersionSkinned;description.macros.push_back({"SE_SKINNED","1"});}
        return bool(device_->CreatePipeline(description,target));
    };
    if(clustered)
    {
        if(!create(se::rhi::PipelineKind::ClusteredCascadedShadowedMesh,"PcssClusteredCascadedShadowedForward.hlsl",next.pbr)
            ||!create(se::rhi::PipelineKind::ClusteredCascadedBasicLitMesh,"PcssClusteredCascadedBasicLit.hlsl",next.basic)
            ||!create(se::rhi::PipelineKind::ClusteredCascadedEnvironmentMesh,"PcssClusteredCascadedEnvironmentForward.hlsl",next.environment)
            ||!create(se::rhi::PipelineKind::ClusteredCascadedShadowedMesh,"PcssClusteredCascadedSkinnedForward.hlsl",next.skinned,true)
            ||!create(se::rhi::PipelineKind::ClusteredCascadedEnvironmentMesh,"PcssClusteredCascadedSkinnedEnvironmentForward.hlsl",next.skinnedEnvironment,true))return false;
    }
    else
    {
        if(!create(se::rhi::PipelineKind::CascadedShadowedMesh,"PcssCascadedShadowedForward.hlsl",next.pbr)
            ||!create(se::rhi::PipelineKind::CascadedBasicLitMesh,"PcssCascadedBasicLit.hlsl",next.basic)
            ||!create(se::rhi::PipelineKind::CascadedEnvironmentMesh,"PcssCascadedEnvironmentForward.hlsl",next.environment)
            ||!create(se::rhi::PipelineKind::CascadedShadowedMesh,"PcssCascadedSkinnedForward.hlsl",next.skinned,true)
            ||!create(se::rhi::PipelineKind::CascadedEnvironmentMesh,"PcssCascadedSkinnedEnvironmentForward.hlsl",next.skinnedEnvironment,true))return false;
    }
    stored=std::move(next);return true;
}

bool SandboxRenderer::PrepareClusteredPipelines(std::size_t group)
{
    if(group>=clusteredPipelines_.size())return false;
    auto& stored=clusteredPipelines_[group];if(stored.pbr&&stored.skinned)return true;
    ClusterPipelines next;const auto shaders=ResourceDirectory()/"Shaders";
    const auto create=[&](se::rhi::PipelineKind kind,const char* file,se::rhi::Pipeline& target,bool skin=false)
    {
        se::rhi::PipelineDescription description{kind,shaders/file,
            skin?se::rhi::SkinnedClusteredLayoutVersion:se::rhi::ClusteredForwardLayoutVersion};
        description.colorFormat=se::rhi::ColorFormat::Rgba16Float;
        if(skin){description.vertexLayoutVersion=se::rhi::VertexLayoutVersionSkinned;description.macros.push_back({"SE_SKINNED","1"});}
        return bool(device_->CreatePipeline(description,target));
    };
    if(group==0)
    {
        if(!create(se::rhi::PipelineKind::ClusteredForwardMesh,"ClusteredForward.hlsl",next.pbr)
            ||!create(se::rhi::PipelineKind::ClusteredForwardMesh,"ClusteredSkinnedForward.hlsl",next.skinned,true))return false;
    }
    else if(group==1)
    {
        if(!create(se::rhi::PipelineKind::ClusteredShadowedMesh,"ClusteredShadowedForward.hlsl",next.pbr)
            ||!create(se::rhi::PipelineKind::ClusteredBasicLitMesh,"ClusteredBasicLit.hlsl",next.basic)
            ||!create(se::rhi::PipelineKind::ClusteredEnvironmentMesh,"ClusteredEnvironmentForward.hlsl",next.environment)
            ||!create(se::rhi::PipelineKind::ClusteredShadowedMesh,"ClusteredSkinnedShadowedForward.hlsl",next.skinned,true)
            ||!create(se::rhi::PipelineKind::ClusteredEnvironmentMesh,"ClusteredSkinnedEnvironmentForward.hlsl",next.skinnedEnvironment,true))return false;
    }
    else
    {
        if(!create(se::rhi::PipelineKind::ClusteredCascadedShadowedMesh,"ClusteredCascadedShadowedForward.hlsl",next.pbr)
            ||!create(se::rhi::PipelineKind::ClusteredCascadedBasicLitMesh,"ClusteredCascadedBasicLit.hlsl",next.basic)
            ||!create(se::rhi::PipelineKind::ClusteredCascadedEnvironmentMesh,"ClusteredCascadedEnvironmentForward.hlsl",next.environment)
            ||!create(se::rhi::PipelineKind::ClusteredCascadedShadowedMesh,"ClusteredCascadedSkinnedForward.hlsl",next.skinned,true)
            ||!create(se::rhi::PipelineKind::ClusteredCascadedEnvironmentMesh,"ClusteredCascadedSkinnedEnvironmentForward.hlsl",next.skinnedEnvironment,true))return false;
    }
    stored=std::move(next);return true;
}

bool SandboxRenderer::PrepareCascadedPipelines()
{
    if(cascadedPbr_&&cascadedBasic_&&cascadedEnvironment_&&cascadedSkinned_&&cascadedSkinnedEnvironment_&&cascadedOutput_)return true;
    const auto shaders=ResourceDirectory()/"Shaders";
    se::rhi::Pipeline pbr,basic,environment,skinned,skinnedEnvironment,output;
    const auto create=[&](se::rhi::PipelineKind kind,const char* file,se::rhi::Pipeline& target,bool skin=false)
    {
        se::rhi::PipelineDescription description{kind,shaders/file,
            skin?se::rhi::SkinnedCascadedLayoutVersion:se::rhi::CascadedShadowLayoutVersion};
        description.colorFormat=se::rhi::ColorFormat::Rgba16Float;
        if(skin){description.vertexLayoutVersion=se::rhi::VertexLayoutVersionSkinned;description.macros.push_back({"SE_SKINNED","1"});}
        return bool(device_->CreatePipeline(description,target));
    };
    if(!create(se::rhi::PipelineKind::CascadedShadowedMesh,"CascadedShadowedForward.hlsl",pbr)
        ||!create(se::rhi::PipelineKind::CascadedBasicLitMesh,"CascadedBasicLit.hlsl",basic)
        ||!create(se::rhi::PipelineKind::CascadedEnvironmentMesh,"CascadedEnvironmentForward.hlsl",environment)
        ||!create(se::rhi::PipelineKind::CascadedShadowedMesh,"CascadedSkinnedForward.hlsl",skinned,true)
        ||!create(se::rhi::PipelineKind::CascadedEnvironmentMesh,"CascadedSkinnedEnvironmentForward.hlsl",skinnedEnvironment,true))return false;
    se::rhi::PipelineDescription final{se::rhi::PipelineKind::CascadedShadowOutput,shaders/"CascadedShadowOutput.hlsl",
        outputLutScene_?se::rhi::OutputLutLayoutVersion:se::rhi::CascadedShadowLayoutVersion};
    if(outputLutScene_)final.macros.push_back({"SE_OUTPUT_LUT","1"});
    if(!device_->CreatePipeline(final,output))return false;
    cascadedPbr_=std::move(pbr);cascadedBasic_=std::move(basic);cascadedEnvironment_=std::move(environment);
    cascadedSkinned_=std::move(skinned);cascadedSkinnedEnvironment_=std::move(skinnedEnvironment);cascadedOutput_=std::move(output);return true;
}

void SandboxRenderer::Shutdown() noexcept
{
    hlodStreamer_.Cancel(); // 先停止后台读取与结果发布，再排空和释放 GPU。
    if(device_&&!profileOutput_.empty())
        try{std::string telemetryError;
            if(!device_->WaitIdle())profileExportFailed_=true;
            const auto tail=device_->GpuTimingsSince(timingCursor_);
            for(const auto& timing:tail)timingCursor_=std::max(timingCursor_,timing.submission);
            telemetry_.ObserveTimings(tail);
            if(!telemetry_.ExportCsv(profileOutput_/"telemetry.csv",telemetryError))profileExportFailed_=true;
            if(!device_->ExportGpuTimingsCsv(profileOutput_/"gpu.csv")
            ||(ForwardScene(scene_)&&!device_->ExportGpuPassTimingsCsv(profileOutput_/"gpu-passes.csv")))profileExportFailed_=true;}
        catch(...){profileExportFailed_=true;}
    if(device_)device_->Shutdown();
    gui_.Clear();
    if(editorGui_)editorGui_->Clear();
    editorGui_.reset();editorTarget_.reset();
    gameUi_.Clear();
    cascadedPbr_.reset();cascadedBasic_.reset();cascadedEnvironment_.reset();cascadedSkinned_.reset();cascadedSkinnedEnvironment_.reset();cascadedOutput_.reset();
    clusteredPipelines_={};
    shadowFilterPipelines_={};
    surfacePipelines_.Clear();
    shadingResources_.Clear();meshletSubmission_=false;
    featureEnvironmentPipeline_.reset();featureEnvironmentSky_.reset();featureForwardOutput_.reset();featurePbrPipeline_.reset();featureEnvironment_=featureForward_=featureFullPbr_=false;
    cube_.reset();sphere_.reset();pipeline_.reset();instancedPipeline_.reset();forwardInstancedPipeline_.reset();compressedPipeline_.reset();instances_.Clear();outputPipeline_.reset();skyPipeline_.reset();shadowPipeline_.reset();skinnedPipeline_.reset();skinnedShadowPipeline_.reset();skinPaletteStorage_.clear();models_.Clear();models_.SetLodLevels(0);environments_.Clear();post_.Clear();bounds_.Clear();reportedDiagnostics_.clear();device_.reset();
    camera_.reset();
    hlodBaked_=false;hlodFailed_=false;hlodInstanceCount_=0;featureHlodSignature_.clear();
    hlodCells_.clear();hlodProxies_.clear();hlodOverrides_.clear();hlodProxyActive_.clear();hlodCandidateCell_.clear();hlodStream_.Clear();
    hlodProxyCache_.Reset();hlodProxyBytes_.clear();
    impostorEnabled_=false;impostorTexture_.reset();impostorPipeline_.reset();impostorQuad_.reset();
    lutFile_.Close();reportedLutFailures_=0;
    lutStrip_.reset();lutDummy_.reset();lutStripReady_=false;lutStripRevision_=0;editorLutPath_.clear();
    outputLutScene_=false;lutStrip16F_=true;
    occlusionEnabled_=false;occlusionPipeline_.reset();forwardOcclusionPipeline_.reset();
    lastCameraError_ = se::CameraError::None;
    lastMultiplePrimary_ = lastRejectedPrimary_ = controlsReported_ = false;
    reportedBc1Uploads_=0;
}
