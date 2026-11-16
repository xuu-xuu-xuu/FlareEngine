#include "ResourceLayer/Assets/BrdfEnergyBake.h"
#include "ResourceLayer/Assets/LtcBake.h"
#include "ResourceLayer/Assets/SheenEnergyBake.h"
#include "ResourceLayer/Assets/SkinDiffuseBake.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "CoreLayer/Hash.h"
#include "CoreLayer/Localization.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <iostream>
#include <string_view>

namespace
{
bool ThreadCount(std::wstring_view text,std::uint32_t& value)
{
    if(text.empty()||text.find_first_not_of(L"0123456789")!=std::wstring_view::npos)return false;
    try{const auto n=std::stoull(std::wstring(text));if(n<1||n>16)return false;value=static_cast<std::uint32_t>(n);return true;}catch(...){return false;}
}
}
// defaults <Lighting-directory> [--threads 1..16]。只在离线工具运行，不由 Render 调用。
int wmain(int argc,wchar_t** argv)
{
    Localization text;std::string ignored;text.Load(GetExecutableDirectory()/"Resources/Localization/zh-CN.txt",ignored);
    const auto print=[&](const char* key,const std::string& detail={}){std::cerr<<text.GetText(key);if(!detail.empty())std::cerr<<": "<<detail;std::cerr<<'\n';};
    const auto usage=[&]{print("RenderingCooker.Usage");return 2;};std::uint32_t threads=1;
    if(argc!=3&&argc!=5)return usage();if(std::wstring_view(argv[1])!=L"defaults")return usage();
    if(argc==5&&(std::wstring_view(argv[3])!=L"--threads"||!ThreadCount(argv[4],threads)))return usage();
    try
    {
        const auto directory=std::filesystem::absolute(argv[2]);std::filesystem::create_directories(directory);
        std::vector<se::AssetDiagnostic> diagnostics;nlohmann::json tables=nlohmann::json::array();
        const auto record=[&](const char* name,bool hit,std::chrono::steady_clock::time_point start)
        {
            std::vector<std::uint8_t> bytes;if(!se::asset_io::Read(directory/name,bytes))return false;
            const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            tables.push_back({{"file",name},{"sha256",se::Sha256(bytes)},{"bytes",bytes.size()},{"cache_hit",hit},{"cpu_wall_ms",ms}});return true;
        };
        const auto failed=[&](const char* name){for(const auto& d:diagnostics)print(d.key.c_str(),d.detail);print("RenderingCooker.Failed",name);return 1;};
        bool hit=false;auto start=std::chrono::steady_clock::now();const se::BrdfEnergySettings direct;
        if(!se::CookBrdfEnergy(directory/"direct-energy.sebrdf",direct,hit,diagnostics,threads)||!record("direct-energy.sebrdf",hit,start))return failed("direct-energy.sebrdf");
        start=std::chrono::steady_clock::now();const se::LtcBakeSettings ltc;
        if(!se::CookLtc(directory/"rectangle.seltc",ltc,hit,diagnostics)||!record("rectangle.seltc",hit,start))return failed("rectangle.seltc");
        start=std::chrono::steady_clock::now();se::SheenEnergyLut sheen;
        hit=se::LoadSheenEnergy(directory/"sheen.sesheen",sheen)&&sheen.size==ltc.size&&sheen.samples==4096;
        if((!hit&&(!se::BakeSheenEnergy(ltc.size,4096,sheen)||!se::SaveSheenEnergy(directory/"sheen.sesheen",sheen)))||!record("sheen.sesheen",hit,start))return failed("sheen.sesheen");
        start=std::chrono::steady_clock::now();const se::SkinDiffuseSettings skin;
        if(!se::CookSkinDiffuse(directory/"skin.seskin",skin,hit,diagnostics,threads)||!record("skin.seskin",hit,start))return failed("skin.seskin");
        const auto report=nlohmann::json{{"format","SelfEngine.RenderingTables"},{"schema",1},{"tables",tables},
            {"direct",{{"algorithm",se::BrdfEnergyCookerVersion},{"width",direct.width},{"height",direct.height},{"samples",direct.samples}}},
            {"ltc",{{"algorithm",se::LtcCookerVersion},{"size",ltc.size},{"sample_side",ltc.sampleSide}}},
            {"sheen",{{"distribution","Charlie/Ashikhmin; normalized directional energy"},{"size",ltc.size},{"samples",4096}}},
            {"skin",{{"algorithm",se::SkinDiffuseCookerVersion},{"width",skin.width},{"height",skin.height},{"samples_per_gaussian",skin.samples}}},
            {"timing_scope","offline CPU bake/load; not frame/GPU timing"},{"gpu_upload","RGBA32Float; CPU LTC RG plus sheen energy/normalizer BA packed only during upload"},
            {"ltc_citation","Heitz, Dupuy, Hill, Neubelt. Real-Time Polygonal-Light Shading with Linearly Transformed Cosines. ACM TOG 35(4), 2016."},
            {"ltc_license_notice","Binary distributions retain ThirdParty/LtcReference/LICENSE and glm/LICENSE-MIT.txt."}}.dump(2);
        if(!se::asset_io::WriteAtomic(directory/"rendering-tables.json",{reinterpret_cast<const std::uint8_t*>(report.data()),report.size()}))return failed("rendering-tables.json");
        print("RenderingCooker.Complete");return 0;
    }
    catch(const std::exception&){print("RenderingCooker.Failed");return 1;}
}
