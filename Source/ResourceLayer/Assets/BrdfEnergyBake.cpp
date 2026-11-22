#include "BrdfEnergyBake.h"
#include "AssetIO.h"
#include "CoreLayer/Hash.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <bit>
#include <thread>

namespace flare
{
namespace
{
using Json=nlohmann::json;
constexpr double pi=3.1415926535897932384626433832795;
constexpr std::uint8_t magic[]{'S','E','B','R','D','F','0','1'};
constexpr std::size_t MaximumBytes=16*1024*1024,MaximumHeader=65536;
constexpr const char* mapping="sqrt-Nv-affine/perceptual-roughness-affine/endpoints/half-texel";
bool Invalid(std::vector<AssetDiagnostic>& diagnostics,const char* detail="")
{asset_io::Diagnostic(diagnostics,"BrdfEnergy.InvalidAsset",detail);return false;}
bool Domain(float nv,float roughness)
{return IsFinite(nv)&&IsFinite(roughness)&&nv>=BrdfEnergyMinimumNv&&nv<=1&&roughness>=BrdfEnergyMinimumRoughness&&roughness<=1;}
double RootMin(){return std::sqrt(double(BrdfEnergyMinimumNv));}
double NvAt(double coordinate){const auto t=RootMin()+(1-RootMin())*coordinate;return t*t;}
double RoughAt(double coordinate){return BrdfEnergyMinimumRoughness+(1-double(BrdfEnergyMinimumRoughness))*coordinate;}
struct Disk{double x,y,cap;};
double Radical(std::uint32_t n)
{
    n=(n<<16)|(n>>16);n=((n&0x55555555)<<1)|((n&0xaaaaaaaa)>>1);n=((n&0x33333333)<<2)|((n&0xcccccccc)>>2);
    n=((n&0x0f0f0f0f)<<4)|((n&0xf0f0f0f0)>>4);n=((n&0x00ff00ff)<<8)|((n&0xff00ff00)>>8);
    return n*2.3283064365386962890625e-10;
}
std::vector<Disk> Disks(std::uint32_t count)
{
    std::vector<Disk> points;points.reserve(count);
    for(std::uint32_t i=0;i<count;++i)
    {const auto r=std::sqrt((double(i)+.5)/count),phi=2*pi*Radical(i),x=r*std::cos(phi);points.push_back({x,r*std::sin(phi),std::sqrt(std::max(0.0,1-x*x))});}
    return points;
}
Vec2 Integrate(double nv,double roughness,std::span<const Disk> points)
{
    // Heitz 2018 的可见半球投影；grazing 下 G2/G1 权重也有界于 1。
    const auto alpha=roughness*roughness,a2=alpha*alpha,vx=std::sqrt(std::max(0.0,1-nv*nv));
    const auto length=std::sqrt(alpha*alpha*vx*vx+nv*nv),sx=alpha*vx/length,sz=nv/length,blend=.5*(1+sz);
    const auto lambda=[&](double mu){return .5*(std::sqrt(mu*mu+a2*(1-mu*mu))/mu-1);};
    const auto lambdaV=lambda(nv);double a=0,b=0;
    for(const auto sample:points)
    {
        const auto dy=(1-blend)*sample.cap+blend*sample.y,hz=std::sqrt(std::max(0.0,1-sample.x*sample.x-dy*dy));
        double x,y,z;
        if(vx>0){x=alpha*(-sz*dy+sx*hz);y=alpha*sample.x;z=std::max(0.0,sx*dy+sz*hz);}
        else{x=alpha*sample.x;y=alpha*dy;z=hz;}
        const auto inverseLength=1/std::sqrt(x*x+y*y+z*z);x*=inverseLength;z*=inverseLength;
        const auto vh=std::clamp(vx*x+nv*z,0.0,1.0),nl=2*vh*z-nv;if(nl<=0)continue;
        const auto weight=(1+lambdaV)/(1+lambdaV+lambda(nl));
        const auto grazing=1-vh,g2=grazing*grazing,fc=g2*g2*grazing;
        a+=(1-fc)*weight;b+=fc*weight;
    }
    return {float(a/points.size()),float(b/points.size())};
}
Vec2 AverageRow(std::span<const Vec2> row)
{
    double a=double(row.front().x)*BrdfEnergyMinimumNv*BrdfEnergyMinimumNv,b=double(row.front().y)*BrdfEnergyMinimumNv*BrdfEnergyMinimumNv;
    const auto integrate=[](double t0,double t1,double y0,double y1){const auto slope=(y1-y0)/(t1-t0),constant=y0-slope*t0;
        return constant*(std::pow(t1,4)-std::pow(t0,4))+.8*slope*(std::pow(t1,5)-std::pow(t0,5));};
    for(std::size_t i=0;i+1<row.size();++i)
    {
        const double t0=RootMin()+(1-RootMin())*double(i)/(row.size()-1),t1=RootMin()+(1-RootMin())*double(i+1)/(row.size()-1);
        a+=integrate(t0,t1,row[i].x,row[i+1].x);b+=integrate(t0,t1,row[i].y,row[i+1].y);
    }
    return {float(a),float(b)};
}
bool Energy(Vec2 ab){return IsFinite(ab.x)&&IsFinite(ab.y)&&ab.x>=0&&ab.y>=0&&ab.x+ab.y<=1.000001f;}
Vec2 Mix(Vec2 a,Vec2 b,double f){return {float(a.x*(1-f)+b.x*f),float(a.y*(1-f)+b.y*f)};}
void U32(std::vector<std::uint8_t>& bytes,std::uint32_t value)
{for(unsigned i=0;i<4;++i)bytes.push_back(static_cast<std::uint8_t>(value>>(8*i)));}
std::uint32_t U32(const std::uint8_t* b){return b[0]|(std::uint32_t(b[1])<<8)|(std::uint32_t(b[2])<<16)|(std::uint32_t(b[3])<<24);}
bool Uint(const Json& json,std::uint32_t& output)
{if(!json.is_number_unsigned()||json.get<std::uint64_t>()>UINT32_MAX)return false;output=json.get<std::uint32_t>();return true;}
Json SettingsJson(const BrdfEnergySettings& s){return {{"width",s.width},{"height",s.height},{"samples",s.samples}};}
}
bool ValidateBrdfEnergySettings(const BrdfEnergySettings& s)
{
    return s.width>=32&&s.width<=1024&&s.height>=16&&s.height<=512&&s.samples>=1024&&s.samples<=262144
        &&std::has_single_bit(s.samples)&&std::uint64_t(s.width)*s.height*s.samples<=8'000'000'000ull;
}
std::string BrdfEnergyBakeKey(const BrdfEnergySettings& settings)
{
    if(!ValidateBrdfEnergySettings(settings))return {};
    return Sha256(Json{{"algorithm",BrdfEnergyCookerVersion},{"settings",SettingsJson(settings)},{"mapping",mapping},
        {"minimum_nv",BrdfEnergyMinimumNv},{"minimum_roughness",BrdfEnergyMinimumRoughness},{"average","exact integral of sqrt-Nv-linear table; below-min-Nv clamped"}}.dump());
}
bool BrdfEnergyUv(const BrdfEnergySettings& s,float nv,float roughness,Vec2& output)
{
    if(!ValidateBrdfEnergySettings(s)||!Domain(nv,roughness))return false;
    const auto x=(std::sqrt(double(nv))-RootMin())/(1-RootMin()),y=(double(roughness)-BrdfEnergyMinimumRoughness)/(1-BrdfEnergyMinimumRoughness);
    output={float((x*(s.width-1)+.5)/s.width),float((y*(s.height-1)+.5)/s.height)};return true;
}
bool SampleBrdfEnergy(const BrdfEnergyLut& lut,float nv,float roughness,Vec2& directional,Vec2& average)
{
    const auto& s=lut.settings;
    if(!ValidateBrdfEnergySettings(s)||lut.directional.size()!=std::size_t(s.width)*s.height||lut.average.size()!=s.height||!Domain(nv,roughness))return false;
    const auto x=std::clamp((std::sqrt(double(nv))-RootMin())/(1-RootMin()),0.0,1.0)*(s.width-1);
    const auto y=std::clamp((double(roughness)-BrdfEnergyMinimumRoughness)/(1-BrdfEnergyMinimumRoughness),0.0,1.0)*(s.height-1);
    const auto ix=std::min(std::uint32_t(x),s.width-2),iy=std::min(std::uint32_t(y),s.height-2);
    const auto first=Mix(lut.directional[ix+iy*s.width],lut.directional[ix+1+iy*s.width],x-ix);
    const auto second=Mix(lut.directional[ix+(iy+1)*s.width],lut.directional[ix+1+(iy+1)*s.width],x-ix);
    const auto d=Mix(first,second,y-iy),a=Mix(lut.average[iy],lut.average[iy+1],y-iy);
    if(!Energy(d)||!Energy(a))return false;directional=d;average=a;return true;
}
bool EstimateBrdfEnergy(float nv,float roughness,std::uint32_t samples,Vec2& output)
{
    if(!Domain(nv,roughness)||samples<1024||samples>4194304||!std::has_single_bit(samples))return false;
    const auto result=Integrate(nv,roughness,Disks(samples));if(!Energy(result))return false;output=result;return true;
}
bool BakeBrdfEnergy(const BrdfEnergySettings& settings,BrdfEnergyLut& output,std::vector<AssetDiagnostic>& diagnostics,std::uint32_t threads)
{
    if(!ValidateBrdfEnergySettings(settings)||threads<1||threads>16){asset_io::Diagnostic(diagnostics,"BrdfEnergy.InvalidSettings");return false;}
    try
    {
        BrdfEnergyLut result;result.settings=settings;result.bakeKey=BrdfEnergyBakeKey(settings);
        result.directional.resize(std::size_t(settings.width)*settings.height);result.average.resize(settings.height);const auto points=Disks(settings.samples);
        std::atomic<std::uint32_t> nextRow{0};const auto worker=[&]{for(;;){const auto row=nextRow.fetch_add(1);if(row>=settings.height)return;
            const auto rough=RoughAt(double(row)/(settings.height-1));for(std::uint32_t column=0;column<settings.width;++column)
                result.directional[column+row*settings.width]=Integrate(NvAt(double(column)/(settings.width-1)),rough,points);}};
        std::vector<std::jthread> workers;for(std::uint32_t i=0;i<std::min(threads,settings.height);++i)workers.emplace_back(worker);workers.clear();
        for(std::uint32_t row=0;row<settings.height;++row)result.average[row]=AverageRow(std::span(result.directional).subspan(std::size_t(row)*settings.width,settings.width));
        if(!ValidateBrdfEnergyLut(result)){asset_io::Diagnostic(diagnostics,"BrdfEnergy.BakeFailed");return false;}
        output=std::move(result);return true;
    }
    catch(const std::exception&){asset_io::Diagnostic(diagnostics,"BrdfEnergy.BakeFailed");return false;}
}
bool ValidateBrdfEnergyLut(const BrdfEnergyLut& lut)
{
    const auto& s=lut.settings;if(!ValidateBrdfEnergySettings(s)||lut.bakeKey!=BrdfEnergyBakeKey(s)||lut.directional.size()!=std::size_t(s.width)*s.height||lut.average.size()!=s.height)return false;
    for(const auto e:lut.directional)if(!Energy(e))return false;
    for(std::uint32_t row=0;row<s.height;++row)
    {
        const auto expected=AverageRow(std::span(lut.directional).subspan(std::size_t(row)*s.width,s.width)),actual=lut.average[row];
        if(!Energy(actual)||std::abs(expected.x-actual.x)>2e-6f||std::abs(expected.y-actual.y)>2e-6f)return false;
    }
    return true;
}
bool EncodeBrdfEnergy(const BrdfEnergyLut& lut,std::vector<std::uint8_t>& output,std::vector<AssetDiagnostic>& diagnostics)
{
    if(!ValidateBrdfEnergyLut(lut))return Invalid(diagnostics);
    std::vector<std::uint8_t> payload;payload.reserve((lut.directional.size()+lut.average.size())*8);
    for(const auto& table:{std::span(lut.directional),std::span(lut.average)})for(const auto value:table)
    {U32(payload,std::bit_cast<std::uint32_t>(value.x));U32(payload,std::bit_cast<std::uint32_t>(value.y));}
    const auto header=Json{{"format","FlareEngine.BrdfEnergy"},{"schema",1},{"algorithm",BrdfEnergyCookerVersion},{"bake_key",lut.bakeKey},
        {"settings",SettingsJson(lut.settings)},{"mapping",mapping},{"minimum_nv",BrdfEnergyMinimumNv},{"minimum_roughness",BrdfEnergyMinimumRoughness},
        {"layout","directional AB float32-le row-major then average AB float32-le"},{"directional_count",lut.directional.size()},{"average_count",lut.average.size()},
        {"payload_bytes",payload.size()},{"payload_sha256",Sha256(payload)}}.dump();
    if(header.size()>MaximumHeader||header.size()+12+payload.size()>MaximumBytes)return Invalid(diagnostics,"size limit");
    std::vector<std::uint8_t> bytes(std::begin(magic),std::end(magic));U32(bytes,static_cast<std::uint32_t>(header.size()));bytes.insert(bytes.end(),header.begin(),header.end());bytes.insert(bytes.end(),payload.begin(),payload.end());output=std::move(bytes);return true;
}
bool DecodeBrdfEnergy(std::span<const std::uint8_t> bytes,BrdfEnergyLut& output,std::vector<AssetDiagnostic>& diagnostics)
{
    if(bytes.size()<12||bytes.size()>MaximumBytes||!std::equal(std::begin(magic),std::end(magic),bytes.begin()))return Invalid(diagnostics);
    const auto length=U32(bytes.data()+8);if(!length||length>MaximumHeader||std::uint64_t(length)+12>bytes.size())return Invalid(diagnostics,"header length");
    try
    {
        const auto header=Json::parse(bytes.begin()+12,bytes.begin()+12+length);
        if(header.at("format")!="FlareEngine.BrdfEnergy"||header.at("schema")!=1||header.at("algorithm")!=BrdfEnergyCookerVersion)
        {asset_io::Diagnostic(diagnostics,"BrdfEnergy.VersionUnsupported");return false;}
        BrdfEnergyLut result;const auto& settings=header.at("settings");
        if(!Uint(settings.at("width"),result.settings.width)||!Uint(settings.at("height"),result.settings.height)||!Uint(settings.at("samples"),result.settings.samples)
            ||!ValidateBrdfEnergySettings(result.settings))return Invalid(diagnostics,"settings");
        result.bakeKey=header.at("bake_key").get<std::string>();const auto count=std::size_t(result.settings.width)*result.settings.height,expected=(count+result.settings.height)*8;
        if(header.at("mapping")!=mapping||header.at("minimum_nv")!=BrdfEnergyMinimumNv||header.at("minimum_roughness")!=BrdfEnergyMinimumRoughness
            ||header.at("layout")!="directional AB float32-le row-major then average AB float32-le"
            ||!header.at("directional_count").is_number_unsigned()||header.at("directional_count").get<std::uint64_t>()!=count
            ||!header.at("average_count").is_number_unsigned()||header.at("average_count").get<std::uint64_t>()!=result.settings.height
            ||!header.at("payload_bytes").is_number_unsigned()||header.at("payload_bytes").get<std::uint64_t>()!=expected||bytes.size()-12-length!=expected)return Invalid(diagnostics,"layout/counts");
        const auto payload=bytes.subspan(12+length);if(header.at("payload_sha256")!=Sha256(payload))return Invalid(diagnostics,"payload hash");
        result.directional.resize(count);result.average.resize(result.settings.height);std::size_t cursor=0;
        for(auto table:{std::span(result.directional),std::span(result.average)})for(auto& value:table)
        {value={std::bit_cast<float>(U32(payload.data()+cursor)),std::bit_cast<float>(U32(payload.data()+cursor+4))};cursor+=8;}
        if(!ValidateBrdfEnergyLut(result))return Invalid(diagnostics,"energy/average identity");output=std::move(result);return true;
    }
    catch(const std::exception&){return Invalid(diagnostics);}
}
bool SaveBrdfEnergy(const std::filesystem::path& path,const BrdfEnergyLut& lut,std::vector<AssetDiagnostic>& diagnostics)
{
    std::vector<std::uint8_t> bytes;if(!EncodeBrdfEnergy(lut,bytes,diagnostics))return false;
    if(!asset_io::WriteAtomic(path,bytes)){asset_io::Diagnostic(diagnostics,"Assets.WriteFailed",asset_io::Utf8(path));return false;}return true;
}
bool LoadBrdfEnergy(const std::filesystem::path& path,BrdfEnergyLut& output,std::vector<AssetDiagnostic>& diagnostics)
{
    std::error_code error;const auto size=std::filesystem::file_size(path,error);if(error||size>MaximumBytes)return Invalid(diagnostics,"file size");
    std::vector<std::uint8_t> bytes;if(!asset_io::Read(path,bytes)){asset_io::Diagnostic(diagnostics,"Assets.ReadFailed",asset_io::Utf8(path));return false;}return DecodeBrdfEnergy(bytes,output,diagnostics);
}
bool CookBrdfEnergy(const std::filesystem::path& destination,const BrdfEnergySettings& settings,bool& cacheHit,std::vector<AssetDiagnostic>& diagnostics,std::uint32_t threads)
{
    cacheHit=false;const auto key=BrdfEnergyBakeKey(settings);if(key.empty()||threads<1||threads>16){asset_io::Diagnostic(diagnostics,"BrdfEnergy.InvalidSettings");return false;}
    BrdfEnergyLut cached;std::vector<AssetDiagnostic> ignored;
    if(LoadBrdfEnergy(destination,cached,ignored)&&cached.bakeKey==key){cacheHit=true;return true;}
    BrdfEnergyLut result;return BakeBrdfEnergy(settings,result,diagnostics,threads)&&SaveBrdfEnergy(destination,result,diagnostics);
}
}
