#include "SkinDiffuseBake.h"
#include "AssetIO.h"
#include "CoreLayer/Hash.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <bit>
#include <thread>

namespace se
{
namespace
{
using Json=nlohmann::json;
constexpr double pi=3.1415926535897932384626433832795;
// d'Eon/Luebke, GPU Gems 3, Chapter 14, Figure 14-13。variance 以 radius² 为单位。
// 这里只使用公开数学参数，自行实现球面积分，不复制上游示例着色器。
constexpr double variances[]{.0064,.0484,.187,.567,1.99,7.41};
constexpr double weights[][3]{{.233,.455,.649},{.1,.336,.344},{.118,.198,0},{.113,.007,.007},{.358,.004,0},{.078,0,0}};
constexpr std::uint8_t magic[]{'S','E','S','K','I','N','0','1'};
constexpr std::size_t MaximumBytes=4*1024*1024,MaximumHeader=65536;
constexpr const char* mapping="x=(signedNoL+1)/2;y=sqrt(radius*curvature/8);endpoints;half-texel";
constexpr const char* layout="RGB float32-le row-major; x fastest; cosine replacement, no albedo/pi/radiance/visibility";
Json Profile()
{
    Json v=Json::array(),w=Json::array();for(const auto variance:variances)v.push_back(variance);
    for(const auto& row:weights)w.push_back(Json::array({row[0],row[1],row[2]}));
    return{{"variance",std::move(v)},{"weights",std::move(w)},{"model","normalized spherical chord-distance diffusion; constant irradiance preserved per RGB"}};
}
Json SettingsJson(const SkinDiffuseSettings& s){return{{"width",s.width},{"height",s.height},{"samples",s.samples}};}
bool Domain(float nl,float q){return IsFinite(nl)&&IsFinite(q)&&nl>=-1&&nl<=1&&q>=0&&q<=SkinDiffuseMaximumCurvature;}
bool Value(Vec3 v){return IsFinite(v)&&v.x>=0&&v.x<=1&&v.y>=0&&v.y<=1&&v.z>=0&&v.z<=1;}
bool Invalid(std::vector<AssetDiagnostic>& d,const char* detail=""){asset_io::Diagnostic(d,"SkinDiffuse.InvalidAsset",detail);return false;}
double AzimuthLambert(double a,double b)
{
    if(a>=b)return a;if(a<=-b)return 0;
    const auto t=std::clamp(-a/b,-1.0,1.0);
    return (a*std::acos(t)+b*std::sqrt(std::max(0.0,1-t*t)))/pi;
}
Vec3 Integrate(double nl,double q,std::uint32_t samples)
{
    if(q==0){const auto diffuse=float(std::max(0.0,nl));return{diffuse,diffuse,diffuse};}
    const auto tangentLight=std::sqrt(std::max(0.0,1-nl*nl));double sum[3]{},normalization[3]{};
    for(unsigned gaussian=0;gaussian<6;++gaussian)
    {
        // u=(1-cos(theta))/(variance*q²)，Gaussian*dA=e^-u du dphi/(2*pi)。
        // 指数 CDF 在窄瓣与小球两个极限都无需事后自适应扩大样本数。
        const auto a=variances[gaussian]*q*q,mass=-std::expm1(-2/a);double diffuse=0;
        for(std::uint32_t sample=0;sample<samples;++sample)
        {
            const auto s=(double(sample)+.5)/samples,u=-std::log1p(-s*mass),cosTheta=std::clamp(1-a*u,-1.0,1.0);
            diffuse+=AzimuthLambert(nl*cosTheta,tangentLight*std::sqrt(std::max(0.0,1-cosTheta*cosTheta)));
        }
        diffuse=diffuse/samples*mass;
        for(unsigned channel=0;channel<3;++channel){sum[channel]+=weights[gaussian][channel]*diffuse;normalization[channel]+=weights[gaussian][channel]*mass;}
    }
    return{float(std::clamp(sum[0]/normalization[0],0.0,1.0)),float(std::clamp(sum[1]/normalization[1],0.0,1.0)),float(std::clamp(sum[2]/normalization[2],0.0,1.0))};
}
void U32(std::vector<std::uint8_t>& b,std::uint32_t n){for(unsigned i=0;i<4;++i)b.push_back(static_cast<std::uint8_t>(n>>(8*i)));}
std::uint32_t U32(const std::uint8_t* b){return b[0]|(std::uint32_t(b[1])<<8)|(std::uint32_t(b[2])<<16)|(std::uint32_t(b[3])<<24);}
bool Uint(const Json& j,std::uint32_t& v){if(!j.is_number_unsigned()||j.get<std::uint64_t>()>UINT32_MAX)return false;v=j.get<std::uint32_t>();return true;}
}
bool ValidateSkinDiffuseSettings(const SkinDiffuseSettings& s)
{return s.width>=17&&s.width<=1025&&(s.width%2)==1&&s.height>=16&&s.height<=256&&s.samples>=256&&s.samples<=65536&&std::has_single_bit(s.samples)&&std::uint64_t(s.width)*s.height*s.samples<=268435456;}
std::string SkinDiffuseBakeKey(const SkinDiffuseSettings& s)
{
    if(!ValidateSkinDiffuseSettings(s))return{};
    return Sha256(Json{{"algorithm",SkinDiffuseCookerVersion},{"settings",SettingsJson(s)},{"profile",Profile()},{"mapping",mapping},{"maximum_curvature",SkinDiffuseMaximumCurvature}}.dump());
}
bool SkinDiffuseUv(const SkinDiffuseSettings& s,float nl,float q,Vec2& output)
{
    if(!ValidateSkinDiffuseSettings(s)||!Domain(nl,q))return false;
    output={float(((double(nl)+1)*.5*(s.width-1)+.5)/s.width),float((std::sqrt(double(q)/SkinDiffuseMaximumCurvature)*(s.height-1)+.5)/s.height)};return true;
}
bool SampleSkinDiffuse(const SkinDiffuseLut& lut,float nl,float q,Vec3& output)
{
    Vec2 uv;const auto& s=lut.settings;if(!SkinDiffuseUv(s,nl,q,uv)||lut.pixels.size()!=std::size_t(s.width)*s.height)return false;
    const auto x=std::clamp(double(uv.x)*s.width-.5,0.0,double(s.width-1)),y=std::clamp(double(uv.y)*s.height-.5,0.0,double(s.height-1));
    const auto ix=std::min(std::uint32_t(x),s.width-2),iy=std::min(std::uint32_t(y),s.height-2);const auto fx=float(x-ix),fy=float(y-iy);
    for(const auto index:{ix+iy*s.width,ix+1+iy*s.width,ix+(iy+1)*s.width,ix+1+(iy+1)*s.width})if(!Value(lut.pixels[index]))return false;
    const auto a=lut.pixels[ix+iy*s.width]*(1-fx)+lut.pixels[ix+1+iy*s.width]*fx,b=lut.pixels[ix+(iy+1)*s.width]*(1-fx)+lut.pixels[ix+1+(iy+1)*s.width]*fx;
    const auto result=a*(1-fy)+b*fy;if(!IsFinite(result))return false;
    output={std::clamp(result.x,0.f,1.f),std::clamp(result.y,0.f,1.f),std::clamp(result.z,0.f,1.f)};return true;
}
bool EstimateSkinDiffuse(float nl,float q,std::uint32_t samples,Vec3& output)
{
    if(!Domain(nl,q)||samples<256||samples>1048576||!std::has_single_bit(samples))return false;
    const auto result=Integrate(nl,q,samples);if(!Value(result))return false;output=result;return true;
}
bool BakeSkinDiffuse(const SkinDiffuseSettings& settings,SkinDiffuseLut& output,std::vector<AssetDiagnostic>& d,std::uint32_t threads)
{
    if(!ValidateSkinDiffuseSettings(settings)||threads<1||threads>16){asset_io::Diagnostic(d,"SkinDiffuse.InvalidSettings");return false;}
    try
    {
        SkinDiffuseLut result;result.settings=settings;result.bakeKey=SkinDiffuseBakeKey(settings);result.pixels.resize(std::size_t(settings.width)*settings.height);std::atomic<std::uint32_t> nextRow{0};
        const auto worker=[&]{for(;;){const auto y=nextRow.fetch_add(1);if(y>=settings.height)return;const auto coordinate=double(y)/(settings.height-1),q=SkinDiffuseMaximumCurvature*coordinate*coordinate;
            for(std::uint32_t x=0;x<settings.width;++x)result.pixels[x+y*settings.width]=Integrate(-1+2.0*x/(settings.width-1),q,settings.samples);}};
        std::vector<std::jthread> workers;for(std::uint32_t i=0;i<std::min(threads,settings.height);++i)workers.emplace_back(worker);workers.clear();
        if(!ValidateSkinDiffuseLut(result)){asset_io::Diagnostic(d,"SkinDiffuse.BakeFailed");return false;}output=std::move(result);return true;
    }
    catch(const std::exception&){asset_io::Diagnostic(d,"SkinDiffuse.BakeFailed");return false;}
}
bool ValidateSkinDiffuseLut(const SkinDiffuseLut& lut)
{
    const auto& s=lut.settings;if(!ValidateSkinDiffuseSettings(s)||lut.bakeKey!=SkinDiffuseBakeKey(s)||lut.pixels.size()!=std::size_t(s.width)*s.height)return false;
    for(const auto p:lut.pixels)if(!Value(p))return false;
    for(std::uint32_t x=0;x<s.width;++x){const auto expected=float(std::max(0.0,-1+2.0*x/(s.width-1)));const auto p=lut.pixels[x];if(std::abs(p.x-expected)>1e-6f||std::abs(p.y-expected)>1e-6f||std::abs(p.z-expected)>1e-6f)return false;}
    return true;
}
bool EncodeSkinDiffuse(const SkinDiffuseLut& lut,std::vector<std::uint8_t>& output,std::vector<AssetDiagnostic>& d)
{
    if(!ValidateSkinDiffuseLut(lut))return Invalid(d);std::vector<std::uint8_t> payload;payload.reserve(lut.pixels.size()*12);
    for(const auto p:lut.pixels)for(const auto v:{p.x,p.y,p.z})U32(payload,std::bit_cast<std::uint32_t>(v));
    const auto header=Json{{"format","SelfEngine.SkinDiffuse"},{"schema",1},{"algorithm",SkinDiffuseCookerVersion},{"settings",SettingsJson(lut.settings)},
        {"profile",Profile()},{"mapping",mapping},{"layout",layout},{"maximum_curvature",SkinDiffuseMaximumCurvature},{"bake_key",lut.bakeKey},
        {"count",lut.pixels.size()},{"payload_bytes",payload.size()},{"payload_sha256",Sha256(payload)}}.dump();
    if(header.size()>MaximumHeader||header.size()+12+payload.size()>MaximumBytes)return Invalid(d,"size limit");
    std::vector<std::uint8_t> bytes(std::begin(magic),std::end(magic));U32(bytes,static_cast<std::uint32_t>(header.size()));bytes.insert(bytes.end(),header.begin(),header.end());bytes.insert(bytes.end(),payload.begin(),payload.end());output=std::move(bytes);return true;
}
bool DecodeSkinDiffuse(std::span<const std::uint8_t> bytes,SkinDiffuseLut& output,std::vector<AssetDiagnostic>& d)
{
    if(bytes.size()<12||bytes.size()>MaximumBytes||!std::equal(std::begin(magic),std::end(magic),bytes.begin()))return Invalid(d);
    const auto length=U32(bytes.data()+8);if(!length||length>MaximumHeader||std::uint64_t(length)+12>bytes.size())return Invalid(d,"header size");
    try
    {
        const auto h=Json::parse(bytes.begin()+12,bytes.begin()+12+length);
        if(h.at("format")!="SelfEngine.SkinDiffuse"||h.at("schema")!=1||h.at("algorithm")!=SkinDiffuseCookerVersion){asset_io::Diagnostic(d,"SkinDiffuse.VersionUnsupported");return false;}
        SkinDiffuseLut result;const auto& s=h.at("settings");if(!Uint(s.at("width"),result.settings.width)||!Uint(s.at("height"),result.settings.height)||!Uint(s.at("samples"),result.settings.samples)||!ValidateSkinDiffuseSettings(result.settings))return Invalid(d,"settings");
        const auto count=std::size_t(result.settings.width)*result.settings.height,expected=count*12;
        if(h.at("mapping")!=mapping||h.at("layout")!=layout||h.at("profile")!=Profile()||h.at("maximum_curvature")!=SkinDiffuseMaximumCurvature
            ||!h.at("count").is_number_unsigned()||h.at("count").get<std::uint64_t>()!=count||!h.at("payload_bytes").is_number_unsigned()||h.at("payload_bytes").get<std::uint64_t>()!=expected||bytes.size()-12-length!=expected)return Invalid(d,"layout/counts");
        const auto payload=bytes.subspan(12+length);if(h.at("payload_sha256")!=Sha256(payload))return Invalid(d,"payload hash");result.bakeKey=h.at("bake_key").get<std::string>();result.pixels.resize(count);
        std::size_t cursor=0;const auto next=[&]{const auto value=std::bit_cast<float>(U32(payload.data()+cursor));cursor+=4;return value;};for(auto& p:result.pixels)p={next(),next(),next()};
        if(!ValidateSkinDiffuseLut(result))return Invalid(d,"values/key");output=std::move(result);return true;
    }
    catch(const std::exception&){return Invalid(d);}
}
bool SaveSkinDiffuse(const std::filesystem::path& path,const SkinDiffuseLut& lut,std::vector<AssetDiagnostic>& d)
{std::vector<std::uint8_t> bytes;if(!EncodeSkinDiffuse(lut,bytes,d))return false;if(!asset_io::WriteAtomic(path,bytes)){asset_io::Diagnostic(d,"Assets.WriteFailed",asset_io::Utf8(path));return false;}return true;}
bool LoadSkinDiffuse(const std::filesystem::path& path,SkinDiffuseLut& output,std::vector<AssetDiagnostic>& d)
{std::error_code e;const auto size=std::filesystem::file_size(path,e);if(e||size>MaximumBytes)return Invalid(d,"file size");std::vector<std::uint8_t> bytes;if(!asset_io::Read(path,bytes)){asset_io::Diagnostic(d,"Assets.ReadFailed",asset_io::Utf8(path));return false;}return DecodeSkinDiffuse(bytes,output,d);}
bool CookSkinDiffuse(const std::filesystem::path& path,const SkinDiffuseSettings& settings,bool& cacheHit,std::vector<AssetDiagnostic>& d,std::uint32_t threads)
{
    cacheHit=false;const auto key=SkinDiffuseBakeKey(settings);if(key.empty()||threads<1||threads>16){asset_io::Diagnostic(d,"SkinDiffuse.InvalidSettings");return false;}
    SkinDiffuseLut cached;std::vector<AssetDiagnostic> ignored;if(LoadSkinDiffuse(path,cached,ignored)&&cached.bakeKey==key){cacheHit=true;return true;}
    SkinDiffuseLut result;return BakeSkinDiffuse(settings,result,d,threads)&&SaveSkinDiffuse(path,result,d);
}
}
