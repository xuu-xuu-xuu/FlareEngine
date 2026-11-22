#include "SphericalHarmonics.h"

namespace flare
{
bool IsFinite(const Sh9Rgb& value) noexcept
{for(const auto& coefficient:value.coefficients)if(!IsFinite(coefficient))return false;return true;}
bool RealSh9Basis(Vec3 direction,std::array<float,9>& output) noexcept
{
    const auto length=LengthSquared(direction);
    if(!IsFinite(direction)||!IsFinite(length)||length<1e-12f)return false;
    const auto d=direction/std::sqrt(length);
    const auto& c=RealSh9Scale;
    output={float(c[0]),float(c[1])*d.y,float(c[2])*d.z,float(c[3])*d.x,
        float(c[4])*d.x*d.y,float(c[5])*d.y*d.z,
        float(c[6])*(3*d.z*d.z-1),float(c[7])*d.x*d.z,float(c[8])*(d.x*d.x-d.y*d.y)};
    return true;
}
bool EvaluateSh9(const Sh9Rgb& coefficients,Vec3 direction,Vec3& output) noexcept
{
    std::array<float,9> basis;
    if(!IsFinite(coefficients)||!RealSh9Basis(direction,basis))return false;
    Vec3 result{};
    for(std::size_t i=0;i<9;++i)result+=coefficients.coefficients[i]*basis[i];
    if(!IsFinite(result))return false;
    output=result;return true;
}
bool ConvolveDiffuseSh9(const Sh9Rgb& radiance,Sh9Rgb& output) noexcept
{
    if(!IsFinite(radiance))return false;
    auto diffuse=radiance;
    for(std::size_t i=1;i<9;++i)diffuse.coefficients[i]*=i<4?2.f/3.f:1.f/4.f;
    output=diffuse;return true;
}
}
