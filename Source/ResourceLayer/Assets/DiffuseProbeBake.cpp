#include "DiffuseProbeBake.h"
#include "Environment.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace flare
{
namespace
{
constexpr double pi=3.1415926535897932384626433832795;
struct Accumulator
{
    std::array<std::array<double,3>,9> values{};
    void Add(Vec3 direction,Vec3 radiance,double weight)
    {
        std::array<float,9> basis{};RealSh9Basis(direction,basis);
        std::array<double,9> integrated{};
        for(std::size_t i=0;i<9;++i)integrated[i]=basis[i]*weight;
        AddIntegrated(integrated,radiance);
    }
    void AddIntegrated(const std::array<double,9>& integrated,Vec3 radiance)
    {
        for(std::size_t i=0;i<9;++i)
        {values[i][0]+=radiance.x*integrated[i];values[i][1]+=radiance.y*integrated[i];values[i][2]+=radiance.z*integrated[i];}
    }
    bool Finish(Sh9Rgb& output) const
    {
        Sh9Rgb result;
        for(std::size_t i=0;i<9;++i)result.coefficients[i]={static_cast<float>(values[i][0]),static_cast<float>(values[i][1]),static_cast<float>(values[i][2])};
        if(!IsFinite(result))return false;
        output=result;return true;
    }
};
bool Nonnegative(Vec3 v){return IsFinite(v)&&v.x>=0&&v.y>=0&&v.z>=0;}
bool ImageValid(const FloatImage& image)
{
    if(!image.width||!image.height||image.width>16384||image.height>16384
        ||std::uint64_t(image.width)*image.height>16*1024*1024||image.pixels.size()!=std::size_t(image.width)*image.height)return false;
    for(const auto pixel:image.pixels)if(!Nonnegative(pixel))return false;
    return true;
}
bool SettingsValid(const DiffuseProbeBakeSettings& s)
{return s.samples>=64&&s.samples<=131072&&IsFinite(s.rayBias)&&s.rayBias>0&&IsFinite(s.maxDistance)&&s.maxDistance>s.rayBias*4;}
bool SceneValid(const DiffuseProbeScene& scene)
{
    if(scene.triangles.size()>4096||scene.lights.size()>256)return false;
    for(const auto& t:scene.triangles)
    {
        if(!IsFinite(t.a)||!IsFinite(t.b)||!IsFinite(t.c)||!Nonnegative(t.baseColor)
            ||t.baseColor.x>1||t.baseColor.y>1||t.baseColor.z>1)return false;
        const auto cross=Cross(t.b-t.a,t.c-t.a);const auto area=LengthSquared(cross);
        if(!IsFinite(cross)||!IsFinite(area)||area<1e-16f)return false;
    }
    for(const auto& light:scene.lights)
        if(!IsFinite(light.position)||!Nonnegative(light.radiance)||!IsFinite(light.range)||light.range<=0)return false;
    return !scene.environment||ImageValid(*scene.environment);
}
bool RayTriangle(Vec3 origin,Vec3 direction,const DiffuseBakeTriangle& triangle,float minimum,float maximum,float& distance)
{
    const auto e1=triangle.b-triangle.a,e2=triangle.c-triangle.a,h=Cross(direction,e2);
    const double determinant=Dot(e1,h);
    if(std::abs(determinant)<1e-10)return false;
    const auto s=origin-triangle.a,q=Cross(s,e1);
    const double u=Dot(s,h)/determinant,v=Dot(direction,q)/determinant,t=Dot(e2,q)/determinant;
    if(u<0||v<0||u+v>1||t<minimum||t>maximum)return false;
    distance=static_cast<float>(t);return true;
}
bool OnSurface(Vec3 p,const DiffuseBakeTriangle& t,float bias)
{
    const auto normal=Normalize(Cross(t.b-t.a,t.c-t.a));const auto signedDistance=Dot(p-t.a,normal);
    if(std::abs(signedDistance)>bias*2)return false;
    const auto projected=p-normal*signedDistance;
    const auto a=t.b-t.a,b=t.c-t.a,c=projected-t.a;
    const double aa=Dot(a,a),ab=Dot(a,b),bb=Dot(b,b),ca=Dot(c,a),cb=Dot(c,b),denominator=aa*bb-ab*ab;
    if(denominator<=0)return true;
    const double u=(bb*ca-ab*cb)/denominator,v=(aa*cb-ab*ca)/denominator;
    return u>=-1e-5&&v>=-1e-5&&u+v<=1.00001;
}
bool PositionValid(const DiffuseProbeScene& scene,Vec3 position,float bias)
{if(!IsFinite(position))return false;for(const auto& t:scene.triangles)if(OnSurface(position,t,bias))return false;return true;}
Vec3 Incoming(const DiffuseProbeScene& scene,Vec3 origin,Vec3 direction,const DiffuseProbeBakeSettings& settings)
{
    float nearest=settings.maxDistance;std::size_t hit=scene.triangles.size();
    for(std::size_t i=0;i<scene.triangles.size();++i)
    {
        float distance;
        if(RayTriangle(origin,direction,scene.triangles[i],settings.rayBias,nearest,distance))
        {nearest=distance;hit=i;}
    }
    if(hit==scene.triangles.size())return scene.environment?SampleEquirectangular(*scene.environment,direction):Vec3{};
    const auto& triangle=scene.triangles[hit];auto normal=Normalize(Cross(triangle.b-triangle.a,triangle.c-triangle.a));
    if(Dot(normal,-direction)<=0)
    {
        if(!triangle.doubleSided)return {}; // 背面仍挡住射线，不能透墙继续取环境。
        normal=-normal;
    }
    const auto position=origin+direction*nearest;Vec3 irradiance{};
    for(const auto& light:scene.lights)
    {
        const auto delta=light.position-position;const auto squared=LengthSquared(delta);
        if(!IsFinite(squared)||squared<=1e-12f||squared>=light.range*light.range)continue;
        const float distance=std::sqrt(squared);const auto l=delta/distance;
        const float cosine=std::max(0.f,Dot(normal,l));if(cosine<=0)continue;
        bool blocked=false;const auto shadowOrigin=position+normal*settings.rayBias;
        const auto shadowDelta=light.position-shadowOrigin;const float shadowDistance=Length(shadowDelta);
        for(std::size_t i=0;i<scene.triangles.size();++i)
        {
            if(i==hit)continue;
            float intersection;
            if(RayTriangle(shadowOrigin,shadowDelta/shadowDistance,scene.triangles[i],settings.rayBias,shadowDistance-settings.rayBias,intersection))
            {blocked=true;break;}
        }
        if(blocked)continue;
        // 与现点灯一致的有限支持 inverse-square 窗口；radiance 是灯的线性强度。
        const float ratio=squared/(light.range*light.range),window=std::max(0.f,1-ratio*ratio);
        irradiance+=light.radiance*(cosine*window*window/std::max(squared,.0025f));
    }
    return Hadamard(triangle.baseColor,irradiance)/static_cast<float>(pi);
}
bool Bake(const DiffuseProbeScene& scene,Vec3 position,const DiffuseProbeBakeSettings& settings,Sh9Rgb& diffuse)
{
    Accumulator projection;
    for(std::uint32_t i=0;i<settings.samples;++i)
    {
        const double y=1-2*(double(i)+.5)/settings.samples,r=std::sqrt(std::max(0.0,1-y*y)),phi=2.39996322972865332*i;
        const Vec3 direction{float(r*std::cos(phi)),float(y),float(r*std::sin(phi))};
        const auto incoming=Incoming(scene,position,direction,settings);
        if(!Nonnegative(incoming))return false;
        projection.Add(direction,incoming,4*pi/settings.samples);
    }
    Sh9Rgb radiance;
    return projection.Finish(radiance)&&ConvolveDiffuseSh9(radiance,diffuse);
}
bool GridShape(Vec3 origin,Vec3 spacing,std::array<std::uint32_t,3> dimensions,std::size_t& probes,std::size_t& cells)
{
    if(!IsFinite(origin)||!IsFinite(spacing)||spacing.x<=0||spacing.y<=0||spacing.z<=0)return false;
    probes=1;cells=1;
    for(const auto count:dimensions){if(count<2||count>128)return false;probes*=count;cells*=count-1;}
    if(probes>65536)return false;
    const auto maximum=origin+Hadamard(spacing,{float(dimensions[0]-1),float(dimensions[1]-1),float(dimensions[2]-1)});
    return IsFinite(maximum)&&maximum.x>origin.x&&maximum.y>origin.y&&maximum.z>origin.z;
}
bool WorkValid(const DiffuseProbeScene& scene,const DiffuseProbeBakeSettings& settings,std::size_t probes)
{
    const auto rays=std::uint64_t(probes)*settings.samples;
    const auto worst=std::uint64_t(std::max<std::size_t>(1,scene.triangles.size()))*(1+scene.lights.size());
    return rays<=32'000'000&&rays*worst<=1'000'000'000;
}
bool CellBlocked(Vec3 minimum,Vec3 maximum,const DiffuseProbeScene& scene)
{
    for(const auto& t:scene.triangles)
    {
        const Vec3 lo{std::min({t.a.x,t.b.x,t.c.x}),std::min({t.a.y,t.b.y,t.c.y}),std::min({t.a.z,t.b.z,t.c.z})};
        const Vec3 hi{std::max({t.a.x,t.b.x,t.c.x}),std::max({t.a.y,t.b.y,t.c.y}),std::max({t.a.z,t.b.z,t.c.z})};
        if(lo.x<=maximum.x&&hi.x>=minimum.x&&lo.y<=maximum.y&&hi.y>=minimum.y&&lo.z<=maximum.z&&hi.z>=minimum.z)return true;
    }
    return false;
}
}
double EquirectangularTexelSolidAngle(std::uint32_t width,std::uint32_t height,std::uint32_t row) noexcept
{
    if(!width||!height||row>=height)return 0;
    return (2*pi/width)*(std::cos(pi*row/height)-std::cos(pi*(double(row)+1)/height));
}
bool ProjectEnvironmentSh9(const FloatImage& image,Sh9Rgb& radiance)
{
    if(!ImageValid(image))return false;
    Accumulator projection;
    for(std::uint32_t y=0;y<image.height;++y)
    {
        const double t0=pi*y/image.height,t1=pi*(double(y)+1)/image.height;
        const double c0=std::cos(t0),c1=std::cos(t1),s0=std::sin(t0),s1=std::sin(t1);
        const double a=c0-c1,b=(t1-t0)/2-(std::sin(2*t1)-std::sin(2*t0))/4;
        const double c=(s1*s1-s0*s0)/2,d=(s1*s1*s1-s0*s0*s0)/3;
        const double f=(c0*c0*c0-c1*c1*c1)/3,e=a-f;
        for(std::uint32_t x=0;x<image.width;++x)
        {
            const double p0=2*pi*(double(x)/image.width-.5),p1=2*pi*((double(x)+1)/image.width-.5),dp=p1-p0;
            const double cosine=std::sin(p1)-std::sin(p0),sine=std::cos(p0)-std::cos(p1);
            const double c2=dp/2+(std::sin(2*p1)-std::sin(2*p0))/4,s2=dp-c2;
            const double cs=(std::sin(p1)*std::sin(p1)-std::sin(p0)*std::sin(p0))/2;
            const auto& k=RealSh9Scale;
            const std::array<double,9> integrated{k[0]*a*dp,k[1]*c*dp,k[2]*b*sine,k[3]*b*cosine,
                k[4]*d*cosine,k[5]*d*sine,k[6]*(3*e*s2-a*dp),k[7]*e*cs,k[8]*(e*c2-f*dp)};
            projection.AddIntegrated(integrated,image.pixels[std::size_t(y)*image.width+x]);
        }
    }
    return projection.Finish(radiance);
}
bool BakeDiffuseProbe(const DiffuseProbeScene& scene,Vec3 position,const DiffuseProbeBakeSettings& settings,Sh9Rgb& diffuse)
{
    if(!SettingsValid(settings)||!SceneValid(scene)||!PositionValid(scene,position,settings.rayBias)||!WorkValid(scene,settings,1))return false;
    Sh9Rgb result;if(!Bake(scene,position,settings,result))return false;diffuse=result;return true;
}
bool BakeDiffuseProbeGrid(const DiffuseProbeScene& scene,Vec3 origin,Vec3 spacing,std::array<std::uint32_t,3> dimensions,
    const DiffuseProbeBakeSettings& settings,DiffuseProbeGrid& output)
{
    std::size_t probes,cells;
    if(!SettingsValid(settings)||!SceneValid(scene)||!GridShape(origin,spacing,dimensions,probes,cells)||!WorkValid(scene,settings,probes))return false;
    DiffuseProbeGrid result;result.origin=origin;result.spacing=spacing;result.dimensions=dimensions;
    result.diffuse.resize(probes);result.probeValid.resize(probes);result.cellValid.resize(cells);
    const auto [nx,ny,nz]=dimensions;
    for(std::uint32_t z=0;z<nz;++z)for(std::uint32_t y=0;y<ny;++y)for(std::uint32_t x=0;x<nx;++x)
    {
        const auto i=x+y*nx+z*nx*ny;const auto p=origin+Hadamard(spacing,{float(x),float(y),float(z)});
        if(!PositionValid(scene,p,settings.rayBias))continue;
        if(!Bake(scene,p,settings,result.diffuse[i]))return false;
        result.probeValid[i]=1;
    }
    for(std::uint32_t z=0;z<nz-1;++z)for(std::uint32_t y=0;y<ny-1;++y)for(std::uint32_t x=0;x<nx-1;++x)
    {
        const auto minimum=origin+Hadamard(spacing,{float(x),float(y),float(z)});
        result.cellValid[x+y*(nx-1)+z*(nx-1)*(ny-1)]=CellBlocked(minimum,minimum+spacing,scene)?0:1;
    }
    output=std::move(result);return true;
}
bool SampleDiffuseProbeGrid(const DiffuseProbeGrid& grid,const Sh9Rgb& globalDiffuse,Vec3 position,Vec3 normal,DiffuseProbeSample& output)
{
    std::size_t probes,cells;Vec3 global;
    if(!GridShape(grid.origin,grid.spacing,grid.dimensions,probes,cells)||grid.diffuse.size()!=probes
        ||grid.probeValid.size()!=probes||grid.cellValid.size()!=cells||!IsFinite(position)||!EvaluateSh9(globalDiffuse,normal,global))return false;
    for(std::size_t i=0;i<probes;++i)if(grid.probeValid[i]>1||!IsFinite(grid.diffuse[i]))return false;
    for(const auto valid:grid.cellValid)if(valid>1)return false;
    DiffuseProbeSample result;result.diffuse=global;
    const auto p=position-grid.origin;const Vec3 q{p.x/grid.spacing.x,p.y/grid.spacing.y,p.z/grid.spacing.z};
    const auto [nx,ny,nz]=grid.dimensions;
    if(q.x<0||q.y<0||q.z<0||q.x>nx-1||q.y>ny-1||q.z>nz-1){output=result;return true;}
    const auto x=std::min(static_cast<std::uint32_t>(q.x),nx-2),y=std::min(static_cast<std::uint32_t>(q.y),ny-2),z=std::min(static_cast<std::uint32_t>(q.z),nz-2);
    if(!grid.cellValid[x+y*(nx-1)+z*(nx-1)*(ny-1)]){output=result;return true;}
    const float fx=q.x-x,fy=q.y-y,fz=q.z-z;Sh9Rgb interpolated;float weightSum=0;
    for(std::uint32_t dz=0;dz<2;++dz)for(std::uint32_t dy=0;dy<2;++dy)for(std::uint32_t dx=0;dx<2;++dx)
    {
        const auto corner=dx+2*dy+4*dz,i=(x+dx)+(y+dy)*nx+(z+dz)*nx*ny;
        const float weight=(dx?fx:1-fx)*(dy?fy:1-fy)*(dz?fz:1-fz);
        result.corners[corner]=i;result.weights[corner]=weight;weightSum+=weight;
        if(!grid.probeValid[i]){output=DiffuseProbeSample{global};return true;}
        for(std::size_t j=0;j<9;++j)interpolated.coefficients[j]+=grid.diffuse[i].coefficients[j]*weight;
    }
    if(!IsFinite(weightSum)||weightSum<1e-6f||std::abs(weightSum-1)>1e-5f||!EvaluateSh9(interpolated,normal,result.diffuse))return false;
    result.usedLocal=true;output=result;return true;
}
}
