#include "LtcBake.h"
#include "AssetIO.h"
#include "CoreLayer/Hash.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <bit>
#pragma warning(push)
#pragma warning(disable: 4201 4244 4458 4100 4996)
#include "LtcFitAdapter.h"
#pragma warning(pop)

namespace flare
{
namespace
{
using Json=nlohmann::json;
constexpr double pi=3.1415926535897932384626433832795;
constexpr std::uint8_t magic[]{'S','E','L','T','C','0','0','1'};
constexpr std::size_t MaximumBytes=2*1024*1024,MaximumHeader=65536;
constexpr const char* mapping="x=perceptual-roughness-affine;y=2/pi*acos(sqrt((Nv-minNv)/(1-minNv)));endpoints;half-texel";
constexpr const char* layout="inverse(xx,zx,xz,zz;yy=1) float32-le row-major then magnitude(E,B) float32-le row-major";
constexpr const char* upstream="selfshadow/ltc_code@31e5e96b54f98f33098f8503003119ba2231a1c6;glm@40398d67cd3e4f74b08649eda428dc411d801fd5";
bool Invalid(std::vector<AssetDiagnostic>& d,const char* detail=""){asset_io::Diagnostic(d,"Ltc.InvalidAsset",detail);return false;}
bool Domain(float nv,float r){return IsFinite(nv)&&IsFinite(r)&&nv>=LtcMinimumNv&&nv<=1&&r>=LtcMinimumRoughness&&r<=1;}
bool Matrix(Vec4 m){return IsFinite(m.x)&&IsFinite(m.y)&&IsFinite(m.z)&&IsFinite(m.w)&&double(m.x)*m.w-double(m.y)*m.z>0;}
bool Magnitude(Vec2 v){return IsFinite(v.x)&&IsFinite(v.y)&&v.x>=0&&v.y>=0&&v.y<=v.x+1e-6f&&v.x<=1.05f;}
void U32(std::vector<std::uint8_t>& bytes,std::uint32_t value){for(unsigned i=0;i<4;++i)bytes.push_back(static_cast<std::uint8_t>(value>>(8*i)));}
std::uint32_t U32(const std::uint8_t* b){return b[0]|(std::uint32_t(b[1])<<8)|(std::uint32_t(b[2])<<16)|(std::uint32_t(b[3])<<24);}
bool Uint(const Json& j,std::uint32_t& v){if(!j.is_number_unsigned()||j.get<std::uint64_t>()>UINT32_MAX)return false;v=j.get<std::uint32_t>();return true;}
Json SettingsJson(const LtcBakeSettings& s){return{{"size",s.size},{"sample_side",s.sampleSide}};}
struct D3
{
    double x,y,z;
    D3 operator+(D3 b)const{return{x+b.x,y+b.y,z+b.z};}
    D3 operator-(D3 b)const{return{x-b.x,y-b.y,z-b.z};}
    D3 operator*(double a)const{return{x*a,y*a,z*a};}
};
D3 D(Vec3 a){return{a.x,a.y,a.z};}
double Dot(D3 a,D3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
D3 Cross(D3 a,D3 b){return{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
D3 Unit(D3 a){return a*(1/std::sqrt(Dot(a,a)));}
bool Unit(Vec3 a){return IsFinite(a)&&std::abs(LengthSquared(a)-1)<=2e-4f;}
bool Emitter(const RectangleEmitter& e,Vec3 p)
{
    if(!IsFinite(e.center)||!IsFinite(e.halfU)||!IsFinite(e.halfV)||!IsFinite(e.radiance)||!IsFinite(p)
        ||e.radiance.x<0||e.radiance.y<0||e.radiance.z<0)return false;
    const auto u=D(e.halfU),v=D(e.halfV);const auto uu=Dot(u,u),vv=Dot(v,v);
    return uu>1e-16&&vv>1e-16&&std::abs(Dot(u,v))<=1e-5*std::sqrt(uu*vv);
}
std::vector<D3> ClipHorizon(const std::vector<D3>& polygon)
{
    std::vector<D3> clipped;clipped.reserve(8);
    for(std::size_t i=0;i<polygon.size();++i)
    {
        const auto a=polygon[i],b=polygon[(i+1)%polygon.size()];
        if(a.z>=0)clipped.push_back(a);
        if((a.z>=0)!=(b.z>=0))clipped.push_back(a+(b-a)*(a.z/(a.z-b.z)));
    }
    return clipped;
}
// 先裁真实 receiver 半球，再裁变换余弦半球，然后精确球面边积分。
// 两者缺一不可；倾斜 LTC 的支持域可能穿过真实 horizon，不能让背面 receiver 受光。
double Polygon(const RectangleEmitter& e,D3 p,D3 n,D3 tangent,D3 bitangent,Vec4 m)
{
    if(Dot(Cross(D(e.halfU),D(e.halfV)),p-D(e.center))<=0)return 0;
    const auto c=D(e.center)-p,u=D(e.halfU),v=D(e.halfV);
    std::vector<D3> polygon;polygon.reserve(6);
    for(const auto point:{c-u-v,c-u+v,c+u+v,c+u-v})
    {
        const auto x=Dot(point,tangent),y=Dot(point,bitangent),z=Dot(point,n);
        polygon.push_back({x,y,z});
    }
    polygon=ClipHorizon(polygon);
    for(auto& point:polygon)point={m.x*point.x+m.z*point.z,point.y,m.y*point.x+m.w*point.z};
    auto clipped=ClipHorizon(polygon);
    if(clipped.size()<3)return 0;
    for(auto& point:clipped){if(Dot(point,point)<1e-28)return 0;point=Unit(point);}
    double sum=0;
    for(std::size_t i=0;i<clipped.size();++i)
    {
        const auto a=clipped[i],b=clipped[(i+1)%clipped.size()],cross=Cross(a,b);const auto length=std::sqrt(Dot(cross,cross));
        if(length>1e-15)sum+=cross.z*std::atan2(length,std::clamp(Dot(a,b),-1.0,1.0))/length;
    }
    return std::clamp(sum/(2*pi),0.0,1.0);
}
void Frame(D3 n,D3 view,D3& tangent,D3& bitangent)
{
    tangent=view-n*Dot(n,view);
    if(Dot(tangent,tangent)<1e-12)tangent=Cross(std::abs(n.z)<.9?D3{0,0,1}:D3{0,1,0},n);
    tangent=Unit(tangent);bitangent=Cross(n,tangent);
}
}
bool ValidateLtcSettings(const LtcBakeSettings& s)
{return s.size>=16&&s.size<=128&&s.sampleSide>=8&&s.sampleSide<=64&&std::has_single_bit(s.sampleSide)&&std::uint64_t(s.size)*s.size*s.sampleSide*s.sampleSide<=16777216;}
std::string LtcBakeKey(const LtcBakeSettings& s)
{
    if(!ValidateLtcSettings(s))return{};
    return Sha256(Json{{"algorithm",LtcCookerVersion},{"upstream",upstream},{"settings",SettingsJson(s)},{"mapping",mapping},
        {"min_nv",LtcMinimumNv},{"min_roughness",LtcMinimumRoughness},{"fit","upstream MIS cubic objective; Heitz2018 VNDF; Nelder-Mead 100"}}.dump());
}
bool LtcUv(const LtcBakeSettings& s,float nv,float r,Vec2& output)
{
    if(!ValidateLtcSettings(s)||!Domain(nv,r))return false;
    const auto x=std::clamp((double(r)-LtcMinimumRoughness)/(1-LtcMinimumRoughness),0.0,1.0),y=2/pi*std::acos(std::sqrt(std::clamp((double(nv)-LtcMinimumNv)/(1-LtcMinimumNv),0.0,1.0)));
    output={float((x*(s.size-1)+.5)/s.size),float((y*(s.size-1)+.5)/s.size)};return true;
}
bool SampleLtc(const LtcLut& lut,float nv,float roughness,LtcSample& output)
{
    const auto& s=lut.settings;Vec2 uv;
    if(!LtcUv(s,nv,roughness,uv)||lut.inverseMatrices.size()!=std::size_t(s.size)*s.size||lut.magnitudeFresnel.size()!=lut.inverseMatrices.size())return false;
    const auto x=std::clamp(double(uv.x)*s.size-.5,0.0,double(s.size-1)),y=std::clamp(double(uv.y)*s.size-.5,0.0,double(s.size-1));
    const auto ix=std::min(std::uint32_t(x),s.size-2),iy=std::min(std::uint32_t(y),s.size-2);const auto fx=x-ix,fy=y-iy;
    const std::size_t indices[]{ix+iy*s.size,ix+1+iy*s.size,ix+(iy+1)*s.size,ix+1+(iy+1)*s.size};
    const double weights[]{(1-fx)*(1-fy),fx*(1-fy),(1-fx)*fy,fx*fy};
    double xx=0,zx=0,xz=0,zz=0,e=0,b=0;
    for(unsigned i=0;i<4;++i){const auto m=lut.inverseMatrices[indices[i]];const auto a=lut.magnitudeFresnel[indices[i]];
        xx+=m.x*weights[i];zx+=m.y*weights[i];xz+=m.z*weights[i];zz+=m.w*weights[i];e+=a.x*weights[i];b+=a.y*weights[i];}
    LtcSample result{{float(xx),float(zx),float(xz),float(zz)},{float(e),float(b)}};
    if(!Matrix(result.inverse)||!Magnitude(result.magnitudeFresnel))return false;output=result;return true;
}
bool BakeLtc(const LtcBakeSettings& s,LtcLut& output,std::vector<AssetDiagnostic>& diagnostics)
{
    if(!ValidateLtcSettings(s)){asset_io::Diagnostic(diagnostics,"Ltc.InvalidSettings");return false;}
    try
    {
        LtcLut result;result.settings=s;result.bakeKey=LtcBakeKey(s);const auto count=std::size_t(s.size)*s.size;
        std::vector<glm::mat3> matrices(count);std::vector<glm::vec2> amplitudes(count);
        ltc_reference::sampleSide=static_cast<int>(s.sampleSide);const ltc_reference::BrdfGGXVisible brdf;
        ltc_reference::fitTab(matrices.data(),amplitudes.data(),static_cast<int>(s.size),brdf);
        result.inverseMatrices.reserve(count);result.magnitudeFresnel.reserve(count);
        for(std::size_t i=0;i<count;++i)
        {
            const auto inverse=glm::inverse(matrices[i])/glm::inverse(matrices[i])[1][1];
            result.inverseMatrices.push_back({inverse[0][0],inverse[0][2],inverse[2][0],inverse[2][2]});
            result.magnitudeFresnel.push_back({amplitudes[i].x,amplitudes[i].y});
        }
        if(!ValidateLtcLut(result)){asset_io::Diagnostic(diagnostics,"Ltc.BakeFailed");return false;}
        output=std::move(result);return true;
    }
    catch(const std::exception&){asset_io::Diagnostic(diagnostics,"Ltc.BakeFailed");return false;}
}
bool ValidateLtcLut(const LtcLut& lut)
{
    if(!ValidateLtcSettings(lut.settings)||lut.bakeKey!=LtcBakeKey(lut.settings)||lut.inverseMatrices.size()!=std::size_t(lut.settings.size)*lut.settings.size||lut.magnitudeFresnel.size()!=lut.inverseMatrices.size())return false;
    for(std::size_t i=0;i<lut.inverseMatrices.size();++i)if(!Matrix(lut.inverseMatrices[i])||!Magnitude(lut.magnitudeFresnel[i]))return false;return true;
}
bool EncodeLtc(const LtcLut& lut,std::vector<std::uint8_t>& output,std::vector<AssetDiagnostic>& diagnostics)
{
    if(!ValidateLtcLut(lut))return Invalid(diagnostics);
    std::vector<std::uint8_t> payload;payload.reserve(lut.inverseMatrices.size()*24);
    for(const auto m:lut.inverseMatrices)for(const auto v:{m.x,m.y,m.z,m.w})U32(payload,std::bit_cast<std::uint32_t>(v));
    for(const auto m:lut.magnitudeFresnel)for(const auto v:{m.x,m.y})U32(payload,std::bit_cast<std::uint32_t>(v));
    const auto header=Json{{"format","FlareEngine.Ltc"},{"schema",1},{"algorithm",LtcCookerVersion},{"upstream",upstream},{"bake_key",lut.bakeKey},
        {"settings",SettingsJson(lut.settings)},{"mapping",mapping},{"minimum_nv",LtcMinimumNv},{"minimum_roughness",LtcMinimumRoughness},{"layout",layout},
        {"count",lut.inverseMatrices.size()},{"payload_bytes",payload.size()},{"payload_sha256",Sha256(payload)}}.dump();
    if(header.size()>MaximumHeader||header.size()+12+payload.size()>MaximumBytes)return Invalid(diagnostics,"size limit");
    std::vector<std::uint8_t> bytes(std::begin(magic),std::end(magic));U32(bytes,static_cast<std::uint32_t>(header.size()));bytes.insert(bytes.end(),header.begin(),header.end());bytes.insert(bytes.end(),payload.begin(),payload.end());output=std::move(bytes);return true;
}
bool DecodeLtc(std::span<const std::uint8_t> bytes,LtcLut& output,std::vector<AssetDiagnostic>& diagnostics)
{
    if(bytes.size()<12||bytes.size()>MaximumBytes||!std::equal(std::begin(magic),std::end(magic),bytes.begin()))return Invalid(diagnostics);
    const auto length=U32(bytes.data()+8);if(!length||length>MaximumHeader||std::uint64_t(length)+12>bytes.size())return Invalid(diagnostics,"header length");
    try
    {
        const auto h=Json::parse(bytes.begin()+12,bytes.begin()+12+length);
        if(h.at("format")!="FlareEngine.Ltc"||h.at("schema")!=1||h.at("algorithm")!=LtcCookerVersion||h.at("upstream")!=upstream){asset_io::Diagnostic(diagnostics,"Ltc.VersionUnsupported");return false;}
        LtcLut result;const auto& settings=h.at("settings");
        if(!Uint(settings.at("size"),result.settings.size)||!Uint(settings.at("sample_side"),result.settings.sampleSide)||!ValidateLtcSettings(result.settings))return Invalid(diagnostics,"settings");
        result.bakeKey=h.at("bake_key").get<std::string>();const auto count=std::size_t(result.settings.size)*result.settings.size,expected=count*24;
        if(h.at("mapping")!=mapping||h.at("minimum_nv")!=LtcMinimumNv||h.at("minimum_roughness")!=LtcMinimumRoughness||h.at("layout")!=layout
            ||!h.at("count").is_number_unsigned()||h.at("count").get<std::uint64_t>()!=count||!h.at("payload_bytes").is_number_unsigned()
            ||h.at("payload_bytes").get<std::uint64_t>()!=expected||bytes.size()-12-length!=expected)return Invalid(diagnostics,"layout/counts");
        const auto payload=bytes.subspan(12+length);if(h.at("payload_sha256")!=Sha256(payload))return Invalid(diagnostics,"payload hash");
        result.inverseMatrices.resize(count);result.magnitudeFresnel.resize(count);std::size_t cursor=0;
        const auto next=[&]{const auto value=std::bit_cast<float>(U32(payload.data()+cursor));cursor+=4;return value;};
        for(auto& m:result.inverseMatrices)m={next(),next(),next(),next()};for(auto& a:result.magnitudeFresnel)a={next(),next()};
        if(!ValidateLtcLut(result))return Invalid(diagnostics,"coefficients/identity");output=std::move(result);return true;
    }
    catch(const std::exception&){return Invalid(diagnostics);}
}
bool SaveLtc(const std::filesystem::path& path,const LtcLut& lut,std::vector<AssetDiagnostic>& diagnostics)
{std::vector<std::uint8_t> bytes;if(!EncodeLtc(lut,bytes,diagnostics))return false;if(!asset_io::WriteAtomic(path,bytes)){asset_io::Diagnostic(diagnostics,"Assets.WriteFailed",asset_io::Utf8(path));return false;}return true;}
bool LoadLtc(const std::filesystem::path& path,LtcLut& output,std::vector<AssetDiagnostic>& diagnostics)
{std::error_code error;const auto size=std::filesystem::file_size(path,error);if(error||size>MaximumBytes)return Invalid(diagnostics,"file size");std::vector<std::uint8_t> bytes;if(!asset_io::Read(path,bytes)){asset_io::Diagnostic(diagnostics,"Assets.ReadFailed",asset_io::Utf8(path));return false;}return DecodeLtc(bytes,output,diagnostics);}
bool CookLtc(const std::filesystem::path& path,const LtcBakeSettings& s,bool& cacheHit,std::vector<AssetDiagnostic>& diagnostics)
{
    cacheHit=false;const auto key=LtcBakeKey(s);if(key.empty()){asset_io::Diagnostic(diagnostics,"Ltc.InvalidSettings");return false;}
    LtcLut cached;std::vector<AssetDiagnostic> ignored;if(LoadLtc(path,cached,ignored)&&cached.bakeKey==key){cacheHit=true;return true;}
    LtcLut result;return BakeLtc(s,result,diagnostics)&&SaveLtc(path,result,diagnostics);
}
bool EvaluateLtcRectangle(const LtcLut& lut,const RectangleEmitter& e,Vec3 p,Vec3 n,Vec3 v,float r,Vec3 f0,Vec3& output)
{
    if(!Emitter(e,p)||!Unit(n)||!Unit(v)||!IsFinite(f0)||f0.x<0||f0.y<0||f0.z<0||f0.x>1||f0.y>1||f0.z>1)return false;
    LtcSample sample;if(!SampleLtc(lut,std::clamp(flare::Dot(n,v),-1.0f,1.0f),r,sample))return false;
    D3 tangent,bitangent;Frame(D(n),D(v),tangent,bitangent);const auto integral=Polygon(e,D(p),D(n),tangent,bitangent,sample.inverse);
    const auto magnitude=sample.magnitudeFresnel;const auto scale=[&](float f){return float((f*magnitude.x+(1-f)*magnitude.y)*integral);};
    const auto result=Hadamard(e.radiance,{scale(f0.x),scale(f0.y),scale(f0.z)});if(!IsFinite(result))return false;output=result;return true;
}
bool EvaluateLambertRectangle(const RectangleEmitter& e,Vec3 p,Vec3 n,Vec3& output)
{
    if(!Emitter(e,p)||!Unit(n))return false;D3 tangent,bitangent;Frame(D(n),D(n),tangent,bitangent);
    const auto result=e.radiance*float(Polygon(e,D(p),D(n),tangent,bitangent,{1,0,0,1}));if(!IsFinite(result))return false;output=result;return true;
}
}
