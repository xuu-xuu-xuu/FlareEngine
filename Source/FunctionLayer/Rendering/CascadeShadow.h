#pragma once
#include "CoreLayer/Math/Projection.h"
#include <array>
#include <cstdint>

namespace se
{
// 级划分使用归一化距离 {0.10,0.35,1.0}；count=2/1 保留前两级/最近一级，
// 因而属于明确缩短远端距离的降级，不重新分配分割点。实际拟合覆盖相邻混合带。
inline constexpr std::uint32_t CascadeShadowMaximumCascades=3;
struct CascadeShadowSettings
{
    std::uint32_t cascadeCount=0,resolution=0;
    std::array<Mat4,CascadeShadowMaximumCascades> clipFromWorld{};
    std::array<float,CascadeShadowMaximumCascades> splitDistances{};
    std::array<Vec3,CascadeShadowMaximumCascades> sphereCenter{};
    std::array<float,CascadeShadowMaximumCascades> sphereRadius{};
    float blendFraction=0.10f;
    std::array<float,CascadeShadowMaximumCascades> fitNear{},fitFar{};
    float coverageDistance=0,casterDepthPadding=0;
};
// 失败不替换 output；cascadeCount 只接受 1..3。
bool PrepareCascadeShadows(const Mat4& viewProjection,float nearPlane,float farPlane,const Vec3& directionToLight,
    std::uint32_t cascadeCount,std::uint32_t resolution,CascadeShadowSettings& output);
// near/far 必须对应实际 projection；shadowDistance 是独立的接收范围。
// casterDepthPadding 只向光源方向扩展深度，保留在接收体视锥外但仍投影到体内的投射者。
bool PrepareCascadeShadows(const Mat4& view,const Mat4& projection,float nearPlane,float farPlane,float shadowDistance,
    const Vec3& directionToLight,std::uint32_t cascadeCount,std::uint32_t resolution,CascadeShadowSettings& output,
    float casterDepthPadding=100.0f,float filterGuardTexels=3.0f,float filterGuardWorld=0.0f);
struct CascadeSelection
{
    std::uint32_t first=0,second=0;
    float blend=0; // 0=只用 first，1=只用 second。
};
// viewDepth 为沿相机前向的正距离；超出最后一级时钳制到最后一级。
CascadeSelection SelectCascade(const CascadeShadowSettings& settings,float viewDepth);
}
