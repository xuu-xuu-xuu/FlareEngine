#include "SheenEnergyBake.h"
#include "AssetIO.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace flare
{
namespace
{
constexpr double pi=3.14159265358979323846;
bool Valid(const SheenEnergyLut& v)
{
    if(v.size<16||v.size>128||v.samples<256||v.samples>65536||v.values.size()!=std::size_t(v.size)*v.size)return false;
    for(auto e:v.values)if(!IsFinite(e.x)||!IsFinite(e.y)||e.x<0||e.x>1||e.y<1)return false;
    return true;
}
void U32(std::vector<std::uint8_t>& b,std::uint32_t v){for(unsigned i=0;i<4;++i)b.push_back(static_cast<std::uint8_t>(v>>(i*8)));}
std::uint32_t Read(const std::uint8_t* b){return b[0]|(std::uint32_t(b[1])<<8)|(std::uint32_t(b[2])<<16)|(std::uint32_t(b[3])<<24);}
}
bool BakeSheenEnergy(std::uint32_t size,std::uint32_t samples,SheenEnergyLut& output)
{
    if(size<16||size>128||samples<256||samples>65536||std::uint64_t(size)*size*samples>268435456)return false;
    SheenEnergyLut result{size,samples,{}};result.values.resize(std::size_t(size)*size);
    // Conty/Kulla Charlie NDF + Ashikhmin visibility；确定性等面积半球积分，非图像调亮拟合。
    // https://blog.selfshadow.com/publications/s2017-shading-course/imageworks/s2017_pbs_imageworks_sheen.pdf
    for(std::uint32_t x=0;x<size;++x)
    {
        const double rough=std::max(.07,.045+.955*double(x)/(size-1)),inverse=1/(rough*rough);
        double maximum=1;
        for(std::uint32_t y=0;y<size;++y)
        {
            const double angle=pi*.5*double(y)/(size-1),nv=.0001+.9999*std::cos(angle)*std::cos(angle),vx=std::sqrt(1-nv*nv);
            double energy=0;
            for(std::uint32_t i=0;i<samples;++i)
            {
                const double nl=(double(i)+.5)/samples,phi=2*pi*std::fmod((double(i)+.5)*.6180339887498948482,1.0);
                const double radius=std::sqrt(1-nl*nl),lx=radius*std::cos(phi),ly=radius*std::sin(phi);
                const double hx=vx+lx,hy=ly,hz=nv+nl,squared=hx*hx+hy*hy+hz*hz;
                if(squared<1e-24)continue;
                const double nh2=hz*hz/squared,D=(2+inverse)*std::pow(std::max(1-nh2,1e-12),.5*inverse)/(2*pi);
                const double visibility=1/std::max(4*(nl+nv-nl*nv),1e-12);
                energy+=D*visibility*nl*(2*pi);
            }
            const double e=energy/samples;maximum=std::max(maximum,e);
            result.values[std::size_t(y)*size+x]={static_cast<float>(e),1};
        }
        // 整列统一缩放，CPU参考与GPU BRDF用同一因子；保证端点的方向能量不超过1。
        maximum*=1.001;
        for(std::uint32_t y=0;y<size;++y)
        {auto& e=result.values[std::size_t(y)*size+x];e.x=static_cast<float>(e.x/maximum);e.y=static_cast<float>(maximum);}
    }
    if(!Valid(result))return false;output=std::move(result);return true;
}
bool SaveSheenEnergy(const std::filesystem::path& path,const SheenEnergyLut& value)
{
    if(!Valid(value))return false;std::vector<std::uint8_t> bytes{'S','E','S','H','E','E','N','2'};
    U32(bytes,value.size);U32(bytes,value.samples);
    for(auto v:value.values){U32(bytes,std::bit_cast<std::uint32_t>(v.x));U32(bytes,std::bit_cast<std::uint32_t>(v.y));}
    return asset_io::WriteAtomic(path,bytes);
}
bool LoadSheenEnergy(const std::filesystem::path& path,SheenEnergyLut& output)
{
    std::vector<std::uint8_t> b;if(!asset_io::Read(path,b)||b.size()<16||std::memcmp(b.data(),"SESHEEN2",8))return false;
    SheenEnergyLut value{Read(b.data()+8),Read(b.data()+12),{}};
    if(value.size<16||value.size>128||b.size()!=16+std::size_t(value.size)*value.size*8)return false;
    value.values.resize(std::size_t(value.size)*value.size);
    for(std::size_t i=0;i<value.values.size();++i)value.values[i]={std::bit_cast<float>(Read(b.data()+16+i*8)),std::bit_cast<float>(Read(b.data()+20+i*8))};
    if(!Valid(value))return false;output=std::move(value);return true;
}
}
