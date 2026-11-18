#pragma once

#include "CoreLayer/Math/Projection.h"
#include "CoreLayer/MaterialAlpha.h"
#include "MaterialLayout.h"
#include "VisibilityData.h"
#include "CoreLayer/GuiGeometry.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

class Localization;
namespace se {class Profiler;}
namespace se::rhi
{
enum class Error { None, InvalidArgument, InvalidHandle, InvalidState, Unsupported, BackendFailure, CapacityExceeded };
struct Result
{
    Error error=Error::None;
    explicit operator bool() const noexcept {return error==Error::None;}
    const char* Key() const noexcept;
};
template<class Tag> struct Handle
{
    std::uint64_t device=0;
    std::uint32_t index=UINT32_MAX,generation=0;
    explicit operator bool() const noexcept {return device&&index!=UINT32_MAX&&generation;}
    bool operator==(const Handle&) const=default;
};
using BufferHandle=Handle<struct BufferTag>;
using TextureHandle=Handle<struct TextureTag>;
using PipelineHandle=Handle<struct PipelineTag>;
// 最后一个 CPU 所有者退出时逻辑释放；命令和 GPU 使用另由后端保留。
using Buffer=std::shared_ptr<const BufferHandle>;
using Texture=std::shared_ptr<const TextureHandle>;
using Pipeline=std::shared_ptr<const PipelineHandle>;
// 通用场景目标：编辑器视口和后续离屏场景共用，不包含“编辑器”语义。
// hdrColor 必须是 Rgba16Float，ldrColor 为 Rgba8，depth 为 D32；三者尺寸一致。
struct SceneTargets
{
    TextureHandle hdrColor,ldrColor,depth;
    std::uint32_t width=0,height=0;
};
// 一次帧合成的目标集合；output 为空时由呈现后端使用交换链后缓冲。
struct FrameComposition
{
    SceneTargets scene;
    TextureHandle output;
};
struct Vertex
{
    std::array<float,3> position{},normal{};
    std::array<float,2> uv{};
    std::array<float,4> tangent{1,0,0,1};
};
static_assert(sizeof(Vertex)==48);
// W03：蒙皮顶点布局 v3（80 字节）。前 48 字节仍是参考 Vertex，随后是
// JOINTS0（R16G16B16A16_UINT，偏移 48）与 WEIGHTS0（R32G32B32A32_FLOAT，偏移 64）。
// WEIGHTS0 需要 16 字节对齐，因此 56..63 是 8 字节保留间隙；不能靠编译器布局推导。
struct SkinnedVertex
{
    Vertex base;
    std::array<std::uint16_t,4> joints{};
    std::array<std::uint8_t,8> reserved{};
    std::array<float,4> weights{};
};
static_assert(sizeof(SkinnedVertex)==80&&offsetof(SkinnedVertex,joints)==48&&offsetof(SkinnedVertex,weights)==64);

// G03：压缩顶点布局的格式描述。既有 Vertex（48 字节）保留为参考布局，
// 新布局的版本号进入 PSO 键；后端按描述建立 input layout，不内嵌语义。
enum class VertexSemantic : std::uint8_t { Position, Normal, Uv, Tangent };
enum class VertexFormat : std::uint8_t
{
    Float32x3, Float32x4, Float16x2, Float16x4,
    Unorm16x3, Unorm16x2, Unorm8x4, R10G10B10A2,
};
struct VertexElement
{
    VertexSemantic semantic=VertexSemantic::Position;
    VertexFormat format=VertexFormat::Float32x3;
    std::uint16_t offset=0;
    std::uint8_t slot=0; // 0=per-vertex，1=per-instance
};
struct VertexLayoutDesc
{
    std::array<VertexElement,8> elements{};
    std::uint8_t elementCount=0;
    std::array<std::uint32_t,2> strides{};
    std::uint32_t version=0; // 参与 PSO 键与资产版本
};
// W03：V3 = 参考 Vertex + JOINTS0/WEIGHTS0，共 80 字节；见 SkinnedVertex。
inline constexpr std::uint32_t VertexLayoutVersionLegacy=1,VertexLayoutVersionCompressed=2,VertexLayoutVersionSkinned=3;
enum class BufferUsage { Vertex, Index };
struct BufferDescription {BufferUsage usage=BufferUsage::Vertex;std::uint32_t stride=sizeof(Vertex);std::string name;};
enum class TextureFormat { Rgba8, Rgba16Float, Bc1Unorm, Bc1UnormSrgb, Bc5Unorm, Bc4Unorm, Bc7Unorm, Bc7UnormSrgb, Rg16Float, Rgba32Float };
struct TextureDescription
{
    std::uint32_t width=0,height=0,mipLevels=1,samples=1;
    TextureFormat format=TextureFormat::Rgba8;
    std::string name;
    std::uint32_t arrayLayers=1;
    bool cube=false;
};
struct TextureSubresource
{
    std::span<const std::uint8_t> bytes;
    std::uint32_t rowPitch=0; // 0 为紧密行；顺序 mip + arraySlice*mipLevels。
};
struct UploadFootprint
{
    std::uint64_t payloadBytes=0,stagingBytes=0,rowPitch=0,alignment=0;
};
enum class PipelineKind { TexturedMesh, Triangle, PbrMesh, ForwardMesh, FinalOutput, Sky, ShadowedMesh, ShadowDepth, ShadowOutput, EnvironmentMesh, EnvironmentSky, BloomExtract, BloomBlur, ToneMap, Fxaa, UiOverlay, PbrInstancedMesh, Gui, BasicLitMesh, PbrForwardInstancedMesh, CascadedShadowedMesh, CascadedEnvironmentMesh, CascadedBasicLitMesh, CascadedShadowOutput, ClusteredForwardMesh, ClusteredShadowedMesh, ClusteredEnvironmentMesh, ClusteredBasicLitMesh, ClusteredCascadedShadowedMesh, ClusteredCascadedEnvironmentMesh, ClusteredCascadedBasicLitMesh };
constexpr bool IsClustered(PipelineKind kind){return kind>=PipelineKind::ClusteredForwardMesh&&kind<=PipelineKind::ClusteredCascadedBasicLitMesh;}
constexpr bool IsCascaded(PipelineKind kind){return kind==PipelineKind::ClusteredCascadedShadowedMesh||kind==PipelineKind::ClusteredCascadedEnvironmentMesh||kind==PipelineKind::ClusteredCascadedBasicLitMesh||kind==PipelineKind::CascadedShadowedMesh||kind==PipelineKind::CascadedEnvironmentMesh||kind==PipelineKind::CascadedBasicLitMesh||kind==PipelineKind::CascadedShadowOutput;}
constexpr bool IsEnvironment(PipelineKind kind){return kind==PipelineKind::EnvironmentMesh||kind==PipelineKind::CascadedEnvironmentMesh||kind==PipelineKind::ClusteredEnvironmentMesh||kind==PipelineKind::ClusteredCascadedEnvironmentMesh;}
constexpr bool IsInstanced(PipelineKind kind){return kind==PipelineKind::PbrInstancedMesh||kind==PipelineKind::PbrForwardInstancedMesh;}
constexpr bool IsBasicLit(PipelineKind kind){return kind==PipelineKind::BasicLitMesh||kind==PipelineKind::CascadedBasicLitMesh||kind==PipelineKind::ClusteredBasicLitMesh||kind==PipelineKind::ClusteredCascadedBasicLitMesh;}
constexpr bool IsPostProcess(PipelineKind kind){return kind>=PipelineKind::BloomExtract&&kind<=PipelineKind::UiOverlay;}
constexpr std::uint32_t BindingLayout(PipelineKind kind)
{return IsClustered(kind)?ClusteredForwardLayoutVersion:IsCascaded(kind)?CascadedShadowLayoutVersion:kind==PipelineKind::ToneMap?ToneMapLayoutVersion:kind==PipelineKind::Gui?GuiLayoutVersion:kind==PipelineKind::PbrForwardInstancedMesh?ForwardInstancingLayoutVersion:IsInstanced(kind)?InstancingLayoutVersion:IsPostProcess(kind)?PostProcessLayoutVersion:kind==PipelineKind::EnvironmentMesh||kind==PipelineKind::EnvironmentSky?EnvironmentLayoutVersion:kind==PipelineKind::ShadowedMesh||kind==PipelineKind::ShadowDepth||kind==PipelineKind::ShadowOutput||kind==PipelineKind::BasicLitMesh?ShadowLayoutVersion:kind==PipelineKind::Sky?SkyLayoutVersion:kind==PipelineKind::ForwardMesh||kind==PipelineKind::FinalOutput?ForwardLayoutVersion:kind==PipelineKind::PbrMesh?PbrMaterialLayoutVersion:MaterialLayoutVersion;}
constexpr bool IsShadowed(PipelineKind kind){return (IsClustered(kind)&&kind!=PipelineKind::ClusteredForwardMesh)||kind==PipelineKind::ShadowedMesh||kind==PipelineKind::EnvironmentMesh||kind==PipelineKind::BasicLitMesh||(IsCascaded(kind)&&kind!=PipelineKind::CascadedShadowOutput);}
constexpr bool IsSky(PipelineKind kind){return kind==PipelineKind::Sky||kind==PipelineKind::EnvironmentSky;}
constexpr bool IsForward(PipelineKind kind){return kind==PipelineKind::ForwardMesh||kind==PipelineKind::ClusteredForwardMesh||kind==PipelineKind::PbrForwardInstancedMesh||IsShadowed(kind);}
constexpr bool IsOutput(PipelineKind kind){return kind==PipelineKind::FinalOutput||kind==PipelineKind::ShadowOutput||kind==PipelineKind::CascadedShadowOutput;}
constexpr bool IsPbr(PipelineKind kind){return kind==PipelineKind::PbrMesh||IsInstanced(kind)||(IsForward(kind)&&!IsBasicLit(kind));}
// 所有使用 PbrDrawConstants + 3 贴图 + 阴影绑定布局的“受光网格”技术。
constexpr bool UsesLitLayout(PipelineKind kind){return IsPbr(kind)||IsBasicLit(kind);}
// W03：可使用蒙皮顶点布局与调色板绑定（SkinnedLayoutVersion）的技术。
// 目前只覆盖前向家族与阴影深度；实例化/后处理/天空不支持蒙皮。
constexpr bool SupportsSkinning(PipelineKind kind)
{return kind==PipelineKind::ShadowedMesh||kind==PipelineKind::ForwardMesh||kind==PipelineKind::PbrMesh
    ||kind==PipelineKind::EnvironmentMesh||kind==PipelineKind::BasicLitMesh||kind==PipelineKind::ShadowDepth
    ||kind==PipelineKind::CascadedShadowedMesh||kind==PipelineKind::CascadedEnvironmentMesh||(IsClustered(kind)&&!IsBasicLit(kind));}
constexpr bool IsShadowFilterLayout(std::uint32_t layout)
{return layout>=CascadedShadowFilterLayoutVersion&&layout<=SkinnedClusteredShadowFilterLayoutVersion;}
constexpr std::uint32_t ShadowFilterBindingLayout(PipelineKind kind,bool skinned=false)
{return IsClustered(kind)?(skinned?SkinnedClusteredShadowFilterLayoutVersion:ClusteredShadowFilterLayoutVersion)
    :(skinned?SkinnedCascadedShadowFilterLayoutVersion:CascadedShadowFilterLayoutVersion);}
constexpr bool ValidShadowFilterBindingLayout(PipelineKind kind,std::uint32_t layout,bool skinned)
{return IsCascaded(kind)&&IsShadowed(kind)&&(!skinned||SupportsSkinning(kind))&&layout==ShadowFilterBindingLayout(kind,skinned);}
constexpr bool ValidPipelineKind(PipelineKind kind){return static_cast<unsigned>(kind)<=static_cast<unsigned>(PipelineKind::ClusteredCascadedBasicLitMesh);}
enum class DepthMode { Default, Disabled, ReadWriteLess, ReadOnlyLessEqual, ReadOnlyEqual };
enum class BlendMode { Opaque, Alpha };
enum class ColorFormat { Rgba8Unorm, Rgba16Float, Rgba8Srgb };
struct ShaderMacro {std::string name,value;bool operator==(const ShaderMacro&) const=default;};
enum class ShaderInput { Source, Artifact };
struct PipelineDescription
{
    PipelineKind kind=PipelineKind::TexturedMesh;
    std::filesystem::path shader;
    std::uint32_t bindingLayoutVersion=MaterialLayoutVersion;
    std::uint32_t vertexLayoutVersion=VertexLayoutVersionLegacy;
    DepthMode depth=DepthMode::Default;
    BlendMode blend=BlendMode::Opaque;
    ColorFormat colorFormat=ColorFormat::Rgba8Unorm;
    std::vector<ShaderMacro> macros;
    bool cachePrepared=true; // false 绕过 prepared PSO 缓存，Shader program 缓存与有效状态不变。
    ShaderInput shaderInput=ShaderInput::Source; // Artifact仅接受固定用户PBR包；失败不回退编译源文件。
    std::uint32_t materialLayoutVersion=0; // 0 保留旧材质；38 在既有绑定布局上追加独立 b4。
    MaterialAlphaMode alphaMode=MaterialAlphaMode::Opaque; // PSO pass/depth/blend 合同，surface 程序共享动态覆盖能力。
    std::uint32_t shadingLayoutVersion=0; // 0 原布局；40 尾追加 b5/b6/t13..24。
};
inline bool ValidMaterialSurfacePipeline(const PipelineDescription& pipeline)
{
    if(!IsValidMaterialAlphaMode(pipeline.alphaMode))return false;
    if(!pipeline.materialLayoutVersion)return pipeline.alphaMode==MaterialAlphaMode::Opaque;
    if(pipeline.materialLayoutVersion!=MaterialSurfaceLayoutVersion||pipeline.shaderInput!=ShaderInput::Source
        ||(!UsesLitLayout(pipeline.kind)&&pipeline.kind!=PipelineKind::ShadowDepth)||IsInstanced(pipeline.kind))return false;
    if(pipeline.kind==PipelineKind::ShadowDepth)
        return pipeline.alphaMode==MaterialAlphaMode::Mask&&pipeline.blend==BlendMode::Opaque
            &&(pipeline.depth==DepthMode::Default||pipeline.depth==DepthMode::ReadWriteLess);
    if(pipeline.alphaMode==MaterialAlphaMode::Blend)
        return IsForward(pipeline.kind)&&pipeline.colorFormat==ColorFormat::Rgba16Float
            &&pipeline.blend==BlendMode::Alpha&&pipeline.depth==DepthMode::ReadOnlyLessEqual;
    return pipeline.blend==BlendMode::Opaque&&(pipeline.alphaMode!=MaterialAlphaMode::Mask
        ||pipeline.depth==DepthMode::Default||pipeline.depth==DepthMode::ReadWriteLess);
}
inline bool ValidShadingExtensionPipeline(const PipelineDescription& pipeline)
{
    return !pipeline.shadingLayoutVersion||(pipeline.shadingLayoutVersion==ShadingExtensionLayoutVersion
        &&pipeline.shaderInput==ShaderInput::Source&&IsForward(pipeline.kind)&&UsesLitLayout(pipeline.kind)&&!IsInstanced(pipeline.kind));
}
struct Surface {void* window=nullptr;std::uint32_t width=0,height=0;};
struct AdapterInformation {std::string name;std::uint32_t vendorId=0,deviceId=0;std::uint64_t dedicatedVideoMemory=0;bool software=false;};
struct CompletionToken {std::uint64_t device=0,queue=0,value=0;bool operator==(const CompletionToken&) const=default;};
enum class UploadMode { Pooled, DedicatedBaseline };
enum class MemoryMode { Placed, CommittedBaseline };
// budgetBytes=0 时按适配器专用显存自动确定；4× 空间采样在 1080p 以上会超过固定小预算。
struct MemorySettings {MemoryMode mode=MemoryMode::Placed;std::uint64_t heapPageBytes=4*1024*1024,budgetBytes=0;};
struct DescriptorLimits {std::uint32_t persistentPage=64,persistentMaximum=65536,frameInitial=64,frameMaximum=65536;};
enum class PassKind { Clear, Opaque, Triangle, Sky, Output, Shadow, BloomExtract, BloomHorizontal, BloomVertical, ToneMap, Fxaa, Ui, Transparent, DepthNormals, AmbientOcclusion };
enum class ResourceUse { ColorWrite, DepthWrite, VertexRead, IndexRead, TextureRead, ColorRead, None, DepthRead };
struct PassDescription
{
    PassKind kind=PassKind::Clear;
    ResourceUse color=ResourceUse::ColorWrite,depth=ResourceUse::DepthWrite;
    std::string name;
    ResourceUse input=ResourceUse::None;
    bool cacheBindings=false; // 只作用于本次 Opaque Pass，不跨命令列表缓存。
};
struct ShadingFrameBindings
{
    bool enabled=false;
    ShadingFrameConstants constants;
    // t14 directEnergy / t15 probeData / t16 LTCmatrix / t17 LTCamp / t18 skinLUT。
    std::array<TextureHandle,5> textures{};
};
struct ShadingMaterialBindings
{
    ShadingMaterialConstants constants;
    // t19 coatFactor / t20 coatRough / t21 coatNormal / t22 sheenColor / t23 sheenRough / t24 anisotropy。
    std::array<TextureHandle,6> textures{};
};
struct GeometryCluster
{
    std::uint32_t firstIndex=0,indexCount=0;
    Aabb localBounds; // 实际上传/重排 IB 的范围，界位于 draw.world 之前的实体局部空间。
};
struct DrawIndexed
{
    BufferHandle vertices,indices;
    TextureHandle texture;
    PipelineHandle pipeline;
    Mat4 world,viewProjection;
    Vec4 tint{1,1,1,1};
    bool textureIsData=false,unlit=false;
    TextureHandle normalTexture,metallicRoughnessTexture;
    struct Pbr
    {
        Vec3 cameraPosition{},directionToLight{0.3f,0.5f,0.8f},radiance{1,1,1};
        float metallic=0,roughness=1,normalScale=1;
        bool normalMapping=false,doubleSided=false;
        bool receivesShadow=true;
        // G05.08：LOD dither 编码。0 关闭；(0,1) 为新级像素占比，(−1,0) 为旧级对应绘制。
        // 只由不带阴影绑定的 PBR 技术（当前为 Materials 场景）使用，写入 options.w。
        float lodDither=0;
        // RG104：独立开关；默认保持旧材质，IBL 多散射与法线高光抗锯齿不改变资产。
        bool multiscatter=false,specularAA=false;
    } pbr;
    bool castsShadow=true;
    // G05.12：仅 TexturedMesh 管线的 Impostor 编码（相邻视角序号与插值权重）；普通绘制保持 0。
    float impostorView=0,impostorNext=0,impostorBlend=0;
    // 仅用于统计与诊断；0 表示未提供，绘制仍以缓冲自身大小为准。
    std::uint32_t indexCount=0;
    // G14/RG104：上一帧世界矩阵。缺省时表示首次出现或无可靠运动历史，速度 Pass 保持 invalidMotion 以重置历史；
    // compressedVertices 由 D3D12 后端按 PSO 填充，调用方无需设置。
    bool hasPreviousWorld=false;
    Mat4 previousWorld;
    bool compressedVertices=false;
    bool outline=false; // 场景可见像素轮廓；不改变材质、阴影或物体深度。
    // W06：外观覆盖参数。x=模式（0 基础色/1 白模/2 Toon），y=混合，z=Toon 分档，w=边缘强度。
    Vec4 look{};
    // W03：蒙皮调色板。每关节 3×float4（行主序 3×4 的实体局部蒙皮矩阵），共 48 字节；
    // 空表示静态绘制。数据由调用方保活到本次 Submit 完成，后端每帧复制进上传环，
    // 因此不需要常驻缓冲或 GPU 句柄。jointOffset 是调色板在本绘制绑定范围内的字节偏移。
    std::span<const std::byte> skinPalette{};
    std::uint32_t skinJointOffset=0,skinJointCount=0;
    MaterialAlphaMode alphaMode=MaterialAlphaMode::Opaque;
    float alphaCutoff=.5f; // 有限且 >=0；>1 合法并自然全裁，不钳到1。
    std::uint64_t stableId=0; // 仅 CPU 透明排序/诊断，绝不转成 float 上传。
    Vec3 sortCenter{}; // 可选世界包围中心；缺省按 world 原点。
    bool hasSortCenter=false;
    ShadingMaterialBindings layers;
    Aabb worldBounds;
    bool hasWorldBounds=false;
    std::span<const GeometryCluster> geometryClusters{}; // Draw 接收时复制快照；非法/缺失覆盖保留 CPU whole-draw。
};
// 世界空间辅助线。width 为逻辑像素；隐藏部分 alpha=0 表示遵守正常遮挡。
struct SceneLine
{
    Vec3 a,b;
    Vec4 color{1,1,1,1};
    float width=1,occludedAlpha=0;
};
struct ParticleBatch
{
    bool additive=false;
    std::vector<ParticleInstanceGpuData> instances;
};
struct ParticleFrame
{
    Vec3 cameraRight{1,0,0};
    Vec3 cameraUp{0,1,0};
    std::vector<ParticleBatch> batches;
};
struct InstanceParameters
{
    Mat4 world;
    Vec4 tint{1,1,1,1};
    float metallic=0,roughness=1,normalScale=1;
    bool normalMapping=false,unlit=false,doubleSided=false,receivesShadow=true;
};
enum class ShadowFilterDebugMode : std::uint32_t { None=0,Visibility=1,Metrics=2 };
struct ShadowFilterSettings
{
    bool pcss=false;
    float angularRadius=.00465f; // 方向面积光角半径，弧度。
    float blockerSearchDistance=20.f; // 搜索 footprint 对应的世界光轴距离，米。
    float maximumRadiusTexels=16.f; // 搜索和滤波椭圆各轴的有限 texel 半径。
    ShadowFilterDebugMode debug=ShadowFilterDebugMode::None;
};
struct ShadowSettings
{
    Mat4 clipFromWorld;
    std::uint32_t resolution=1024;
    float depthBias=0.0008f,slopeBias=0.002f,normalBias=0.015f;
    bool enabled=true,pcf=true,preview=false;
    // G08：0 保持旧单图；1..3 为真实深度数组，矩阵已经包含混合带与 caster 深度。
    std::uint32_t cascadeCount=0;
    std::array<Mat4,3> cascadeClipFromWorld{};
    std::array<float,3> cascadeSplits{};
    Vec4 cascadeViewDepth{}; // dot(world.xyz1, cascadeViewDepth) 得正视深。
    float cascadeNear=0.1f,cascadeBlend=0.10f;
    bool cascadeDebug=false;
    ShadowFilterSettings filter; // 独立新布局消费；默认沿用旧 point / 5x5 PCF。
};
struct EnvironmentBindings
{
    TextureHandle irradiance,specular,brdfLut;
    Mat4 skyFromClip;
    float intensity=1,yaw=0; // yaw 弧度，资产绕世界 +Y 旋转。
};
struct PostProcessSettings
{
    bool bloom=true,toneMapping=true,fxaa=true;
    float bloomThreshold=1,bloomStrength=0.08f;
    // G15：GPU 自动曝光与 33³ LUT。默认关闭，保持既有 Reinhard/FXAA 路径逐字节不变。
    // 曝光为 1×1 R32_FLOAT ping-pong；LUT 只在参数变化时重建，关闭时 ToneMap 不采样。
    bool autoExposure=false,lutEnabled=false;
    float exposureDeltaSeconds=1.0f/60.0f;
    float exposureLowPercentile=0.60f,exposureHighPercentile=0.80f;
    float exposureBrightAdaptation=3.0f,exposureDarkAdaptation=1.0f;
    float exposureMinimum=1.0f/1024.0f,exposureMaximum=64.0f,exposureKey=0.18f;
    float lutLift[3]{0,0,0},lutGamma[3]{1,1,1},lutGain[3]{1,1,1};
    // G15：外部 `.selut` LUT。非空且 size 为 33³ 时替代 lift/gamma/gain 生成；
    // revision 标识数据版本，后端只在变化时上传一次，不逐帧复制晶格。
    std::shared_ptr<const std::vector<Vec3>> lutAsset;
    std::uint64_t lutRevision=0;
    // G14：TAA。关闭时 ToneMap/Bloom 直接读 HDR，FXAA 保持原回退路径。
    // 两个裁剪矩阵由调用方提供（均不含采样抖动）；historyValid=false 表示首帧/resize/cut。
    bool taa=false;
    Mat4 taaCurrentClipFromWorld{},taaPreviousClipFromWorld{};
    bool taaHistoryValid=false;
    float taaStaticBlend=0.1f,taaDynamicBlend=0.2f;
    float taaDepthThreshold=0.05f,taaVelocityThreshold=0.01f,taaSharpen=0.0f;
    bool supersample4x=false; // HDR 几何 2×2 空间采样，后处理保持输出分辨率。
};
struct PostProcessPipelines {PipelineHandle extract,blur,toneMap,fxaa,overlay;};
struct OverlayQuad
{
    Vec4 rectangle,color{1,1,1,1},clip{0,0,1e6f,1e6f}; // 像素 x/y/width/height；颜色为线性 RGBA。
};
struct PointLight {Vec3 position{},radiance{};float range=10;};
inline constexpr std::uint32_t MaximumLocalLights=8;
struct LocalLight
{
    Vec3 position{},radiance{},direction{0,0,-1};
    float range=10,innerCos=.9f,outerCos=.8f;
    bool spot=false,castsShadow=false;
};
struct ClusteredLighting
{
    bool enabled=false,debug=false,bruteForce=false,counterReadback=false;
    std::vector<PointLight> lights;
    std::vector<std::uint64_t> ids;
    Mat4 view,clipFromView;
    float nearPlane=.1f,farPlane=1000;
    std::uint32_t capacity=65536;
};
struct ClusterReadback
{
    std::vector<std::array<std::uint32_t,2>> ranges;
    std::vector<std::uint32_t> rawCounts;
    std::vector<std::uint32_t> indices;
    std::array<std::uint32_t,8> counters{};
    std::vector<std::uint64_t> inputIds;
    std::vector<PointLight> lights;
    Mat4 view,clipFromView;
    float nearPlane=0,farPlane=0;
};
// 普通天空的方向渐变与太阳盘；EnvironmentSky 的 HDR 资产路径不受影响。
struct SkySettings
{
    Vec3 zenithColor{0.10f,0.18f,0.34f};
    Vec3 horizonColor{0.55f,0.66f,0.78f};
    Vec3 groundColor{0.16f,0.15f,0.14f};
    Vec3 sunColor{1.0f,0.96f,0.90f};
    float sunIntensity=12.0f;
    float sunAngularRadius=0.012f;
};
struct IndoorLightingSettings
{
    bool indirect=false,reflections=false,volumetricFog=false;
    float indirectStrength=1,reflectionStrength=1,fogDensity=.035f,fogDistance=25;
    Vec3 fogAlbedo{.85f,.9f,1};
    Mat4 view,clipFromView; // 与本帧几何实际抖动投影一致。
    bool SurfaceEnabled() const {return (indirect&&indirectStrength>0)||(reflections&&reflectionStrength>0);}
    bool Enabled() const {return SurfaceEnabled()||(volumetricFog&&fogDensity>0&&fogDistance>0);}
};
struct ForwardLighting
{
    Vec3 directionToLight{0,0,1},directionalRadiance{},sky{0.02f,0.04f,0.08f};
    SkySettings skySettings;
    bool directionalEnabled=false;
    float exposure=1;
    std::uint32_t pointCount=0;
    std::array<PointLight,MaximumPointLights> points;
    // G15：可选显示线性 LUT strip（1089×33，Rgba8 或 Rgba16Float）。仅输出 LUT 布局的
    // FinalOutput/ShadowOutput 采样；无效句柄时后端使用调用方提供的 1×1 占位纹理或拒绝该布局。
    TextureHandle colorLut;
    ClusteredLighting cluster;
    ShadingFrameBindings shading;
    std::vector<LocalLight> localLights;
    bool gpuDriven=false,meshletCulling=false,gpuDrivenHiZ=false;
    IndoorLightingSettings indoor;
};
struct Image {std::uint32_t width=0,height=0;std::vector<std::uint8_t> rgba8;};
struct RenderTargetDescription {std::uint32_t width=0,height=0;bool depth=true,srgb=false;TextureFormat format=TextureFormat::Rgba8;};
struct HdrImage {std::uint32_t width=0,height=0;std::vector<float> rgba32;};
struct DepthImage {std::uint32_t width=0,height=0;std::vector<float> depth;std::uint32_t layers=1;};
struct DepthNormalSettings {bool enabled=false;};
struct ScreenSpaceOcclusionSettings
{
    bool gtao=false,contactShadows=false;
    Mat4 view,clipFromView; // 实际抖动投影；与39输入、HDR几何像素视口同一相机。
    float radius=.6f,intensity=.8f,bias=.02f;
    float contactLength=.5f,contactBias=.02f,contactThickness=.1f,contactFade=.1f;
};
enum class ScreenSpaceOcclusionFallback : std::uint32_t
{None,MissingDepthNormals,IncompleteGeometry,UnsupportedPipeline,InvalidCamera,InitializationFailed,ResourceFailure};
struct ScreenSpaceOcclusionStatistics
{
    bool requested=false,valid=false,aoValid=false,contactValid=false;
    std::uint32_t width=0,height=0,halfWidth=0,halfHeight=0;
    ScreenSpaceOcclusionFallback fallback=ScreenSpaceOcclusionFallback::None;
    std::uint64_t allocatedBytes=0;
    std::uint64_t kernelCreations=0,programCompilations=0,artifactLoads=0,artifactRejections=0;
};
struct ScreenSpaceOcclusionReadback
{
    ScreenSpaceOcclusionStatistics statistics;
    // 实际全尺寸 RG16F 与半尺寸 RG16F 展开：full=(AO,contact)，half=(AO,有效样本)。
    std::vector<float> visibility,halfAo;
};
struct DepthNormalStatistics
{
    bool requested=false,valid=false,complete=false;
    std::uint32_t draws=0,unsupportedDraws=0;
    std::uint64_t allocatedBytes=0;
    std::uint64_t pipelineCreations=0,programCompilations=0,artifactLoads=0,artifactRejections=0;
};
// 正确性回读专用；depth 是预绘后/主颜色前的原 D32，octNormals 为 RG16F 展开浮点。
// 世界几何法线八面体编码 [-1,1]；(2,2) 表示无几何，complete=false 禁止作为完整 AO 输入。
struct DepthNormalReadback
{
    std::uint32_t layoutVersion=DepthNormalLayoutVersion,width=0,height=0;
    bool complete=false,valid=false;
    std::uint32_t draws=0,unsupportedDraws=0;
    std::vector<float> depth,octNormals;
};
struct ClusteredStatistics
{
    bool valid=false;
    std::uint64_t sourceFrame=0;
    std::array<std::uint32_t,8> counters{};
};
struct DrawBindingStatistics
{
    std::uint64_t pipelines=0,rootSignatures=0,descriptorTables=0,vertexBuffers=0,indexBuffers=0,constants=0;
    std::uint64_t instanceBuffers=0;
};
// 这里只报告实际生成/录制的候选命令；可见数需要独立 GPU 回读，不能用 CPU 估计填充。
struct GpuDrivenStatistics
{
    bool requested=false,valid=false,meshlets=false,hzbRequested=false,hzbValid=false;
    std::uint32_t sourceDraws=0,candidateCommands=0,gpuBuckets=0,cpuDraws=0,cpuBuckets=0,meshletCommands=0;
    std::array<std::uint32_t,11> fallbackReasons{};
};
// G06.09：间接参数回读的遮挡计数。visible 来自 GPU 写入的 ExecuteIndirect 参数，
// 不是 CPU 按包围盒估算；valid=false 表示当前设备还没有完成过任何回读。
// frame 是被统计帧的 sourceFrame，age 由调用方按当前帧计算，避免 HUD 把旧值当新值。
struct OcclusionCounters
{
    bool valid=false,overflow=false;
    std::uint64_t frame=0;
    std::uint32_t candidates=0,visible=0,culled=0;
    float occlusionRate=0;
};
// G06.11：RHI 遮挡 GPU 时间。prepass/HZB/cull/main 为最近一次完成回读的分段毫秒数；
// cull 为所有批次的求和，main 为最后一次 gather 后到全部间接绘制结束。
struct OcclusionTiming
{
    bool valid=false;
    std::uint64_t frame=0,frequency=0;
    double prepassMs=0,hzbMs=0,cullMs=0,mainMs=0,totalMs=0;
};
struct Statistics
{
    DepthNormalStatistics depthNormals;
    ScreenSpaceOcclusionStatistics screenSpaceOcclusion;
    GpuDrivenStatistics gpuDriven;
    ClusteredStatistics clustered;
    std::uint32_t hdrWidth=0,hdrHeight=0;
    std::uint64_t hdrCreations=0;
    std::uint32_t shadowResolution=0;
    std::uint64_t shadowCreations=0;
    std::uint32_t postWidth=0,postHeight=0,bloomWidth=0,bloomHeight=0;
    std::uint64_t postCreations=0,postResourceBytes=0;
    std::size_t buffers=0,textures=0,pipelines=0,retiring=0,inFlightBatches=0;
    std::uint64_t submittedFrames=0,globalWaits=0,completedValue=0;
    std::uint64_t debugWarnings=0,debugErrors=0;
    std::size_t trackedResources=0,trackedSubresources=0;
    std::uint64_t stateTransitions=0,bufferDecays=0;
    std::uint64_t uploadAllocations=0,uploadPagesCreated=0,uploadReservedBytes=0,uploadPeakUsedBytes=0,uploadFailures=0;
    std::uint64_t uploadSubmissions=0,uploadedBytes=0;
    std::uint32_t persistentDescriptors=0,descriptorCapacity=0,descriptorPeak=0,retiringDescriptors=0,rtvDescriptors=0,dsvDescriptors=0;
    std::uint32_t frameDescriptors=0,frameDescriptorCapacity=0,frameDescriptorPeak=0;
    std::uint64_t descriptorHeapsCreated=0,descriptorCopies=0,descriptorFailures=0;
    std::uint64_t memoryBudgetBytes=0,memoryReservedBytes=0,memoryUsedBytes=0,memoryFreeBytes=0,memoryPeakReservedBytes=0,memoryPeakUsedBytes=0;
    std::uint64_t memoryResources=0,memoryHeaps=0,memoryCommittedResources=0,memoryHeapCreations=0,memoryCommittedCreations=0,memoryResourceCreations=0,memoryBudgetFailures=0,stateActivations=0;
    std::uint64_t shaderProgramCompilations=0,shaderArtifactLoads=0,shaderArtifactRejections=0,shaderProgramHits=0;
    std::uint64_t pipelineCacheHits=0,pipelineCacheMisses=0,pipelineCreations=0,rootSignatureCreations=0,livePreparedPipelines=0,liveShaderPrograms=0;
    std::uint64_t pipelineLibraryHits=0,pipelineLibraryStores=0;
    DrawBindingStatistics opaqueBindings; // 最近一次提交的 Opaque 实际 Set 调用，不是所有 Pass 的总数。
    std::uint64_t frameInstances=0,frameInstanceBytes=0,frameDrawConstantBytes=0;
    std::uint64_t stagedCopies=0,stagedPayloadBytes=0,stagedFootprintBytes=0;
    // G06.08：实际走实例化遮挡批次的帧数（含首帧/ resize 后只做视锥剔除的间接帧）。
    std::uint64_t occlusionFrames=0,occlusionInstances=0;
    // G06.09：最近一次完成回读的遮挡计数；只读，HUD/测试不得修改。
    OcclusionCounters occlusion;
    // G06.11：最近一次完成回读的遮挡 GPU 分段时间；默认不产生查询。
    OcclusionTiming occlusionTiming;
};
struct GpuTiming
{
    bool heterogeneous=false,heterogeneousHiZ=false;
    std::array<std::uint64_t,2> heterogeneousTicks{};
    std::array<std::uint64_t,3> heterogeneousHiZTicks{};
    bool ambientOcclusionHalf=false,ambientOcclusion=false;
    std::array<std::uint64_t,2> aoTicks{},aoResolveTicks{};
    bool depthNormals=false;
    std::array<std::uint64_t,2> depthNormalTicks{};
    std::uint64_t sourceFrame=0,submission=0,frequency=0;
    bool clustered=false;
    std::array<std::uint64_t,2> clusterTicks{};
    std::array<std::uint64_t,4> ticks{}; // frame begin, geometry begin/end, frame end
    std::uint32_t draws=0;
    double frameMilliseconds=0,geometryMilliseconds=0;
    std::array<std::uint64_t,8> passTicks{}; // Forward: Clear/Sky/Opaque/Output 各一对。
    bool forward=false;
    std::array<std::uint64_t,2> shadowTicks{};
    bool shadow=false;
    std::array<std::uint64_t,12> postTicks{}; // Extract/HBlur/VBlur/ToneMap/FXAA/UI。
    bool post=false,postBloom=false,postToneMapping=false,postFxaa=false,postUi=false;
};
// G06.08：实例化遮挡批次。bounds.id 必须是 instances 的下标；主绘制在 HZB 可用时
// 走 depth prepass → HZB → compute 剔除 → ExecuteIndirect，否则只做视锥剔除的间接绘制。
// 目标管线必须是 PbrInstancedMesh（前向布局暂不支持），开启 HZB 时需要 DepthMode::ReadOnlyLessEqual。
struct OcclusionSettings
{
    bool enabled=true;
    float depthBias=1e-6f;
    // G06.09：按帧槽把 48B 间接参数异步复制到 READBACK，完成后填充 Statistics::occlusion。
    // 默认关闭；只有需要运行时 HUD/诊断的调用方开启，避免为纯绘制路径增加回读开销。
    bool counterReadback=false;
    // G06.11：为每批次记录 cull 查询并记录 prepass/HZB/main 边界，完成后填充
    // Statistics::occlusionTiming。默认关闭；仅 HZB 开启且批次数量可控时使用。
    bool telemetry=false;
};
// G06.10：多批次实例化遮挡。每个批次对应一次 DrawInstanced 与一次 ExecuteIndirect；
// bounds.id 是该批次实例缓冲内的局部下标（0..size-1），批次顺序必须与绘制顺序一致。
struct OcclusionBatch
{
    std::span<const se::VisibilityItem> bounds;
};
class Device;
class CommandContext
{
public:
    ~CommandContext();
    CommandContext(CommandContext&&) noexcept;
    CommandContext& operator=(CommandContext&&) noexcept;
    // 即使没有网格，天空也需要当前相机；必须在录制 Pass 前设置。
    Result ConfigureView(const Mat4& viewProjection);
    Result ConfigureForward(const ForwardLighting& lighting,PipelineHandle outputPipeline,PipelineHandle skyPipeline);
    // G06.08：在 Clear 之后、DrawInstanced 之前声明遮挡批次。bounds 与 instances 由调用方保活。
    // 非 Forward 帧只允许一次实例化绘制；G06.10 起 Forward 实例化可同时配置 Forward，但仍与阴影/后处理/GUI 互斥。
    Result ConfigureOcclusion(std::span<const se::VisibilityItem> bounds,const se::VisibilityFrustum& frustum,
        const OcclusionSettings& settings=OcclusionSettings{});
    // G06.10：多批次版本。Batch 顺序与后续 DrawInstanced 顺序一一对应；Forward/阴影/后处理/GUI 仍互斥。
    Result ConfigureOcclusion(std::span<const OcclusionBatch> batches,const se::VisibilityFrustum& frustum,
        const OcclusionSettings& settings=OcclusionSettings{});
    // 通用离屏场景目标；必须在 ConfigureForward/ConfigureGui 之前设置。
    Result ConfigureComposition(const FrameComposition& composition);
    // 非 Forward 场景 -> sceneTarget；GUI 在线性空间混合到 sRGB composite，再复制到交换链。
    Result ConfigureGui(TextureHandle sceneTarget,TextureHandle composite,PipelineHandle pipeline,
        const GuiGeometry& geometry,std::span<const TextureHandle> commandTextures);
    // 场景最终输出之后、编辑器 GUI 之前的 sRGB 游戏界面；帧开始时复制几何并保活资源。
    Result ConfigureGameUi(PipelineHandle pipeline,const GuiGeometry& geometry,std::span<const TextureHandle> commandTextures);
    Result ConfigureSceneOverlay(std::span<const SceneLine> lines,const std::filesystem::path& shader,float pixelScale=1,bool groundGrid=false);
    Result ConfigureParticles(const ParticleFrame& particles,const std::filesystem::path& shader);
    Result ConfigureShadows(const ShadowSettings& shadows,PipelineHandle depthPipeline);
    Result ConfigureEnvironment(const EnvironmentBindings& environment);
    Result ConfigurePostProcess(const PostProcessSettings& settings,const PostProcessPipelines& pipelines);
    Result ConfigureDepthNormals(const DepthNormalSettings& settings);
    Result ConfigureScreenSpaceOcclusion(const ScreenSpaceOcclusionSettings& settings);
    Result DrawOverlay(const OverlayQuad& quad);
    // W03：阴影投射统一走整份绘制描述，蒙皮投射需要顶点缓冲已记录调色板。
    Result DrawShadow(const DrawIndexed& draw);
    Result BeginPass(const PassDescription& pass);
    Result Draw(const DrawIndexed& draw);
    Result DrawInstanced(const DrawIndexed& draw,std::span<const InstanceParameters> instances);
    Result Triangle(PipelineHandle pipeline,const Mat4& viewProjection);
    Result EndPass();
    Result Close();
private:
    friend class Device;
    struct Impl;
    explicit CommandContext(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
class Device : public std::enable_shared_from_this<Device>
{
public:
    ~Device();
    Device(const Device&)=delete;
    Device& operator=(const Device&)=delete;
    static std::shared_ptr<Device> CreateD3D12(Surface surface,const Localization& text,Result& result,MemorySettings memory={});
    Result CreateBuffer(const BufferDescription& description,std::span<const std::byte> bytes,Buffer& output);
    Result CreateTexture(const TextureDescription& description,std::span<const std::uint8_t> rgba8,Texture& output);
    Result CreateRenderTarget(const RenderTargetDescription& description,Texture& output);
    Result CreateTextureSubresources(const TextureDescription& description,std::span<const TextureSubresource> subresources,Texture& output);
    // G04.06：按驻留预算钳制压缩纹理可见的最细 mip（重建 sRGB/线性两个 SRV）。
    // 只影响之后的绘制；已记录的绘制持有旧视图快照。
    Result SetTextureMipRange(const Texture& texture,std::uint32_t mostDetailedMip);
    // G04.07：分配完整 mip 链但只上传 [firstMip, …) 的连续区间；未上传的 mip 不可采样。
    Result CreateTextureSubresourcesRange(const TextureDescription& description,std::uint32_t firstMip,
        std::span<const TextureSubresource> subresources,Texture& output);
    // 向已存在的压缩纹理补传连续 mip 区间（从 firstMip 起）；同步完成。
    Result UploadTextureSubresourcesRange(const Texture& texture,std::uint32_t firstMip,
        std::span<const TextureSubresource> subresources);
    // staged 只可顺序写入一次；完整覆盖且相关 Fence 完成前不能 Draw。
    Result CreateStagedBuffer(const BufferDescription& description,std::uint64_t bytes,Buffer& output);
    Result CreateStagedTexture(const TextureDescription& description,Texture& output);
    Result QueryBufferUploadFootprint(std::uint64_t payloadBytes,UploadFootprint& output) const;
    Result QueryTextureUploadFootprint(const TextureDescription& description,std::uint32_t rows,UploadFootprint& output) const;
    Result UploadBufferRange(BufferHandle buffer,std::uint64_t offset,std::span<const std::byte> bytes);
    Result UploadTextureRows(TextureHandle texture,std::uint32_t firstRow,std::uint32_t rows,std::span<const std::uint8_t> rgba8);
    Result CreatePipeline(const PipelineDescription& description,Pipeline& output);
    Result BeginUploadBatch();
    Result EndUploadBatch(CompletionToken& token); // 已成功创建的资源一次提交；不等待 GPU。
    Result SetUploadMode(UploadMode mode); // 显式 drain 后切换基线/复用策略。
    Result ConfigureDescriptors(DescriptorLimits limits); // 无纹理/记录引用时，drain 后更改。
    Result TrimMemory(); // 显式 drain 后释放空 DEFAULT heap，不搬迁资源。
    Result Destroy(BufferHandle handle);
    Result Destroy(TextureHandle handle);
    Result Destroy(PipelineHandle handle);
    std::unique_ptr<CommandContext> BeginCommands(std::uint64_t sourceFrame=0);
    Result Resize(std::uint32_t width,std::uint32_t height);
    // capture 非空时显式等待该次复制；正常提交不为测量/截图全局等待。
    Result Submit(CommandContext& commands,CompletionToken& token,Image* capture=nullptr,HdrImage* hdrCapture=nullptr,DepthImage* shadowCapture=nullptr,ClusterReadback* clusterCapture=nullptr,DepthImage* sceneDepthCapture=nullptr,DepthNormalReadback* depthNormalCapture=nullptr,ScreenSpaceOcclusionReadback* occlusionCapture=nullptr);
    bool IsComplete(CompletionToken token) const;
    Result WaitIdle();
    void Collect();
    Statistics Stats() const;
    std::vector<std::string> DebugMessages() const; // 调试层 warning/error 的原始诊断，按需读取。
    AdapterInformation Adapter() const;
    bool Occluded() const;
    Result EnableGpuTiming(bool enabled);
    void SetCpuProfiler(Profiler* profiler);
    std::vector<GpuTiming> GpuTimings(); // 只读已完成结果，不等待 GPU。
    std::vector<GpuTiming> GpuTimingsSince(std::uint64_t submission); // 只复制严格大于游标的完成结果。
    Result ExportGpuTimingsCsv(const std::filesystem::path& path); // 显式 drain 后导出最后几帧。
    Result ExportGpuPassTimingsCsv(const std::filesystem::path& path);
    void Shutdown() noexcept;
private:
    friend class CommandContext;
    friend struct D3D12DiagnosticsAccess;
    struct Impl;
    Device();
    std::shared_ptr<Impl> impl_;
};
}
