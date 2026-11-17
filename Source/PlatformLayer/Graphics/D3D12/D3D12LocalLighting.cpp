#include "D3D12LocalLighting.h"

using namespace se;
bool ValidD3D12LocalLight(const rhi::LocalLight& light)
{
    const auto p=light.position,r=light.radiance;
    if(!IsFinite(p)||std::max({std::abs(p.x),std::abs(p.y),std::abs(p.z)})>1e6f
        ||!IsFinite(r)||std::min({r.x,r.y,r.z})<0||std::max({r.x,r.y,r.z})>10000
        ||!IsFinite(light.range)||light.range<.05f||light.range>10000)return false;
    return !light.spot||(IsFinite(light.direction)&&std::abs(LengthSquared(light.direction)-1)<.001f
        &&IsFinite(light.innerCos)&&IsFinite(light.outerCos)&&light.innerCos>=light.outerCos
        &&light.innerCos<=1&&light.outerCos>0&&light.outerCos<1);
}

namespace
{
Mat4 LocalView(Vec3 position,Vec3 forward)
{
    const Vec3 z=-forward;
    const Vec3 up=std::abs(z.y)>.95f?Vec3{0,0,1}:Vec3{0,1,0};
    const Vec3 x=Normalize(Cross(up,z)),y=Cross(z,x);
    const std::array<Vec3,3> axes{x,y,z};Mat4 view;
    for(std::size_t row=0;row<axes.size();++row)
    {
        const auto axis=axes[row];
        view(row,0)=axis.x;view(row,1)=axis.y;view(row,2)=axis.z;view(row,3)=-Dot(axis,position);
    }
    return view;
}
}
bool PrepareD3D12LocalLightConstants(std::span<const rhi::LocalLight> lights,D3D12LocalLightConstants& output)
{
    if(lights.size()>rhi::MaximumLocalLights)return false;
    constexpr std::array<Vec3,6> faces{{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}}};
    D3D12LocalLightConstants candidate{};
    unsigned slice=0;
    for(std::size_t i=0;i<lights.size();++i)
    {
        const auto& light=lights[i];if(!ValidD3D12LocalLight(light))return false;
        const auto pos=light.position,rad=light.radiance,dir=light.spot?Normalize(light.direction):Vec3{};
        candidate.positionRange[i]={pos.x,pos.y,pos.z,light.range};
        candidate.radianceSpot[i]={rad.x,rad.y,rad.z,light.spot?1.f:0.f};
        candidate.directionOuter[i]={dir.x,dir.y,dir.z,light.spot?light.outerCos:0};
        candidate.coneShadow[i]={light.spot?light.innerCos:0,light.castsShadow?float(slice):-1.f,0,0};
        if(!light.castsShadow)continue;
        const float fov=light.spot?2.f*std::acos(light.outerCos):Pi*.5f;
        const auto projection=PerspectiveRH(fov,1,std::min(.05f,light.range*.02f),light.range);
        if(!projection)return false;
        candidate.coneShadow[i].z=(*projection)(2,3);
        for(unsigned face=0;face<(light.spot?1u:6u);++face)
            candidate.clipFromWorld[slice++]=*projection*LocalView(pos,light.spot?dir:faces[face]);
    }
    candidate.info={float(lights.size()),float(D3D12LocalShadowResolution),float(slice),0};
    output=candidate;return true;
}
