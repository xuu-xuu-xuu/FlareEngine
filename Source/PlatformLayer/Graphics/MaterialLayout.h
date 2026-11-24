#pragma once
#include "CoreLayer/Math/Projection.h"
#include <cstddef>
#include <cstdint>
namespace flare::rhi
{
inline constexpr std::uint32_t MaterialLayoutVersion=1,MaterialConstantAlignment=256;
// RG104-15：独立几何输入格式；不替换绑定布局或 38 号材质 b4。
inline constexpr std::uint32_t DepthNormalLayoutVersion=39;
inline constexpr std::uint32_t ShadingExtensionLayoutVersion=40;
// RG104-16：GPU 广告牌粒子实例与常量布局；与 ParticleBindings.hlsli 严格对齐。
inline constexpr std::uint32_t ParticleLayoutVersion=41;
// 室内 SSGI 常量布局；与 IndoorLighting.hlsl IndoorConstants 严格对齐。
inline constexpr std::uint32_t IndoorLightingLayoutVersion=42;
// 自定义表面着色器扩展材质 b4 布局（复用既有 256B 对齐切片中的 176B，兼容内置 38 号布局）。
inline constexpr std::uint32_t UserSurfaceMaterialLayoutVersion=43;
struct alignas(16) IndoorLightingConstants
{
    Mat4 view,projection,inverseProjection,inverseView;
    Mat4 previousClipFromWorld;
    Vec4 viewport,strengths,fogAlbedo,sunDirection,sunRadiance,environment,ordinaryLightCount;
    std::array<Vec4,8> ordinaryPosition{},ordinaryRadiance{};
    std::array<Mat4,4> sunClip{};
    Vec4 sunSplits{},sunShadowParameters{},sampling{};
    std::uint32_t frameIndex=0;
    float historyBlendWeight=0.0f;
    float historyValid=0.0f;
    std::uint32_t atrousStep=1;
    Vec4 fogParameters{}; // x=heightFalloff, y=baseHeight, z=sunScatter, w=turbulence
};
static_assert(sizeof(IndoorLightingConstants)==1024&&alignof(IndoorLightingConstants)==16);
static_assert(offsetof(IndoorLightingConstants,previousClipFromWorld)==256);
static_assert(offsetof(IndoorLightingConstants,sunClip)==688);
static_assert(offsetof(IndoorLightingConstants,frameIndex)==992
    &&offsetof(IndoorLightingConstants,historyBlendWeight)==996
    &&offsetof(IndoorLightingConstants,historyValid)==1000
    &&offsetof(IndoorLightingConstants,atrousStep)==1004
    &&offsetof(IndoorLightingConstants,fogParameters)==1008);
struct alignas(16) ParticleInstanceGpuData
{
    float position[3]{};
    float size=0;
    float color[4]{};
    float params[4]{}; // x=rotationRadians, y=additive, z=softFactor, w=reserved
};
struct alignas(16) ParticleFrameConstants
{
    Mat4 clipFromWorld{};
    float cameraRight[4]{};
    float cameraUp[4]{};
    float viewport[4]{};
};
static_assert(sizeof(ParticleInstanceGpuData)==48&&alignof(ParticleInstanceGpuData)==16
    &&offsetof(ParticleInstanceGpuData,size)==12&&offsetof(ParticleInstanceGpuData,color)==16
    &&offsetof(ParticleInstanceGpuData,params)==32);
static_assert(sizeof(ParticleFrameConstants)==112&&alignof(ParticleFrameConstants)==16
    &&offsetof(ParticleFrameConstants,cameraRight)==64&&offsetof(ParticleFrameConstants,cameraUp)==80
    &&offsetof(ParticleFrameConstants,viewport)==96);
struct alignas(16) ShadingFrameConstants
{
    Vec4 modes{},viewport{},probeOrigin{},probeSpacing{},probeDimensions{};
    std::array<Vec4,9> globalSh{};
    std::array<Vec4,4> rectCenter{},rectHalfU{},rectHalfV{},rectRadiance{};
};
struct alignas(16) ShadingMaterialConstants
{
    Vec4 coat{},sheen{},anisotropy{},subsurface{},subsurfaceColor{},textureFlags{};
};
static_assert(sizeof(ShadingFrameConstants)==480&&offsetof(ShadingFrameConstants,globalSh)==80);
static_assert(offsetof(ShadingFrameConstants,rectCenter)==224&&offsetof(ShadingFrameConstants,rectRadiance)==416);
static_assert(sizeof(ShadingMaterialConstants)==96&&offsetof(ShadingMaterialConstants,textureFlags)==80);
inline constexpr std::uint32_t PbrMaterialLayoutVersion=2,PbrConstantStride=512;
inline constexpr std::uint32_t ForwardLayoutVersion=3,MaximumPointLights=8;
inline constexpr std::uint32_t ShadowLayoutVersion=4;
inline constexpr std::uint32_t EnvironmentLayoutVersion=5;
inline constexpr std::uint32_t PostProcessLayoutVersion=6;
inline constexpr std::uint32_t InstancingLayoutVersion=7,InstanceGpuStride=240,MaximumFrameInstances=65536;
inline constexpr std::uint32_t GuiLayoutVersion=8;
inline constexpr std::uint32_t SkyLayoutVersion=9;
struct alignas(16) SkyConstants
{
    Mat4 skyFromClip;
    Vec4 zenithColor,horizonColor,groundColor,sunDirectionIntensity,sunColor,parameters,cloudParameters;
};
static_assert(sizeof(SkyConstants)==176&&offsetof(SkyConstants,zenithColor)==64);
static_assert(offsetof(SkyConstants,horizonColor)==80&&offsetof(SkyConstants,groundColor)==96);
static_assert(offsetof(SkyConstants,sunDirectionIntensity)==112&&offsetof(SkyConstants,sunColor)==128);
static_assert(offsetof(SkyConstants,parameters)==144&&offsetof(SkyConstants,cloudParameters)==160);
struct alignas(16) GuiConstants {Vec4 scaleTranslate;};
static_assert(sizeof(GuiConstants)==16);
struct alignas(16) InstanceGpuData
{
    Mat4 clipFromLocal,worldFromLocal,normalFromLocal;
    Vec4 baseColor,parameters,options;
};
static_assert(sizeof(InstanceGpuData)==InstanceGpuStride&&alignof(InstanceGpuData)==16);
static_assert(offsetof(InstanceGpuData,worldFromLocal)==64&&offsetof(InstanceGpuData,normalFromLocal)==128);
static_assert(offsetof(InstanceGpuData,baseColor)==192&&offsetof(InstanceGpuData,parameters)==208&&offsetof(InstanceGpuData,options)==224);
struct alignas(16) PostProcessConstants {Vec4 parameters,resolution,options;};
struct alignas(16) OverlayConstants {Vec4 rectangle,color,viewport;};
static_assert(sizeof(PostProcessConstants)==48&&sizeof(OverlayConstants)==48);
struct alignas(16) ForwardFrameConstants
{
    Vec4 directionAndEnabled,directionalRadiance,skyAndExposure,pointCount;
    std::array<Vec4,MaximumPointLights> pointPositionRadius,pointRadiance;
};
static_assert(sizeof(ForwardFrameConstants)==320&&offsetof(ForwardFrameConstants,pointPositionRadius)==64);
static_assert(offsetof(ForwardFrameConstants,pointRadiance)==192);
struct alignas(16) ShadowFrameConstants
{
    ForwardFrameConstants lights;
    Mat4 clipFromWorld;
    Vec4 parameters;
};
static_assert(sizeof(ShadowFrameConstants)==400&&offsetof(ShadowFrameConstants,clipFromWorld)==320);
static_assert(offsetof(ShadowFrameConstants,parameters)==384);
struct alignas(16) EnvironmentFrameConstants
{
    ShadowFrameConstants shadow;
    Vec4 environment;
};
struct alignas(16) EnvironmentSkyConstants {Mat4 skyFromClip;Vec4 environment;};
static_assert(sizeof(EnvironmentFrameConstants)==416&&offsetof(EnvironmentFrameConstants,environment)==400);
static_assert(sizeof(EnvironmentSkyConstants)==80);
struct alignas(16) MeshDrawConstants
{
    Mat4 clipFromLocal,normalFromLocal;
    Vec4 tint,options;
};
static_assert(sizeof(MeshDrawConstants)==160&&alignof(MeshDrawConstants)==16);
static_assert(offsetof(MeshDrawConstants,clipFromLocal)==0&&offsetof(MeshDrawConstants,normalFromLocal)==64);
static_assert(offsetof(MeshDrawConstants,tint)==128&&offsetof(MeshDrawConstants,options)==144);
static_assert(sizeof(Mat4)==64);
struct alignas(16) PbrDrawConstants
{
    Mat4 clipFromLocal,worldFromLocal,normalFromLocal;
    Vec4 baseColor,cameraPosition,directionToLight,radiance,parameters,options;
    // W06：外观覆盖。x=模式（0 基础色/1 白模/2 Toon），y=混合，z=Toon 分档数，w=轮廓/边缘强度。
    Vec4 look;
    // W03：xy 为蒙皮调色板偏移/关节数；RG104：原保留 zw 为 IBL 多散射/法线高光 AA。
    // 零值兼容旧 shader；不改变布局/偏移，shader 内容摘要使产物缓存失效。
    Vec4 skin;
};
static_assert(sizeof(PbrDrawConstants)==320&&alignof(PbrDrawConstants)==16);
static_assert(offsetof(PbrDrawConstants,worldFromLocal)==64&&offsetof(PbrDrawConstants,normalFromLocal)==128);
static_assert(offsetof(PbrDrawConstants,baseColor)==192&&offsetof(PbrDrawConstants,cameraPosition)==208);
static_assert(offsetof(PbrDrawConstants,directionToLight)==224&&offsetof(PbrDrawConstants,radiance)==240);
static_assert(offsetof(PbrDrawConstants,parameters)==256&&offsetof(PbrDrawConstants,options)==272);
static_assert(offsetof(PbrDrawConstants,look)==288&&offsetof(PbrDrawConstants,skin)==304);

// G 系列预留的绑定布局版本。G00 只登记，不创建对应描述符布局；
// 新版本不得复用 1–9 的既有布局，也不得与 BasicLit 技术号混用。
inline constexpr std::uint32_t VertexCompressedLayoutVersion=20;
inline constexpr std::uint32_t ClusteredForwardLayoutVersion=21;
inline constexpr std::uint32_t CascadedShadowLayoutVersion=22;
inline constexpr std::uint32_t AtmosphereLayoutVersion=23;
inline constexpr std::uint32_t AmbientOcclusionLayoutVersion=24;
inline constexpr std::uint32_t VolumetricFogLayoutVersion=25;
inline constexpr std::uint32_t TemporalAALayoutVersion=26;
inline constexpr std::uint32_t ExposureColorLayoutVersion=27;
inline constexpr std::uint32_t MeshletLayoutVersion=28;
// RG104-17.02：硬件 Amplification/Mesh Shader 管线布局（space1 剔除与几何视图）。
struct alignas(16) GpuMeshletItem
{
    std::uint32_t firstIndex=0,triangleCount=0,indexStride=4,flags=0;
};
static_assert(sizeof(GpuMeshletItem)==16&&alignof(GpuMeshletItem)==16
    &&offsetof(GpuMeshletItem,firstIndex)==0&&offsetof(GpuMeshletItem,triangleCount)==4
    &&offsetof(GpuMeshletItem,indexStride)==8&&offsetof(GpuMeshletItem,flags)==12);
struct alignas(16) MeshletCullConstants
{
    std::array<Vec4,6> planes{};
    std::uint32_t totalMeshlets=0,reserved[3]{};
    Mat4 clipFromWorld{};
    Vec4 screen{};
    std::uint32_t occlusionEnabled=0,occlusionReserved[3]{};
    std::uint32_t padding[12]{};
};
static_assert(sizeof(MeshletCullConstants)==256
    &&offsetof(MeshletCullConstants,totalMeshlets)==96
    &&offsetof(MeshletCullConstants,clipFromWorld)==112
    &&offsetof(MeshletCullConstants,screen)==176
    &&offsetof(MeshletCullConstants,occlusionEnabled)==192);
// G15：输出 LUT 的显示线性 strip。t0 场景、t1 阴影预览（仅 ShadowOutput）、t2 33³ 2D strip；
// 编辑器 BasicLit（ShadowOutput）与 Lighting 演示（FinalOutput）共用此布局，t1 在 FinalOutput 中不使用。
// G15.09：strip 可为 Rgba8（兼容）或 Rgba16Float（保留 >1 的显示线性值与精度）。
inline constexpr std::uint32_t OutputLutLayoutVersion=29;
// G06.10：Forward 实例化的组合布局。在 InstancingLayoutVersion 的实例 SRV（t7）
// 之外追加 ForwardConstants（b1），用于 HDR 前向场景；b0 仍提供相机位置等逐绘制常量。
// 与 7/3 号布局都不兼容（参数表为 b0/纹理表/b1/实例 SRV 四槽），因此单独占用 30。
inline constexpr std::uint32_t ForwardInstancingLayoutVersion=30;
// G15：ToneMap 的扩展绑定布局。t0 场景、t1 Bloom、t2 1×1 曝光、t3 33³ LUT；
// 与旧 PostProcessLayoutVersion 的 t0/t1 布局不兼容，因此单独占用 27 而不复用 6。
inline constexpr std::uint32_t ToneMapLayoutVersion=ExposureColorLayoutVersion;
// W03：蒙皮绑定布局。在受光/阴影技术原有槽位（b0 逐绘制常量、贴图表、b1 帧常量）
// 之外追加 VS 的 t8 ByteAddressBuffer 调色板；阴影深度只用 b0 + t8。
// 与 4 号 ShadowLayoutVersion / 3 号 ForwardLayoutVersion 不兼容，因此单独占用 31。
inline constexpr std::uint32_t SkinnedLayoutVersion=31;
// G08：22 是静态 CSM；32 保留相同帧常量并附加 t8 蒙皮，旧 31 不变。
inline constexpr std::uint32_t SkinnedCascadedLayoutVersion=32;
struct alignas(16) CascadedShadowFrameConstants
{
    ForwardFrameConstants lights;
    std::array<Mat4,4> clipFromWorld;
    Vec4 splits; // xyzw 为最多四级分割远距。
    Vec4 viewDepth; // 世界位置到正视深的平面方程。
    Vec4 parameters; // inverseResolution、depthBias、slopeBias、normalBias。
    Vec4 options; // cascadeCount、blendFraction、debug、cascadeNear。
    Vec4 environment;
};
static_assert(sizeof(CascadedShadowFrameConstants)==656);
static_assert(offsetof(CascadedShadowFrameConstants,clipFromWorld)==320);
static_assert(offsetof(CascadedShadowFrameConstants,splits)==576);
static_assert(offsetof(CascadedShadowFrameConstants,viewDepth)==592);
static_assert(offsetof(CascadedShadowFrameConstants,parameters)==608);
static_assert(offsetof(CascadedShadowFrameConstants,options)==624);
static_assert(offsetof(CascadedShadowFrameConstants,environment)==640);

inline constexpr std::uint32_t SkinnedClusteredLayoutVersion=33;
// G08.02：独立 b3 阴影过滤常量，不改变既有 CSM/Forward+ 的 b1 或根签名。
inline constexpr std::uint32_t CascadedShadowFilterLayoutVersion=34;
inline constexpr std::uint32_t SkinnedCascadedShadowFilterLayoutVersion=35;
inline constexpr std::uint32_t ClusteredShadowFilterLayoutVersion=36;
inline constexpr std::uint32_t SkinnedClusteredShadowFilterLayoutVersion=37;
// 05：独立的材质覆盖 schema，与 base bindingLayoutVersion 正交组合，避免组合号爆炸。
inline constexpr std::uint32_t MaterialSurfaceLayoutVersion=38;
struct alignas(16) MaterialSurfaceConstants
{
    Vec4 baseColorFactor; // 来自唯一 CPU RGBA 因子 DrawIndexed.tint。
    Vec4 coverage; // cutoff、MaterialAlphaMode、doubleSided、世界矩阵行列式符号。
};
static_assert(sizeof(MaterialSurfaceConstants)==32&&alignof(MaterialSurfaceConstants)==16);
static_assert(offsetof(MaterialSurfaceConstants,coverage)==16);

struct alignas(16) UserMaterialSurfaceConstants
{
    Vec4 baseColorFactor;
    Vec4 coverage;
    Vec4 flareTime; // x=time, y=sin(time), z=cos(time), w=deltaTime
    std::array<Vec4,8> flareUserParams{};
};
static_assert(sizeof(UserMaterialSurfaceConstants)==176&&alignof(UserMaterialSurfaceConstants)==16);
static_assert(offsetof(UserMaterialSurfaceConstants,flareTime)==32&&offsetof(UserMaterialSurfaceConstants,flareUserParams)==48);

inline constexpr bool IsMaterialSurfaceLayout(std::uint32_t version) noexcept
{
    return version==MaterialSurfaceLayoutVersion||version==UserSurfaceMaterialLayoutVersion;
}

struct alignas(16) ShadowFilterConstants
{
    Vec4 parameters; // tan(角半径)、搜索世界距离、最大半径 texel、启用。
    Vec4 options; // debug 模式（0 正常、1 visibility、2 世界半径/距离/命中比）、保留。
    std::array<Vec4,4> cascadeMetrics; // 世界深度跨度、每 texel 的世界 XY 尺度、保留。
};
static_assert(sizeof(ShadowFilterConstants)==96&&alignof(ShadowFilterConstants)==16);
static_assert(offsetof(ShadowFilterConstants,options)==16&&offsetof(ShadowFilterConstants,cascadeMetrics)==32);

struct alignas(16) ClusteredFrameConstants
{
    CascadedShadowFrameConstants shadow;
    Mat4 view,projection,inverseProjection;
    Vec4 clusterParameters; // near,far,lightCount,capacity
    Vec4 clusterViewport; // 实际光栅宽高及倒数，包含空间采样倍数。
    Vec4 clusterModes; // debug,bruteForce,保留。
};
static_assert(sizeof(ClusteredFrameConstants)==896);
static_assert(offsetof(ClusteredFrameConstants,view)==656&&offsetof(ClusteredFrameConstants,projection)==720);
static_assert(offsetof(ClusteredFrameConstants,inverseProjection)==784&&offsetof(ClusteredFrameConstants,clusterParameters)==848);
static_assert(offsetof(ClusteredFrameConstants,clusterViewport)==864&&offsetof(ClusteredFrameConstants,clusterModes)==880);

struct LayoutVersionRecord
{
    std::uint32_t version;
    const char* system;
    const char* fields;
    std::uint32_t bytes; // 0 表示尚未冻结字节数
};

inline constexpr LayoutVersionRecord GSeriesLayoutVersions[] =
{
    {VertexCompressedLayoutVersion,"G03","position/normal/uv/tangent 压缩布局",0},
    {ClusteredForwardLayoutVersion,"G09","32B点光/uint32索引/固定集群，b1=896B",896},
    {CascadedShadowLayoutVersion,"G08","四级矩阵、分割/视深/混合/环境（b1）",656},
    {AtmosphereLayoutVersion,"G11","大气 LUT 与太阳参数",0},
    {AmbientOcclusionLayoutVersion,"G10","AO 半径、深度重建与升采样参数",0},
    {VolumetricFogLayoutVersion,"G11","雾密度、风与光线步进参数",0},
    {TemporalAALayoutVersion,"G14","抖动、历史与邻域夹取参数",0},
    {ExposureColorLayoutVersion,"G15","直方图、曝光与 LUT 参数",0},
    {MeshletLayoutVersion,"G07","cluster/meshlet 描述、剔除常量与间接参数",sizeof(MeshletCullConstants)},
    {OutputLutLayoutVersion,"G15","输出 LUT strip（t0 场景/t1 阴影/t2 33³，Rgba8 或 Rgba16F）",0},
    {ForwardInstancingLayoutVersion,"G06","Forward 实例化（b0/纹理表/b1/实例 SRV）",0},
    {SkinnedLayoutVersion,"W03","蒙皮调色板（受光：b0/纹理表/b1/t8；阴影深度：b0/t8）",0},
    {SkinnedCascadedLayoutVersion,"G08/W03","CSM 帧常量与 t8 蒙皮调色板",656},
    {SkinnedClusteredLayoutVersion,"G09/W03","集群帧常量及 t8 蒙皮",896},
    {CascadedShadowFilterLayoutVersion,"G08.02","CSM b1 656B，追加96B过滤b3",96},
    {SkinnedCascadedShadowFilterLayoutVersion,"G08.02/W03","CSM蒙皮追加96B过滤b3",96},
    {ClusteredShadowFilterLayoutVersion,"G08.02/G09","集群b1 896B，追加96B过滤b3",96},
    {SkinnedClusteredShadowFilterLayoutVersion,"G08.02/G09/W03","集群蒙皮追加96B过滤b3",96},
    {MaterialSurfaceLayoutVersion,"RG104-05/15","独立材质覆盖b4：RGBA因子/cutoff/mode/doubleSided",32},
    {DepthNormalLayoutVersion,"RG104-15/G10","独立 D32 + RG16F 世界几何法线 oct（每像素8B，非CB）",8},
    {ShadingExtensionLayoutVersion,"RG104-08/09/10/11/12","独立光照扩展 b5=480B/b6=96B/t13..31，覆盖38正交",480},
    {ParticleLayoutVersion,"RG104-16","GPU粒子广告牌实例(t0 48B)与常量b0=112B",112},
    {IndoorLightingLayoutVersion,"RG104-SSGI","室内SSGI时域重投影、帧抖动、历史权重、À-Trous与湍流雾常量b0=1024B",1024},
    {UserSurfaceMaterialLayoutVersion,"SurfaceShader","自定义表面着色器b4：RGBA/覆盖/时间/8×Vec4参数",176},
};
}
