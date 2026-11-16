#include "ResourceLayer/Assets/DiffuseProbeAsset.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Assets/AssetStore.h"
#include "ResourceLayer/Scenes/SceneSerializer.h"
#include "FunctionLayer/Rendering/DiffuseProbePreparation.h"
#include "ToolLayer/Tools/Project.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Hash.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <map>
#include <string_view>

namespace fs=std::filesystem;
using namespace se;
namespace
{
using Clock=std::chrono::steady_clock;
double Ms(Clock::time_point from){return std::chrono::duration<double,std::milli>(Clock::now()-from).count();}
bool Float(std::wstring_view text,float& output)
{
    try{std::size_t used=0;const auto value=std::stof(std::wstring(text),&used);if(used!=text.size()||!IsFinite(value))return false;output=value;return true;}catch(...){return false;}
}
bool Uint(std::wstring_view text,std::uint32_t& output)
{
    if(text.empty()||text.find_first_not_of(L"0123456789")!=std::wstring_view::npos)return false;
    try{const auto n=std::stoull(std::wstring(text));if(n>UINT32_MAX)return false;output=static_cast<std::uint32_t>(n);return true;}catch(...){return false;}
}
struct Options
{
    bool local=false;fs::path hdr,output,scene,content,project;
    Vec3 origin{},spacing{};std::array<std::uint32_t,3> dimensions{};DiffuseProbeBakeSettings bake;
};
bool Parse(int argc,wchar_t** argv,Options& options)
{
    if(argc<4)return false;
    const std::wstring_view mode=argv[1];if(mode!=L"global"&&mode!=L"local")return false;
    options.local=mode==L"local";options.hdr=fs::absolute(argv[2]);options.output=fs::absolute(argv[3]);
    if(!options.local)return argc==4;
    bool origin=false,spacing=false,dims=false,samples=false;std::map<std::wstring,bool> seen;
    for(int i=4;i<argc;++i)
    {
        const std::wstring key=argv[i];if(seen[key])return false;seen[key]=true;
        if(key==L"--scene"||key==L"--content"||key==L"--project")
        {if(++i>=argc)return false;auto& path=key==L"--scene"?options.scene:key==L"--content"?options.content:options.project;path=fs::absolute(argv[i]);}
        else if(key==L"--origin"||key==L"--spacing")
        {if(i+3>=argc)return false;auto& vector=key==L"--origin"?options.origin:options.spacing;if(!Float(argv[i+1],vector.x)||!Float(argv[i+2],vector.y)||!Float(argv[i+3],vector.z))return false;i+=3;(key==L"--origin"?origin:spacing)=true;}
        else if(key==L"--dims")
        {if(i+3>=argc)return false;for(unsigned c=0;c<3;++c)if(!Uint(argv[i+1+c],options.dimensions[c]))return false;i+=3;dims=true;}
        else if(key==L"--samples")
        {if(++i>=argc||!Uint(argv[i],options.bake.samples))return false;samples=true;}
        else if(key==L"--bias"||key==L"--distance")
        {if(++i>=argc||!Float(argv[i],key==L"--bias"?options.bake.rayBias:options.bake.maxDistance))return false;}
        else return false;
    }
    return origin&&spacing&&dims&&samples&&(options.project.empty()?(!options.scene.empty()&&!options.content.empty()):(options.scene.empty()&&options.content.empty()));
}
bool Digest(const fs::path& path,std::string& output,std::vector<AssetDiagnostic>& diagnostics)
{
    std::vector<std::uint8_t> bytes;if(!asset_io::Read(path,bytes)){asset_io::Diagnostic(diagnostics,"Assets.ReadFailed",asset_io::Utf8(path.filename()));return false;}
    output=Sha256(bytes);return !output.empty();
}
}
int wmain(int argc,wchar_t** argv)
{
    Localization text;std::string localizationError;
    // 稳定键保留为独立命令行启动回退；不会写死中文或改变并行编译控制台代码页。
    text.Load(fs::current_path()/"Resources/Localization/zh-CN.txt",localizationError);
    const auto print=[&](const char* key,const std::string& detail){std::cerr<<text.GetText(key);if(!detail.empty())std::cerr<<": "<<detail;std::cerr<<'\n';};
    Options options;if(!Parse(argc,argv,options)){print("DiffuseProbe.CookerUsage",{});return 2;}
    std::vector<AssetDiagnostic> diagnostics;
    try
    {
        const auto totalStart=Clock::now();auto start=Clock::now();
        std::vector<std::uint8_t> hdrBytes;
        if(!asset_io::Read(options.hdr,hdrBytes)){print("Assets.ReadFailed",asset_io::Utf8(options.hdr.filename()));return 1;}
        FloatImage environment;DiffuseProbeAsset asset;
        if(!DecodeRadiance(hdrBytes,environment,diagnostics))throw std::runtime_error("DiffuseProbe.BakeInvalid");
        asset.environmentSourceHash=Sha256(hdrBytes);asset.bakeSettings=options.bake;
        asset.sources.push_back({"hdr/"+asset_io::Utf8(options.hdr.filename()),asset.environmentSourceHash});
        const double hdrLoadMs=Ms(start);start=Clock::now();Sh9Rgb radiance;
        if(!ProjectEnvironmentSh9(environment,radiance)||!ConvolveDiffuseSh9(radiance,asset.globalDiffuse))throw std::runtime_error("DiffuseProbe.BakeInvalid");
        const double projectMs=Ms(start);double prepareMs=0,bakeMs=0;PreparedDiffuseProbeScene prepared;
        std::string sceneHash,projectHash;ProjectDescription project;
        if(options.local)
        {
            start=Clock::now();
            if(!options.project.empty())
            {
                std::vector<ProjectDiagnostic> projectDiagnostics;
                if(!ReadProject(options.project,project,projectDiagnostics))
                {for(const auto& d:projectDiagnostics)asset_io::Diagnostic(diagnostics,d.key.c_str(),d.detail);throw std::runtime_error("DiffuseProbe.SceneInvalid");}
                for(const auto& body:project.bodies)if(body.mass>0)throw std::runtime_error("DiffuseProbe.UnsupportedSource");
                options.scene=project.entryScene;options.content=project.contentRoot;
                if(!Digest(options.project,projectHash,diagnostics))throw std::runtime_error("DiffuseProbe.SceneInvalid");
                asset.sources.push_back({"project/"+asset_io::Utf8(options.project.filename()),projectHash});
            }
            if(!Digest(options.scene,sceneHash,diagnostics))throw std::runtime_error("DiffuseProbe.SceneInvalid");
            asset.sources.push_back({"scene/"+asset_io::Utf8(options.scene.filename()),sceneHash});
            World world;std::string error;
            if(!LoadScene(options.scene,world,error)){asset_io::Diagnostic(diagnostics,error.c_str());throw std::runtime_error("DiffuseProbe.SceneInvalid");}
            AssetStore assets(options.content,options.output.parent_path()/".diffuse-probe-cache");
            if(!assets.Scan(diagnostics)||!PrepareDiffuseProbeScene(world,assets,prepared,diagnostics))throw std::runtime_error("DiffuseProbe.SceneInvalid");
            asset.sources.insert(asset.sources.end(),prepared.sources.begin(),prepared.sources.end());
            asset.localSourceHash=Sha256(prepared.contentHash+sceneHash+projectHash);prepareMs=Ms(start);start=Clock::now();
            DiffuseProbeGrid grid;
            if(!BakeDiffuseProbeGrid(prepared.View(&environment),options.origin,options.spacing,options.dimensions,options.bake,grid))throw std::runtime_error("DiffuseProbe.BakeInvalid");
            asset.local=std::move(grid);bakeMs=Ms(start);
            std::string latest;
            if(!Digest(options.scene,latest,diagnostics)||latest!=sceneHash)throw std::runtime_error("Assets.SourceChanged");
            if(!options.project.empty()&&(!Digest(options.project,latest,diagnostics)||latest!=projectHash))throw std::runtime_error("Assets.SourceChanged");
        }
        std::string latest;if(!Digest(options.hdr,latest,diagnostics)||latest!=asset.environmentSourceHash)throw std::runtime_error("Assets.SourceChanged");
        for(const auto& source:prepared.sources)
            if(!source.path.starts_with("content/")||!Digest(options.content/asset_io::FromUtf8(source.path.substr(8)),latest,diagnostics)||latest!=source.sha256)
                throw std::runtime_error("Assets.SourceChanged");
        std::sort(asset.sources.begin(),asset.sources.end(),[](const auto& a,const auto& b){return a.path<b.path;});
        if(!ValidateDiffuseProbeAsset(asset))throw std::runtime_error("DiffuseProbe.InvalidAsset");
        start=Clock::now();std::vector<std::uint8_t> bytes;
        if(!EncodeDiffuseProbeAsset(asset,bytes,diagnostics))throw std::runtime_error("DiffuseProbe.InvalidAsset");
        const auto encodeMs=Ms(start);
        nlohmann::json report{{"format","SelfEngine.DiffuseProbeCookReport"},{"version",1},{"algorithm",DiffuseProbeCookerVersion},
            {"artifact",asset_io::Utf8(options.output.filename())},{"artifact_bytes",bytes.size()},{"artifact_sha256",Sha256(bytes)},
            {"hdr_width",environment.width},{"hdr_height",environment.height},{"sources",nlohmann::json::array()},
            {"triangles",prepared.triangles.size()},{"point_lights",prepared.lights.size()},{"source_entities",prepared.sourceEntityIds},
            {"local",options.local},{"coefficient_units","E/pi"},{"scope","one Lambert diffuse bounce from point lights; ray misses sample explicit HDR; excludes Fresnel/specular/textures/animation"},
            {"timing_scope","offline CPU wall clock; not GPU/frame time"},{"hdr_load_ms",hdrLoadMs},{"global_project_ms",projectMs},
            {"scene_prepare_ms",prepareMs},{"local_bake_ms",bakeMs},{"encode_ms",encodeMs},{"total_cpu_ms_before_publish",Ms(totalStart)}};
        for(const auto& source:asset.sources)report["sources"].push_back({{"path",source.path},{"sha256",source.sha256}});
        if(asset.local)
        {
            const auto& grid=*asset.local;report["origin"]={grid.origin.x,grid.origin.y,grid.origin.z};report["spacing"]={grid.spacing.x,grid.spacing.y,grid.spacing.z};report["dimensions"]=grid.dimensions;
            report["probe_count"]=grid.diffuse.size();report["cell_count"]=grid.cellValid.size();report["valid_probes"]=std::count(grid.probeValid.begin(),grid.probeValid.end(),1);
            report["valid_cells"]=std::count(grid.cellValid.begin(),grid.cellValid.end(),1);report["samples"]=asset.bakeSettings.samples;
        }
        auto reportPath=options.output;reportPath+=".json";const auto reportText=report.dump(2);
        if(!asset_io::WriteAtomic(reportPath,{reinterpret_cast<const std::uint8_t*>(reportText.data()),reportText.size()})||!asset_io::WriteAtomic(options.output,bytes))
            throw std::runtime_error("Assets.WriteFailed");
        print("DiffuseProbe.CookerComplete",asset_io::Utf8(options.output.filename()));print("DiffuseProbe.CookerTiming",std::to_string(Ms(totalStart))+" ms");
        return 0;
    }
    catch(const std::exception& error)
    {
        for(const auto& d:diagnostics)print(d.key.c_str(),d.detail);
        print(error.what(),{});return 1;
    }
}
