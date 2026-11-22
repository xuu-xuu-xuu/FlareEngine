#pragma once
#include "ShadingResources.h"

#include "FunctionLayer/Rendering/Renderer.h"
#include "PlatformLayer/Graphics/Rhi.h"
#include "FunctionLayer/Rendering/RenderTelemetry.h"
#include "FunctionLayer/Rendering/MaterialInstance.h"
#include "FunctionLayer/Runtime/Camera.h"
#include "StaticModelRender.h"
#include "MaterialSurfacePipelines.h"
#include "LodSelect.h"
#include "HlodSelect.h"
#include "HlodStream.h"
#include "HlodStreamer.h"
#include "HlodProxyCache.h"
#include "EnvironmentRender.h"
#include "PostProcessRenderer.h"
#include "FrameGraph.h"
#include "Visibility.h"
#include "Instancing.h"
#include "GuiRenderer.h"
#include "GuiCompositor.h"
#include "EditorViewportTarget.h"
#include "GameUIRenderer.h"
#include "ColorLutFile.h"
#include <unordered_set>
#ifdef DeviceCapabilities
#undef DeviceCapabilities
#endif

// 共用图形宿主；样例可显式启用三角形，世界/UI 绘制按各自任务接入。
enum class SandboxScene { Clear, Triangle, TexturedCube, Scene, Materials, Lighting, Shadows, Environment, PostProcess, Benchmark, BasicLit, Hlod };
struct SandboxDrawStatistics {std::uint64_t candidates=0,visible=0,draws=0,culled=0,boundsUpdates=0,instances=0,instanceBatches=0,instanceCapacity=0,debugLines=0,occludedInstances=0;};
class SandboxRenderer final : public Renderer
{
public:
    explicit SandboxRenderer(bool drawTriangle = false) : scene_(drawTriangle?SandboxScene::Triangle:SandboxScene::Clear) {}
    explicit SandboxRenderer(SandboxScene scene,std::filesystem::path resources={}) : scene_(scene),resourceDirectory_(std::move(resources)),models_(scene==SandboxScene::Scene||scene==SandboxScene::Materials||scene==SandboxScene::Lighting||scene==SandboxScene::Shadows||scene==SandboxScene::Environment||scene==SandboxScene::PostProcess||scene==SandboxScene::Benchmark||scene==SandboxScene::BasicLit||scene==SandboxScene::Hlod) {}
    ~SandboxRenderer() override { Shutdown(); }
    bool Initialize(const RenderSurface& surface, const Localization& localization) override;
    RenderResult Render(const RenderFrame& frame, const Localization& localization) override;
    EditorRenderView QueryEditorView() const override;
    void Shutdown() noexcept override;
    void SetProfileOutput(std::filesystem::path directory){profileOutput_=std::move(directory);}
    bool ProfileExportSucceeded() const noexcept {return !profileExportFailed_;}
    void SetCaptureOutput(std::filesystem::path path,std::uint64_t frame){captureOutput_=std::move(path);captureFrame_=frame;captured_=false;}
    bool CaptureSucceeded() const noexcept {return captureOutput_.empty()||captured_;}
    void SetMemoryCapture(bool enabled) noexcept {memoryCaptureRequested_=enabled;}
    const flare::rhi::Image& LatestMemoryCapture() const noexcept {return latestMemoryCapture_;}
    flare::rhi::Statistics Statistics() const {return device_?device_->Stats():flare::rhi::Statistics{};}
    std::vector<std::string> DebugMessages()const{return device_?device_->DebugMessages():std::vector<std::string>{};}
    const flare::RenderTelemetry& Telemetry() const noexcept {return telemetry_;}
    flare::rhi::AdapterInformation Adapter() const {return device_?device_->Adapter():flare::rhi::AdapterInformation{};}
    flare::rhi::DeviceCapabilities Capabilities() const noexcept {return device_?device_->Capabilities():flare::rhi::DeviceCapabilities{};}
    SandboxDrawStatistics DrawStatistics() const {return drawStatistics_;}
    flare::FrameGraphStats FrameGraphStatistics() const noexcept {return frameGraph_.Stats();}
    flare::GameUiRenderStatistics UiStatistics()const{return gameUi_.Statistics();}
    std::vector<flare::rhi::GpuTiming> GpuTimingsSince(std::uint64_t submission){return device_?device_->GpuTimingsSince(submission):std::vector<flare::rhi::GpuTiming>{};}
    flare::rhi::Result WaitIdle(){return device_?device_->WaitIdle():flare::rhi::Result{flare::rhi::Error::InvalidState};}

private:
    std::filesystem::path ResourceDirectory() const;
    // W03：把实体局部调色板压成每关节 3×float4（行主序 3×4）的字节；空调色板时写单位矩阵
    // （绑定姿势），保证蒙皮顶点缓冲也能以“未摆姿势”画出原位置。
    std::span<const std::byte> BuildSkinPalette(const std::vector<flare::Mat4>& palette,std::size_t jointCount,std::uint32_t& outJointCount);
    // G15：把当前 `.selut` 晶格烘成 1089×33 显示线性 strip；失败保留旧 strip。
    // G15.09：默认 Rgba16Float（保留 >1 值与精度），FLARE_LUT_8BIT=1 回退 Rgba8 做 A/B。
    bool UpdateLutStrip(const Localization& localization);
    bool PrepareCascadedPipelines();
    bool PrepareClusteredPipelines(std::size_t group);
    bool PrepareShadowFilterPipelines(bool clustered);
    std::shared_ptr<flare::rhi::Device> device_;
    SandboxScene scene_=SandboxScene::Clear;
    std::filesystem::path resourceDirectory_;
    flare::rhi::Pipeline pipeline_;
    flare::rhi::Pipeline instancedPipeline_;
    // G06.10：Forward 实例化（布局 30）与只读深度变体；仅在 Lighting 场景按需创建。
    flare::rhi::Pipeline forwardInstancedPipeline_;
    flare::rhi::Pipeline forwardOcclusionPipeline_;
    flare::rhi::Pipeline compressedPipeline_;
    flare::PbrInstanceBatcher instances_;
    flare::rhi::Pipeline outputPipeline_;
    flare::rhi::Pipeline skyPipeline_;
    flare::rhi::Pipeline featureEnvironmentPipeline_,featureEnvironmentSky_;
    flare::rhi::Pipeline featurePbrPipeline_;
    flare::rhi::Pipeline cascadedPbr_,cascadedBasic_,cascadedEnvironment_,cascadedSkinned_,cascadedSkinnedEnvironment_,cascadedOutput_;
    struct ClusterPipelines {flare::rhi::Pipeline pbr,basic,environment,skinned,skinnedEnvironment;};
    std::array<ClusterPipelines,3> clusteredPipelines_; // 普通前向、单图阴影、CSM。
    std::array<ClusterPipelines,2> shadowFilterPipelines_; // CSM / cluster+CSM，独立b3。
    flare::MaterialSurfacePipelineCache surfacePipelines_;
    bool featureFullPbr_=false;
    bool featureEnvironment_=false,featurePostAces_=false;
    flare::rhi::Pipeline featureForwardOutput_;
    bool featureForward_=false;
    flare::rhi::Pipeline shadowPipeline_;
    // W03：蒙皮前向（ShadowedMesh）+ 蒙皮阴影深度（ShadowDepth）。只在阴影家族场景创建；
    // 环境/实例化帧不启用，避免与默认 Pipeline 的 pass 类别不一致。
    flare::rhi::Pipeline skinnedPipeline_,skinnedShadowPipeline_;
    // 每帧每演员一份调色板字节；RHI 只借用，存活到 Submit 结束，因此放在成员里而不是栈上。
    std::vector<std::vector<std::byte>> skinPaletteStorage_;
    std::shared_ptr<const flare::StaticRenderModel> cube_;
    std::shared_ptr<const flare::StaticRenderModel> sphere_;
    flare::StaticModelRenderCache models_;
    flare::EnvironmentRenderCache environments_;
    flare::ShadingResourceCache shadingResources_;
    bool meshletSubmission_=false;
    flare::PostProcessRenderer post_;
    flare::GuiRenderer gui_;
    std::unique_ptr<flare::GuiCompositor> editorGui_;
    std::unique_ptr<flare::EditorViewportTargetResource> editorTarget_;
    flare::GameUIRenderer gameUi_;
    flare::WorldBoundsCache bounds_;
    std::unordered_set<std::string> reportedDiagnostics_;
    std::optional<flare::CameraSnapshot> camera_;
    flare::CameraError lastCameraError_ = flare::CameraError::None;
    bool lastMultiplePrimary_ = false, lastRejectedPrimary_ = false, controlsReported_ = false;
    std::filesystem::path profileOutput_;
    bool profileExportFailed_=false;
    flare::RenderTelemetry telemetry_;
    std::uint64_t timingCursor_=0;
    std::uint64_t frameTriangles_=0;
    std::uint64_t reportedBc1Uploads_=0;
    bool lodEnabled_=false;
    // G05.08 A/B 钩子：FLARE_LOD_LEVEL=0..3 固定级别并跳过过渡，仅用于同相机参照图。
    std::uint32_t forcedLodLevel_=UINT32_MAX;
    // G05.08：过渡状态按“模型 + 实体”区分；同一内建模型可被多个实体复用，不能只按模型指针。
    struct LodTransitionKey
    {
        const flare::StaticRenderModel* model=nullptr;
        std::uint64_t entity=0;
        bool operator==(const LodTransitionKey&)const=default;
    };
    struct LodTransitionKeyHash
    {
        std::size_t operator()(const LodTransitionKey& key)const noexcept
        {
            const auto model=std::hash<const void*>{}(key.model),entity=std::hash<std::uint64_t>{}(key.entity);
            return model^(entity+0x9e3779b97f4a7c15ull+(model<<6)+(model>>2));
        }
    };
    std::unordered_map<LodTransitionKey,flare::LodTransition,LodTransitionKeyHash> lodTransitions_;
    // G05.10：HLOD 运行时接入。单元与代理按实例集合缓存，实例数变化时整体重建。
    bool hlodEnabled_=false,hlodBaked_=false,hlodFailed_=false;
    std::uint32_t hlodLevel_=0;
    std::size_t hlodInstanceCount_=0;
    std::string featureHlodSignature_;
    std::vector<flare::HlodCell> hlodCells_;
    std::vector<std::shared_ptr<const flare::StaticRenderModel>> hlodProxies_;
    std::vector<std::vector<std::shared_ptr<const flare::PbrRenderMaterial>>> hlodOverrides_;
    std::vector<bool> hlodProxyActive_;
    std::vector<std::int32_t> hlodCandidateCell_;
    // G05.11：`.sehlod` 加载与按相机半径驻留；非驻留单元回退到实例绘制。
    flare::HlodStream hlodStream_;
    // G05.13：后台读取 `.sehlod` + 代理显存预算/每帧上传上限；不启用时保持 G05.11 的全量同步行为。
    flare::HlodStreamer hlodStreamer_;
    flare::HlodProxyCache hlodProxyCache_;
    bool hlodAsync_=false,hlodLazyProxies_=false;
    std::uint32_t hlodUploadBudget_=0;
    std::uint64_t hlodVramBudget_=0;
    std::vector<std::uint64_t> hlodProxyBytes_;
    std::filesystem::path hlodFilePath_,hlodSavePath_;
    bool hlodFileLoaded_=false,hlodSaved_=false;
    float hlodResidentRadius_=0;
    // G05.12：16 视角 Impostor。远于阈值的实例改画朝向相机的四边形，alpha test 保留轮廓。
    bool impostorEnabled_=false;
    float impostorDistance_=1.0e9f;
    flare::rhi::Texture impostorTexture_;
    flare::rhi::Pipeline impostorPipeline_;
    std::shared_ptr<const flare::StaticRenderModel> impostorQuad_;
    flare::VariantInventory variants_;
    // G13：FLARE_FRAMEGRAPH=1 时改用 Frame Graph 编译出的 Pass 顺序；
    // DOT/JSON 只在首帧导出一次，默认关闭保持 R 系列手写路径。
    flare::FrameGraph frameGraph_;
    bool frameGraphEnabled_=false,frameGraphDumped_=false;
    std::filesystem::path frameGraphDotPath_,frameGraphJsonPath_;
    // G15：FLARE_AUTO_EXPOSURE=1 启用 GPU 直方图曝光；FLARE_LUT=1 启用 33³ LUT。
    // 两者默认关闭，保持既有 Reinhard/CPU 曝光路径逐字节不变。
    bool autoExposureEnabled_=false,lutEnabled_=false;
    std::uint32_t lastSceneWidth_=0,lastSceneHeight_=0;
    std::uint64_t lastIndoorSceneToken_=0,lastRayTracingSceneToken_=0;
    bool indoorHistoryValid_=false,rayTracingHistoryValid_=false;
    // G15：FLARE_LUT_FILE 选中的 `.selut` 每帧按修改时间热重载；失败时保留上一版。
    flare::ColorLutFile lutFile_;
    std::uint64_t reportedLutFailures_=0;
    // G15：输出 LUT。BasicLit（ShadowOutput）取编辑器选择，Lighting（FinalOutput）取 FLARE_LUT_FILE；
    // strip 为 1089×33 显示线性（Rgba16Float，可选 Rgba8）；禁用时绑定 1×1 占位并由 Shader 跳过。
    bool outputLutScene_=false;
    std::filesystem::path editorLutPath_;
    flare::rhi::Texture lutStrip_,lutDummy_;
    std::uint64_t lutStripRevision_=0;
    bool lutStripReady_=false,lutStrip16F_=true;
    // G06.08：FLARE_OCCLUSION=1 时，单一实例批次的场景改用 RHI 遮挡批次
    // （prepass → HZB → 间接剔除 → ExecuteIndirect）；多批次/HLOD/Impostor/LOD 帧回退普通实例绘制。
    bool occlusionEnabled_=false;
    // G06.09：FLARE_OCCLUSION_HUD=1 时请求 48B 参数回读，并在场景 UI 之后画 HUD。
    // 默认关闭；occlusionCommitted_ 记录上一帧是否真的提交了遮挡批次，避免回退帧显示陈旧计数。
    bool occlusionHudEnabled_=false,occlusionCommitted_=false;
    // G06.11：FLARE_OCCLUSION_TIMING=1 时请求每批次 cull 与 prepass/HZB/main 的 GPU 时间。
    bool occlusionTimingEnabled_=false;
    flare::rhi::Pipeline occlusionPipeline_;
    // G14：FLARE_TAA=1 启用抖动、速度缓冲与历史；上一帧矩阵/世界用于重投影。
    bool taaEnabled_=false;
    flare::Mat4 previousViewProjection_{};
    bool previousViewProjectionValid_=false;
    std::uint32_t lastSurfaceWidth_=0,lastSurfaceHeight_=0;
    std::unordered_map<flare::StableEntityId,flare::Mat4> previousWorlds_;
    std::filesystem::path captureOutput_;
    std::uint64_t captureFrame_=0;
    bool captured_=false;
    bool memoryCaptureRequested_=false;
    flare::rhi::Image latestMemoryCapture_;
    SandboxDrawStatistics drawStatistics_;
};
