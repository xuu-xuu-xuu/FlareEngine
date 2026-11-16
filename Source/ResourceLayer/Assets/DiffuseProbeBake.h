#pragma once
#include "HdrImage.h"
#include "CoreLayer/Math/SphericalHarmonics.h"
#include <array>
#include <span>

namespace se
{
// 复用既有 equirect 的世界方向：u=atan2(z,x)/(2*pi)+.5，v=acos(y)/pi。
// 每行使用精确 Δphi*(cos(theta0)-cos(theta1))，所有 texel 总固角为 4*pi。
// 每个 texel 当作常量辐亮度，解析积分九个 basis；1x1 白环境也不会生出方向性。
double EquirectangularTexelSolidAngle(std::uint32_t width,std::uint32_t height,std::uint32_t row) noexcept;
bool ProjectEnvironmentSh9(const FloatImage& image,Sh9Rgb& radiance);

// 纯 CPU 烘焙输入；三角形已经在世界空间，不借用运行时 World/RHI 对象。
struct DiffuseBakeTriangle
{
    Vec3 a{},b{},c{},baseColor{1,1,1};
    bool doubleSided=false;
};
struct DiffuseBakePointLight
{
    Vec3 position{},radiance{};
    float range=1;
};
struct DiffuseProbeScene
{
    std::span<const DiffuseBakeTriangle> triangles;
    std::span<const DiffuseBakePointLight> lights;
    const FloatImage* environment=nullptr; // ray miss 的入射 radiance；空则黑。
};
struct DiffuseProbeBakeSettings
{
    std::uint32_t samples=4096;
    float rayBias=.001f,maxDistance=1000;
};
// 首交面只受点灯的有遮挡直接光，Lambert baseColor/pi 出射，再投影到探针。
// 因此这是一次真实 diffuse bounce，不含递归反弹、镜面、透明/材质贴图或直接点灯项。
bool BakeDiffuseProbe(const DiffuseProbeScene& scene,Vec3 position,
    const DiffuseProbeBakeSettings& settings,Sh9Rgb& diffuse);
struct DiffuseProbeGrid
{
    Vec3 origin{},spacing{1,1,1};
    std::array<std::uint32_t,3> dimensions{2,2,2};
    std::vector<Sh9Rgb> diffuse;
    std::vector<std::uint8_t> probeValid,cellValid;
    // Probe 索引 x+y*nx+z*nx*ny；cell 索引用 nx-1 / ny-1。
    // 与任一场景三角形 AABB 相交的 cell 保守 invalid，防止穿墙插值。
};
bool BakeDiffuseProbeGrid(const DiffuseProbeScene& scene,Vec3 origin,Vec3 spacing,
    std::array<std::uint32_t,3> dimensions,const DiffuseProbeBakeSettings& settings,DiffuseProbeGrid& output);
struct DiffuseProbeSample
{
    Vec3 diffuse{};
    bool usedLocal=false;
    std::array<std::uint32_t,8> corners{};
    std::array<float,8> weights{};
};
// 精确三线性 8 角；域外、跨墙 cell、任角 invalid 回 global，返回 usedLocal=false。
// 值保留 signed SH 负瓣；产品着色端决定 clamp。参数/表损坏失败保留 output。
bool SampleDiffuseProbeGrid(const DiffuseProbeGrid& grid,const Sh9Rgb& globalDiffuse,
    Vec3 position,Vec3 normal,DiffuseProbeSample& output);
}
