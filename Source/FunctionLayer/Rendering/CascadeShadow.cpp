#include "CascadeShadow.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace se
{
namespace
{
constexpr std::array<float,CascadeShadowMaximumCascades> NormalizedSplits{0.10f,0.35f,1.0f};
bool ValidDepth(float value){return std::isfinite(value)&&value>0;}
bool Unproject(const Mat4& inverse,float x,float y,float z,Vec3& output)
{
    const auto world=inverse*Vec4{x,y,z,1};
    if(!IsFinite(world)||std::abs(world.w)<1e-8f)return false;
    output={world.x/world.w,world.y/world.w,world.z/world.w};return IsFinite(output);
}
bool Fit(const std::array<Vec3,4>& nearCorners,const std::array<Vec3,4>& farCorners,
    float actualNear,float actualFar,float nearPlane,float shadowDistance,const Vec3& directionToLight,
    std::uint32_t count,std::uint32_t resolution,float casterPadding,float filterGuardTexels,float filterGuardWorld,CascadeShadowSettings& output)
{
    if(count<1||count>CascadeShadowMaximumCascades||resolution<256||resolution>4096
        ||!ValidDepth(nearPlane)||!ValidDepth(actualNear)||!ValidDepth(actualFar)||actualFar<=actualNear
        ||!ValidDepth(shadowDistance)||shadowDistance<=nearPlane||shadowDistance>actualFar*1.002f
        ||!IsFinite(directionToLight)||!std::isfinite(casterPadding)||casterPadding<0||casterPadding>10000
        ||!std::isfinite(filterGuardTexels)||filterGuardTexels<3||filterGuardTexels>65||2*filterGuardTexels>=resolution
        ||!std::isfinite(filterGuardWorld)||filterGuardWorld<0||filterGuardWorld>.25f)return false;
    const auto z=Normalize(directionToLight);if(LengthSquared(z)<.99f)return false;
    const Vec3 up=std::abs(z.y)>.95f?Vec3{0,0,1}:Vec3{0,1,0};
    const auto x=Normalize(Cross(up,z)),y=Cross(z,x);
    CascadeShadowSettings candidate;candidate.cascadeCount=count;candidate.resolution=resolution;
    candidate.blendFraction=.10f;candidate.casterDepthPadding=casterPadding;
    for(std::uint32_t i=0;i<count;++i)candidate.splitDistances[i]=nearPlane+(shadowDistance-nearPlane)*NormalizedSplits[i];
    candidate.coverageDistance=candidate.splitDistances[count-1];
    for(std::uint32_t cascade=0;cascade<count;++cascade)
    {
        const float previous=cascade?candidate.splitDistances[cascade-1]:nearPlane;
        const float lowerBlend=cascade?((candidate.splitDistances[cascade-1]-(cascade>1?candidate.splitDistances[cascade-2]:nearPlane))*candidate.blendFraction):0;
        const float upperBlend=cascade+1<count?(candidate.splitDistances[cascade]-previous)*candidate.blendFraction:0;
        const float fitNear=std::max(nearPlane,previous-lowerBlend),fitFar=std::min(shadowDistance,candidate.splitDistances[cascade]+upperBlend);
        candidate.fitNear[cascade]=fitNear;candidate.fitFar[cascade]=fitFar;
        std::array<Vec3,8> corners;
        for(size_t i=0;i<4;++i)
        {
            if(!IsFinite(nearCorners[i])||!IsFinite(farCorners[i]))return false;
            const auto ray=farCorners[i]-nearCorners[i];
            corners[i]=nearCorners[i]+ray*((fitNear-actualNear)/(actualFar-actualNear));
            corners[i+4]=nearCorners[i]+ray*((fitFar-actualNear)/(actualFar-actualNear));
        }
        Vec3 center{};for(const auto corner:corners)center+=corner;center=center/8.f;
        float radius=0;for(const auto corner:corners)radius=std::max(radius,Length(corner-center));
        if(!ValidDepth(radius))return false;
        // 对旋转不变的包围球半径量化，避免浮点舍入使投影尺度逐帧变动。
        // 默认三个 texel 保持旧 5×5 PCF；PCSS 留最大核半径与 texel center/snap 的余量。
        radius=std::ceil((radius+filterGuardWorld)*16.f)/16.f;
        radius*=float(resolution)/(float(resolution)-2*filterGuardTexels);
        const double texel=2.0*radius/double(resolution);
        const double coordinateX=double(Dot(center,x)),coordinateY=double(Dot(center,y));
        const float snappedX=float(std::round(coordinateX/texel)*texel),snappedY=float(std::round(coordinateY/texel)*texel);
        const auto snappedCenter=center+x*(snappedX-float(coordinateX))+y*(snappedY-float(coordinateY));
        float minimum=std::numeric_limits<float>::max(),maximum=-minimum;
        for(const auto corner:corners){const auto value=Dot(corner-snappedCenter,z);minimum=std::min(minimum,value);maximum=std::max(maximum,value);}
        const auto position=snappedCenter+z*(maximum+casterPadding+1.f);
        Mat4 world;
        for(size_t row=0;row<3;++row)
        {
            const float a[]{x.x,x.y,x.z},b[]{y.x,y.y,y.z},c[]{z.x,z.y,z.z},p[]{position.x,position.y,position.z};
            world(row,0)=a[row];world(row,1)=b[row];world(row,2)=c[row];world(row,3)=p[row];
        }
        const auto lightInverse=InverseRigid(world);
        const auto projection=OrthographicRH(radius*2,radius*2,.1f,maximum-minimum+casterPadding+2.f);
        if(!lightInverse||!projection)return false;
        candidate.clipFromWorld[cascade]=*projection * *lightInverse;
        candidate.sphereCenter[cascade]=snappedCenter;candidate.sphereRadius[cascade]=radius;
        if(!IsFinite(candidate.clipFromWorld[cascade]))return false;
    }
    output=candidate;return true;
}
}
bool PrepareCascadeShadows(const Mat4& view,const Mat4& projection,float nearPlane,float farPlane,float shadowDistance,
    const Vec3& directionToLight,std::uint32_t count,std::uint32_t resolution,CascadeShadowSettings& output,float casterPadding,float filterGuardTexels,float filterGuardWorld)
{
    if(!IsFinite(view)||!IsFinite(projection)||!ValidDepth(nearPlane)||!ValidDepth(farPlane)||farPlane<=nearPlane
        ||!ValidDepth(shadowDistance)||shadowDistance>farPlane)return false;
    const auto world=InverseRigid(view),inverseProjection=Inverse(projection);if(!world||!inverseProjection)return false;
    const auto projectedNear=projection*Vec4{0,0,-nearPlane,1},projectedFar=projection*Vec4{0,0,-farPlane,1};
    if(std::abs(projectedNear.w)<1e-8f||std::abs(projectedFar.w)<1e-8f
        ||std::abs(projectedNear.z/projectedNear.w)>.001f||std::abs(projectedFar.z/projectedFar.w-1.f)>.001f)return false;
    std::array<Vec3,4> nearCorners,farCorners;float actualNear=0,actualFar=0;
    constexpr float coordinates[4][2]{{-1,-1},{1,-1},{1,1},{-1,1}};
    for(size_t i=0;i<4;++i)
    {
        Vec3 a,b;if(!Unproject(*inverseProjection,coordinates[i][0],coordinates[i][1],0,a)
            ||!Unproject(*inverseProjection,coordinates[i][0],coordinates[i][1],1,b))return false;
        if(i==0){actualNear=-a.z;actualFar=-b.z;}
        const auto aw=*world*Vec4{a.x,a.y,a.z,1},bw=*world*Vec4{b.x,b.y,b.z,1};
        nearCorners[i]={aw.x,aw.y,aw.z};farCorners[i]={bw.x,bw.y,bw.z};
    }
    // 仅用归一深度误差不足以核对 far（透视深度在远端高度压缩）。
    // 这里比实际反投影的视深，允许 float 矩阵在 1000 m 处的小量精度损失。
    if(std::abs(actualNear-nearPlane)>nearPlane*.002f||std::abs(actualFar-farPlane)>farPlane*.002f)return false;
    return Fit(nearCorners,farCorners,actualNear,actualFar,nearPlane,shadowDistance,directionToLight,count,resolution,casterPadding,filterGuardTexels,filterGuardWorld,output);
}
bool PrepareCascadeShadows(const Mat4& viewProjection,float nearPlane,float farPlane,const Vec3& directionToLight,
    std::uint32_t count,std::uint32_t resolution,CascadeShadowSettings& output)
{
    if(!IsFinite(viewProjection)||!ValidDepth(nearPlane)||!ValidDepth(farPlane)||farPlane<=nearPlane)return false;
    const auto inverse=Inverse(viewProjection);if(!inverse)return false;
    std::array<Vec3,4> nearCorners,farCorners;Vec3 nearCenter,farCenter;
    if(!Unproject(*inverse,0,0,0,nearCenter)||!Unproject(*inverse,0,0,1,farCenter))return false;
    constexpr float coordinates[4][2]{{-1,-1},{1,-1},{1,1},{-1,1}};
    for(size_t i=0;i<4;++i)if(!Unproject(*inverse,coordinates[i][0],coordinates[i][1],0,nearCorners[i])
        ||!Unproject(*inverse,coordinates[i][0],coordinates[i][1],1,farCorners[i]))return false;
    const auto actualFar=nearPlane+Length(farCenter-nearCenter);
    return Fit(nearCorners,farCorners,nearPlane,actualFar,nearPlane,farPlane,directionToLight,count,resolution,100.f,3.f,0.f,output);
}
CascadeSelection SelectCascade(const CascadeShadowSettings& settings,float viewDepth)
{
    CascadeSelection selection;if(settings.cascadeCount==0)return selection;
    const auto last=settings.cascadeCount-1;
    if(!std::isfinite(viewDepth)||viewDepth>=settings.splitDistances[last]){selection.first=selection.second=last;return selection;}
    for(std::uint32_t index=0;index<last;++index)
    {
        // 与着色器共用分割点：混合带每侧为本级跨度的 blendFraction。
        const auto previous=index?settings.splitDistances[index-1]:settings.fitNear[0];
        const auto width=(settings.splitDistances[index]-previous)*std::clamp(settings.blendFraction,0.f,.5f);
        if(width>1e-6f&&viewDepth>=settings.splitDistances[index]-width&&viewDepth<=settings.splitDistances[index]+width)
        {selection.first=index;selection.second=index+1;selection.blend=std::clamp((viewDepth-(settings.splitDistances[index]-width))/(2*width),0.f,1.f);return selection;}
    }
    std::uint32_t index=0;while(index<last&&viewDepth>settings.splitDistances[index])++index;
    selection.first=selection.second=index;return selection;
}
}
