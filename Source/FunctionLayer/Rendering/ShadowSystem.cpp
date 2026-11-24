#include "ShadowSystem.h"
#include <cmath>
#include "CascadeShadow.h"
#include <algorithm>
#include "ResourceLayer/Assets/AssetIO.h"

namespace flare
{
bool PrepareDirectionalShadow(const DirectionalShadowView& view,const rhi::ForwardLighting& lighting,
    rhi::ShadowSettings& output,std::vector<AssetDiagnostic>& diagnostics)
{
    const auto invalid=[&]{asset_io::Diagnostic(diagnostics,"Rendering.ShadowInvalid");return false;};
    if(!IsFinite(view.center)||std::abs(view.center.x)>1e6||std::abs(view.center.y)>1e6||std::abs(view.center.z)>1e6
        ||!IsFinite(view.halfExtent)||view.halfExtent<0.1f||view.halfExtent>1000
        ||!IsFinite(view.lightDistance)||view.lightDistance<2*view.halfExtent||view.lightDistance>10000
        ||(view.resolution!=256&&view.resolution!=512&&view.resolution!=1024&&view.resolution!=1536&&view.resolution!=2048&&view.resolution!=4096)
        ||!IsFinite(view.depthBias)||view.depthBias<0||view.depthBias>0.02f||!IsFinite(view.slopeBias)||view.slopeBias<0||view.slopeBias>0.05f
        ||!IsFinite(view.normalBias)||view.normalBias<0||view.normalBias>0.25f||!IsFinite(lighting.directionToLight))return invalid();
    const auto z=Normalize(lighting.directionToLight);if(LengthSquared(z)<0.99f)return invalid();
    const Vec3 up=std::abs(z.y)>0.95f?Vec3{0,0,1}:Vec3{0,1,0};
    const auto x=Normalize(Cross(up,z)),y=Cross(z,x);
    const double texel=2.0*view.halfExtent/view.resolution;
    const double coordinateX=double(view.center.x)*x.x+double(view.center.y)*x.y+double(view.center.z)*x.z;
    const double coordinateY=double(view.center.x)*y.x+double(view.center.y)*y.y+double(view.center.z)*y.z;
    const auto center=view.center+x*float(std::round(coordinateX/texel)*texel-coordinateX)+y*float(std::round(coordinateY/texel)*texel-coordinateY);
    const auto position=center+z*view.lightDistance;Mat4 world;
    for(size_t row=0;row<3;++row)
    {
        const float a[]{x.x,x.y,x.z},b[]{y.x,y.y,y.z},c[]{z.x,z.y,z.z},p[]{position.x,position.y,position.z};
        world(row,0)=a[row];world(row,1)=b[row];world(row,2)=c[row];world(row,3)=p[row];
    }
    const auto inverse=InverseRigid(world),projection=OrthographicRH(view.halfExtent*2,view.halfExtent*2,0.1f,view.lightDistance+view.halfExtent*2);
    if(!inverse||!projection)return invalid();
    rhi::ShadowSettings candidate;candidate.clipFromWorld=*projection * *inverse;candidate.resolution=view.resolution;
    candidate.depthBias=view.depthBias;candidate.slopeBias=view.slopeBias;candidate.normalBias=view.normalBias;
    candidate.enabled=view.enabled&&lighting.directionalEnabled;candidate.pcf=view.pcf;candidate.preview=view.preview;
    if(!IsFinite(candidate.clipFromWorld))return invalid();output=candidate;return true;
}
bool PrepareDirectionalCascadeShadow(const DirectionalShadowView& view,const rhi::ForwardLighting& lighting,
    const CameraSnapshot& camera,rhi::ShadowSettings& output,std::vector<AssetDiagnostic>& diagnostics)
{
    rhi::ShadowSettings candidate;
    if(!PrepareDirectionalShadow(view,lighting,candidate,diagnostics))return false;
    CascadeShadowSettings fit;
    if(!IsFinite(view.cascadeDistance)||view.cascadeDistance<=0)
    {asset_io::Diagnostic(diagnostics,"Rendering.ShadowInvalid");return false;}
    const auto distance=std::min(view.cascadeDistance,camera.lens.farPlane);
    const auto casterPadding=std::clamp(distance*1.5f,100.f,2000.f);
    if(!PrepareCascadeShadows(camera.view,camera.projection,camera.lens.nearPlane,camera.lens.farPlane,
        distance,lighting.directionToLight,view.cascadeCount,view.cascadeResolution,fit,casterPadding,
        view.filter.pcss&&view.pcf&&view.filter.angularRadius>0?std::max(3.f,std::ceil(view.filter.maximumRadiusTexels)+1.f):3.f,
        view.filter.pcss&&view.pcf&&view.filter.angularRadius>0?view.cascadeNormalBias:0.f,
        view.cascadeSplitLambda))
    {asset_io::Diagnostic(diagnostics,"Rendering.CsmUnavailable");return false;}
    candidate.resolution=fit.resolution;candidate.cascadeCount=fit.cascadeCount;
    candidate.cascadeClipFromWorld=fit.clipFromWorld;candidate.cascadeSplits=fit.splitDistances;
    candidate.cascadeViewDepth={-camera.view(2,0),-camera.view(2,1),-camera.view(2,2),-camera.view(2,3)};
    candidate.cascadeNear=camera.lens.nearPlane;candidate.cascadeBlend=fit.blendFraction;candidate.cascadeDebug=view.cascadeDebug;
    candidate.depthBias=view.cascadeDepthBias;candidate.slopeBias=view.cascadeSlopeBias;candidate.normalBias=view.cascadeNormalBias;
    const auto& filter=view.filter;
    if(!IsFinite(filter.angularRadius)||filter.angularRadius<0||filter.angularRadius>.2f
        ||!IsFinite(filter.blockerSearchDistance)||filter.blockerSearchDistance<.01f||filter.blockerSearchDistance>100
        ||!IsFinite(filter.maximumRadiusTexels)||filter.maximumRadiusTexels<1||filter.maximumRadiusTexels>32
        ||static_cast<std::uint32_t>(filter.debug)>2)
    {asset_io::Diagnostic(diagnostics,"Rendering.ShadowInvalid");return false;}
    candidate.filter=filter;
    candidate.clipFromWorld=fit.clipFromWorld[0];output=candidate;return true;
}
}
