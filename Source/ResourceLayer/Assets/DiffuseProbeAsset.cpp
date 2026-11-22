#include "DiffuseProbeAsset.h"
#include "AssetIO.h"
#include "CoreLayer/Hash.h"
#include "CoreLayer/Utf.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <bit>
#include <limits>
#include <set>

namespace flare
{
namespace
{
using Json=nlohmann::json;
constexpr std::uint8_t magic[]{'S','E','S','H','0','0','0','1'};
constexpr std::size_t MaximumHeader=1024*1024,MaximumAsset=16*1024*1024;
constexpr const char* basis="Y00,y,z,x,xy,yz,3z2-1,xz,x2-y2";
bool Invalid(std::vector<AssetDiagnostic>& diagnostics,const char* detail="")
{asset_io::Diagnostic(diagnostics,"DiffuseProbe.InvalidAsset",detail);return false;}
bool Digest(const std::string& hash)
{return hash.size()==64&&std::all_of(hash.begin(),hash.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');});}
bool RelativePath(const std::string& path)
{
    std::u32string text;
    if(path.empty()||path.size()>1024||path.front()=='/'||path.back()=='/'||path.find_first_of(":\\")!=std::string::npos
        ||std::any_of(path.begin(),path.end(),[](unsigned char c){return c<32;})
        ||!DecodeUtf8(path,text))return false;
    std::size_t from=0;
    while(from<path.size())
    {const auto end=path.find('/',from),length=(end==std::string::npos?path.size():end)-from;const auto part=path.substr(from,length);if(part.empty()||part=="."||part=="..")return false;if(end==std::string::npos)break;from=end+1;}
    return true;
}
bool Settings(const DiffuseProbeBakeSettings& settings)
{return settings.samples>=64&&settings.samples<=131072&&IsFinite(settings.rayBias)&&settings.rayBias>0
    &&IsFinite(settings.maxDistance)&&settings.maxDistance>settings.rayBias*4;}
bool Grid(const DiffuseProbeGrid& grid,std::size_t& probes,std::size_t& cells)
{
    probes=1;cells=1;
    if(!IsFinite(grid.origin)||!IsFinite(grid.spacing)||grid.spacing.x<=0||grid.spacing.y<=0||grid.spacing.z<=0)return false;
    for(const auto count:grid.dimensions){if(count<2||count>128)return false;probes*=count;cells*=count-1;}
    if(probes>65536||grid.diffuse.size()!=probes||grid.probeValid.size()!=probes||grid.cellValid.size()!=cells)return false;
    const auto maximum=grid.origin+Hadamard(grid.spacing,{float(grid.dimensions[0]-1),float(grid.dimensions[1]-1),float(grid.dimensions[2]-1)});
    if(!IsFinite(maximum)||maximum.x<=grid.origin.x||maximum.y<=grid.origin.y||maximum.z<=grid.origin.z)return false;
    for(const auto& coefficients:grid.diffuse)if(!IsFinite(coefficients))return false;
    for(const auto valid:grid.probeValid)if(valid>1)return false;
    for(const auto valid:grid.cellValid)if(valid>1)return false;
    return true;
}
void U32(std::vector<std::uint8_t>& bytes,std::uint32_t value)
{for(unsigned shift=0;shift<32;shift+=8)bytes.push_back(static_cast<std::uint8_t>(value>>shift));}
std::uint32_t U32(const std::uint8_t* bytes)
{return std::uint32_t(bytes[0])|(std::uint32_t(bytes[1])<<8)|(std::uint32_t(bytes[2])<<16)|(std::uint32_t(bytes[3])<<24);}
void Coefficients(std::vector<std::uint8_t>& bytes,const Sh9Rgb& value)
{for(const auto c:value.coefficients)for(const auto f:{c.x,c.y,c.z})U32(bytes,std::bit_cast<std::uint32_t>(f));}
void Coefficients(std::span<const std::uint8_t> bytes,std::size_t& cursor,Sh9Rgb& value)
{
    for(auto& c:value.coefficients)
    {c={std::bit_cast<float>(U32(bytes.data()+cursor)),std::bit_cast<float>(U32(bytes.data()+cursor+4)),std::bit_cast<float>(U32(bytes.data()+cursor+8))};cursor+=12;}
}
Json Vector(Vec3 v){return {v.x,v.y,v.z};}
bool Vector(const Json& json,Vec3& output)
{
    if(!json.is_array()||json.size()!=3)return false;
    float values[3];
    for(std::size_t i=0;i<3;++i)
    {if(!json[i].is_number())return false;const auto n=json[i].get<double>();if(!std::isfinite(n)||std::abs(n)>std::numeric_limits<float>::max())return false;values[i]=float(n);}
    output={values[0],values[1],values[2]};return true;
}
bool Unsigned(const Json& json,std::uint32_t& output)
{if(!json.is_number_unsigned()||json.get<std::uint64_t>()>UINT32_MAX)return false;output=json.get<std::uint32_t>();return true;}
bool Number(const Json& json,float& output)
{if(!json.is_number())return false;const auto n=json.get<double>();if(!std::isfinite(n)||std::abs(n)>std::numeric_limits<float>::max())return false;output=float(n);return true;}
}
bool ValidateDiffuseProbeAsset(const DiffuseProbeAsset& asset)
{
    if(!IsFinite(asset.globalDiffuse)||!Settings(asset.bakeSettings)||!Digest(asset.environmentSourceHash)||asset.sources.empty()||asset.sources.size()>512)return false;
    std::string previous;
    for(const auto& source:asset.sources)
    {if(!RelativePath(source.path)||!Digest(source.sha256)||(!previous.empty()&&source.path<=previous))return false;previous=source.path;}
    if(asset.local)
    {std::size_t probes,cells;if(!Digest(asset.localSourceHash)||!Grid(*asset.local,probes,cells))return false;}
    else if(!asset.localSourceHash.empty())return false;
    return true;
}
bool EncodeDiffuseProbeAsset(const DiffuseProbeAsset& asset,std::vector<std::uint8_t>& output,std::vector<AssetDiagnostic>& diagnostics)
{
    if(!ValidateDiffuseProbeAsset(asset))return Invalid(diagnostics);
    try
    {
        std::vector<std::uint8_t> payload;Coefficients(payload,asset.globalDiffuse);
        Json local=nullptr;
        if(asset.local)
        {
            const auto& grid=*asset.local;
            for(const auto& probe:grid.diffuse)Coefficients(payload,probe);
            payload.insert(payload.end(),grid.probeValid.begin(),grid.probeValid.end());payload.insert(payload.end(),grid.cellValid.begin(),grid.cellValid.end());
            local={{"origin",Vector(grid.origin)},{"spacing",Vector(grid.spacing)},{"dimensions",grid.dimensions},
                {"probe_count",grid.diffuse.size()},{"cell_count",grid.cellValid.size()}};
        }
        Json sources=Json::array();for(const auto& source:asset.sources)sources.push_back({{"path",source.path},{"sha256",source.sha256}});
        const Json metadata{{"format","FlareEngine.DiffuseProbe"},{"schema",1},{"algorithm",DiffuseProbeCookerVersion},{"basis",basis},
            {"units","diffuse E/pi"},{"transfer","linear-rgb-f32-le"},{"coefficients_per_probe",9},{"channels",3},
            {"environment_source_sha256",asset.environmentSourceHash},{"local_source_sha256",asset.localSourceHash},{"sources",sources},
            {"bake",{{"samples",asset.bakeSettings.samples},{"ray_bias",asset.bakeSettings.rayBias},{"max_distance",asset.bakeSettings.maxDistance}}},
            {"local",local},{"payload_bytes",payload.size()},{"payload_sha256",Sha256(payload)}};
        const auto text=metadata.dump();
        if(text.size()>MaximumHeader||text.size()+12+payload.size()>MaximumAsset)return Invalid(diagnostics,"size limit");
        std::vector<std::uint8_t> result(std::begin(magic),std::end(magic));U32(result,static_cast<std::uint32_t>(text.size()));
        result.insert(result.end(),text.begin(),text.end());result.insert(result.end(),payload.begin(),payload.end());output=std::move(result);return true;
    }
    catch(const std::exception&){return Invalid(diagnostics);}
}
bool DecodeDiffuseProbeAsset(std::span<const std::uint8_t> bytes,DiffuseProbeAsset& output,std::vector<AssetDiagnostic>& diagnostics)
{
    if(bytes.size()>MaximumAsset||bytes.size()<12||!std::equal(std::begin(magic),std::end(magic),bytes.begin()))return Invalid(diagnostics);
    const auto length=U32(bytes.data()+8);if(!length||length>MaximumHeader||std::uint64_t(length)+12>bytes.size())return Invalid(diagnostics,"header size");
    try
    {
        const auto metadata=Json::parse(bytes.begin()+12,bytes.begin()+12+length);
        if(metadata.at("format")!="FlareEngine.DiffuseProbe"||metadata.at("schema")!=1||metadata.at("algorithm")!=DiffuseProbeCookerVersion)
        {asset_io::Diagnostic(diagnostics,"DiffuseProbe.VersionUnsupported");return false;}
        if(metadata.at("basis")!=basis||metadata.at("units")!="diffuse E/pi"||metadata.at("transfer")!="linear-rgb-f32-le"
            ||metadata.at("coefficients_per_probe")!=9||metadata.at("channels")!=3)return Invalid(diagnostics,"basis/units/layout");
        if(!metadata.at("payload_bytes").is_number_unsigned()||metadata.at("payload_bytes").get<std::uint64_t>()!=bytes.size()-12-length)return Invalid(diagnostics,"payload length");
        const auto payload=bytes.subspan(12+length);if(metadata.at("payload_sha256")!=Sha256(payload))return Invalid(diagnostics,"payload hash");
        DiffuseProbeAsset asset;asset.environmentSourceHash=metadata.at("environment_source_sha256").get<std::string>();
        asset.localSourceHash=metadata.at("local_source_sha256").get<std::string>();
        const auto& sources=metadata.at("sources");if(!sources.is_array()||sources.size()>512)return Invalid(diagnostics,"source limit");
        for(const auto& source:sources)asset.sources.push_back({source.at("path").get<std::string>(),source.at("sha256").get<std::string>()});
        const auto& bake=metadata.at("bake");
        if(!Unsigned(bake.at("samples"),asset.bakeSettings.samples)||!Number(bake.at("ray_bias"),asset.bakeSettings.rayBias)
            ||!Number(bake.at("max_distance"),asset.bakeSettings.maxDistance))return Invalid(diagnostics,"bake settings");
        std::size_t expected=108,probes=0,cells=0;
        const auto& local=metadata.at("local");
        if(!local.is_null())
        {
            auto& grid=asset.local.emplace();if(!Vector(local.at("origin"),grid.origin)||!Vector(local.at("spacing"),grid.spacing))return Invalid(diagnostics,"grid vectors");
            const auto& dims=local.at("dimensions");if(!dims.is_array()||dims.size()!=3)return Invalid(diagnostics,"grid dimensions");
            probes=1;cells=1;
            for(std::size_t i=0;i<3;++i)
            {if(!Unsigned(dims[i],grid.dimensions[i])||grid.dimensions[i]<2||grid.dimensions[i]>128)return Invalid(diagnostics,"grid dimensions");probes*=grid.dimensions[i];cells*=grid.dimensions[i]-1;}
            if(probes>65536||!local.at("probe_count").is_number_unsigned()||!local.at("cell_count").is_number_unsigned()
                ||local.at("probe_count").get<std::uint64_t>()!=probes||local.at("cell_count").get<std::uint64_t>()!=cells)return Invalid(diagnostics,"grid counts");
            expected+=probes*108+probes+cells;
            if(expected!=payload.size())return Invalid(diagnostics,"grid bytes");
            grid.diffuse.resize(probes);grid.probeValid.resize(probes);grid.cellValid.resize(cells);
        }
        if(expected!=payload.size())return Invalid(diagnostics,"global bytes");
        std::size_t cursor=0;Coefficients(payload,cursor,asset.globalDiffuse);
        if(asset.local)
        {
            auto& grid=*asset.local;for(auto& probe:grid.diffuse)Coefficients(payload,cursor,probe);
            std::copy_n(payload.begin()+cursor,probes,grid.probeValid.begin());cursor+=probes;
            std::copy_n(payload.begin()+cursor,cells,grid.cellValid.begin());cursor+=cells;
        }
        if(cursor!=payload.size()||!ValidateDiffuseProbeAsset(asset))return Invalid(diagnostics);
        output=std::move(asset);return true;
    }
    catch(const std::exception&){return Invalid(diagnostics);}
}
bool SaveDiffuseProbeAsset(const std::filesystem::path& path,const DiffuseProbeAsset& asset,std::vector<AssetDiagnostic>& diagnostics)
{
    if(path.extension()!=L".sesh")return Invalid(diagnostics,".sesh path");
    std::vector<std::uint8_t> bytes;if(!EncodeDiffuseProbeAsset(asset,bytes,diagnostics))return false;
    if(!asset_io::WriteAtomic(path,bytes)){asset_io::Diagnostic(diagnostics,"Assets.WriteFailed",asset_io::Utf8(path));return false;}return true;
}
bool LoadDiffuseProbeAsset(const std::filesystem::path& path,DiffuseProbeAsset& output,std::vector<AssetDiagnostic>& diagnostics)
{
    std::error_code error;const auto size=std::filesystem::file_size(path,error);
    if(error){asset_io::Diagnostic(diagnostics,"Assets.ReadFailed",asset_io::Utf8(path));return false;}
    if(size>MaximumAsset)return Invalid(diagnostics,"file size limit");
    std::vector<std::uint8_t> bytes;if(!asset_io::Read(path,bytes)){asset_io::Diagnostic(diagnostics,"Assets.ReadFailed",asset_io::Utf8(path));return false;}
    return DecodeDiffuseProbeAsset(bytes,output,diagnostics);
}
}
