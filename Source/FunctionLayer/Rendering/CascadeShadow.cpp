#include "CascadeShadow.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

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
        ||!ValidDepth(nearPlane)||!ValidDepth(actualNear)||!ValidDepth(actualFar)||actualFar<=actualNear)return false;
    const float farTolerance=std::clamp((actualFar/actualNear)*2e-6f,.002f,.05f);
    if(!ValidDepth(shadowDistance)||shadowDistance<=nearPlane||shadowDistance>actualFar*(1.f+farTolerance)
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
    // 这里比实际反投影的视深，容差随远近比反映 float32 投影逆矩阵的 ULP 量化下界。
    const float farTolerance=std::clamp((farPlane/nearPlane)*2e-6f,.002f,.05f);
    if(std::abs(actualNear-nearPlane)>nearPlane*.002f||std::abs(actualFar-farPlane)>farPlane*farTolerance)return false;
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

namespace
{
std::uint32_t CanonicalFloatBits(float v) noexcept
{
    if(v==0.f)return 0u;
    std::uint32_t bits=0;
    std::memcpy(&bits,&v,sizeof(bits));
    return bits;
}
std::uint64_t MixHash64(std::uint64_t seed,std::uint64_t value) noexcept
{
    value+=0x9E3779B97F4A7C15ull;
    value=(value^(value>>30))*0xBF58476D1CE4E5B9ull;
    value=(value^(value>>27))*0x94D049BB133111EBull;
    value^=(value>>31);
    return seed^(value+0x9E3779B97F4A7C15ull+(seed<<6)+(seed>>2));
}
bool OverlapsOrthographicCascade(const Mat4& clipFromWorld,const Aabb& bounds) noexcept
{
    if(!IsFinite(bounds.minimum)||!IsFinite(bounds.maximum)
        ||bounds.minimum.x>bounds.maximum.x||bounds.minimum.y>bounds.maximum.y||bounds.minimum.z>bounds.maximum.z)
        return true;
    Vec3 clipMin{std::numeric_limits<float>::max(),std::numeric_limits<float>::max(),std::numeric_limits<float>::max()};
    Vec3 clipMax{-clipMin.x,-clipMin.y,-clipMin.z};
    for(int corner=0;corner<8;++corner)
    {
        const Vec3 p{
            (corner&1)?bounds.maximum.x:bounds.minimum.x,
            (corner&2)?bounds.maximum.y:bounds.minimum.y,
            (corner&4)?bounds.maximum.z:bounds.minimum.z};
        const Vec4 c=clipFromWorld*Vec4{p.x,p.y,p.z,1.f};
        if(!IsFinite(c))return true;
        clipMin={(std::min)(clipMin.x,c.x),(std::min)(clipMin.y,c.y),(std::min)(clipMin.z,c.z)};
        clipMax={(std::max)(clipMax.x,c.x),(std::max)(clipMax.y,c.y),(std::max)(clipMax.z,c.z)};
    }
    return !(clipMax.x<-1.f||clipMin.x>1.f||clipMax.y<-1.f||clipMin.y>1.f||clipMax.z<0.f||clipMin.z>1.f);
}
}

std::uint64_t HashShadowTransform(const Mat4& world) noexcept
{
    std::uint64_t h=0x6A09E667F3BCC908ull;
    for(const float v:world.values)h=MixHash64(h,CanonicalFloatBits(v));
    return h?h:1ull;
}

std::uint64_t HashShadowMaskMaterial(std::uint64_t textureKey,float alphaCutoff,float alphaScale,bool doubleSided) noexcept
{
    std::uint64_t h=0xBB67AE8584CAA73Bull;
    h=MixHash64(h,textureKey);
    h=MixHash64(h,CanonicalFloatBits(alphaCutoff));
    h=MixHash64(h,CanonicalFloatBits(alphaScale));
    h=MixHash64(h,doubleSided?1ull:0ull);
    return h?h:1ull;
}

std::array<std::uint64_t,CascadeShadowMaximumCascades> ComputeCascadeStaticShadowHashes(
    std::span<const Mat4> cascadeClipFromWorld,const Vec3& directionToLight,std::uint32_t resolution,
    std::span<const StaticShadowCasterInput> staticCasters)
{
    std::array<std::uint64_t,CascadeShadowMaximumCascades> result{};
    if(cascadeClipFromWorld.empty()||!resolution||!IsFinite(directionToLight))return result;
    const Vec3 dir=Normalize(directionToLight);
    if(LengthSquared(dir)<0.99f)return result;

    struct SortedCaster
    {
        std::size_t index=0;
        std::uint64_t transformHash=0;
    };
    std::vector<SortedCaster> sorted;
    sorted.reserve(staticCasters.size());
    for(std::size_t i=0;i<staticCasters.size();++i)
        sorted.push_back({i,HashShadowTransform(staticCasters[i].world)});
    std::sort(sorted.begin(),sorted.end(),[&](const SortedCaster& a,const SortedCaster& b){
        const auto& ca=staticCasters[a.index];
        const auto& cb=staticCasters[b.index];
        if(ca.entityId!=cb.entityId)return ca.entityId<cb.entityId;
        if(ca.geometryKey!=cb.geometryKey)return ca.geometryKey<cb.geometryKey;
        if(a.transformHash!=b.transformHash)return a.transformHash<b.transformHash;
        return ca.maskMaterialHash<cb.maskMaterialHash;
    });

    const std::size_t count=(std::min)(static_cast<std::size_t>(CascadeShadowMaximumCascades),cascadeClipFromWorld.size());
    for(std::size_t slice=0;slice<count;++slice)
    {
        const auto& matrix=cascadeClipFromWorld[slice];
        if(!IsFinite(matrix))continue;
        std::uint64_t h=MixHash64(0x3C6EF372FE94F82Bull,slice);
        h=MixHash64(h,resolution);
        h=MixHash64(h,CanonicalFloatBits(dir.x));
        h=MixHash64(h,CanonicalFloatBits(dir.y));
        h=MixHash64(h,CanonicalFloatBits(dir.z));
        for(const float v:matrix.values)h=MixHash64(h,CanonicalFloatBits(v));

        std::uint64_t casterCount=0;
        for(const auto& item:sorted)
        {
            const auto& caster=staticCasters[item.index];
            if(caster.hasWorldBounds&&!OverlapsOrthographicCascade(matrix,caster.worldBounds))continue;
            h=MixHash64(h,caster.entityId);
            h=MixHash64(h,caster.geometryKey);
            h=MixHash64(h,item.transformHash);
            h=MixHash64(h,caster.maskMaterialHash);
            ++casterCount;
        }
        h=MixHash64(h,casterCount);
        result[slice]=h?h:1ull;
    }
    return result;
}
}
