#include "EditorScriptTools.h"
#include "EditorProcessRunner.h"
#include "EditorSessionStore.h"
#include "CoreLayer/Utf.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ToolLayer/Tools/Project.h"
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <set>

namespace flare
{
namespace
{
namespace fs=std::filesystem;
using Json=nlohmann::json;
bool Fail(std::string& key,std::string& detail,const char* value,const fs::path& path={})
{key=value;detail=asset_io::Utf8(path);return false;}
bool Safe(const fs::path& root,const fs::path& path)
{
    if(!path.is_absolute()||!asset_io::Within(root,path))return false;
    fs::path part;
    for(const auto& component:path)
    {
        part/=component;const DWORD flags=GetFileAttributesW(part.c_str());
        if(flags!=INVALID_FILE_ATTRIBUTES){if(flags&FILE_ATTRIBUTE_REPARSE_POINT)return false;}
        else if(GetLastError()!=ERROR_FILE_NOT_FOUND&&GetLastError()!=ERROR_PATH_NOT_FOUND)return false;
    }
    return true;
}
std::wstring Lower(std::wstring value)
{std::transform(value.begin(),value.end(),value.begin(),[](wchar_t c){return static_cast<wchar_t>(std::towlower(c));});return value;}
bool Read(const fs::path& path,std::string& text)
{
    std::vector<std::uint8_t> bytes;if(!asset_io::Read(path,bytes))return false;
    text.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());return true;
}
bool Write(const fs::path& path,std::string_view text,bool replace)
{return asset_io::WriteAtomic(path,{reinterpret_cast<const std::uint8_t*>(text.data()),text.size()},replace);}
bool Namespace(std::string_view value)
{
    if(value.empty())return true;
    for(std::size_t start=0;;)
    {
        const auto end=value.find('.',start);
        if(!EditorScriptTools::IsIdentifier(value.substr(start,end==std::string_view::npos?value.size()-start:end-start)))return false;
        if(end==std::string_view::npos)return true;start=end+1;
    }
}
bool FileName(std::string_view value)
{
    auto name=Lower(asset_io::FromUtf8(value).wstring());
    if(name==L"con"||name==L"prn"||name==L"aux"||name==L"nul"||name==L"clock$")return false;
    return !(name.size()==4&&(name.starts_with(L"com")||name.starts_with(L"lpt"))&&name[3]>=L'1'&&name[3]<=L'9');
}
Json ParseBuild(std::string_view text)
{
    bool duplicate=false;std::vector<std::set<std::string>> keys;
    auto json=Json::parse(text.begin(),text.end(),[&](int depth,Json::parse_event_t event,Json& value)
    {
        if(depth>32)throw std::runtime_error("build document depth");
        if(event==Json::parse_event_t::object_start)keys.emplace_back();
        else if(event==Json::parse_event_t::key&&!keys.empty()&&!keys.back().insert(value.get<std::string>()).second)duplicate=true;
        else if(event==Json::parse_event_t::object_end)keys.pop_back();return true;
    });
    const auto format=json.value("format",std::string{});
    if(duplicate||!json.is_object()||(format!="FlareEngine.Build"&&format!="SelfEngine.Build")||json.value("version",0)!=1)
        throw std::runtime_error("Project.InvalidBuildSettings");
    return json;
}
constexpr std::string_view ScriptProject=R"xml(<Project Sdk="Microsoft.NET.Sdk">
  <Import Project="$(FlareEngineRoot)/SDK/FlareEngine.Scripts.props" Condition="Exists('$(FlareEngineRoot)/SDK/FlareEngine.Scripts.props')" />
  <PropertyGroup>
    <TargetFramework>net10.0</TargetFramework>
    <ImplicitUsings>enable</ImplicitUsings>
    <Nullable>enable</Nullable>
    <PlatformTarget>x64</PlatformTarget>
    <AssemblyName>FlareGame.Scripts</AssemblyName>
  </PropertyGroup>
  <ItemGroup Condition="Exists('$(FlareEngineRoot)/SDK/Managed/FlareEngine.Managed.dll')">
    <Reference Include="FlareEngine.Managed">
      <HintPath>$(FlareEngineRoot)/SDK/Managed/FlareEngine.Managed.dll</HintPath>
      <Private>false</Private>
    </Reference>
  </ItemGroup>
  <ItemGroup Condition="!Exists('$(FlareEngineRoot)/SDK/Managed/FlareEngine.Managed.dll') And Exists('$(FlareEngineRoot)/Source/FunctionLayer/Scripting/Managed/Engine.Managed.csproj')">
    <ProjectReference Include="$(FlareEngineRoot)/Source/FunctionLayer/Scripting/Managed/Engine.Managed.csproj" />
  </ItemGroup>
  <Target Name="CheckFlareEngineApi" BeforeTargets="ResolveReferences">
    <Error Condition="!Exists('$(FlareEngineRoot)/SDK/Managed/FlareEngine.Managed.dll') And !Exists('$(FlareEngineRoot)/Source/FunctionLayer/Scripting/Managed/Engine.Managed.csproj')" Text="Script.EngineApiMissing: FlareEngineRoot" />
  </Target>
</Project>
)xml";

std::string XmlEscape(std::string_view value)
{
    std::string out;
    out.reserve(value.size() + 16);
    for (const char ch : value)
    {
        switch (ch)
        {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default: out.push_back(ch); break;
        }
    }
    return out;
}

std::string ToForwardSlashes(std::string value)
{
    std::replace(value.begin(), value.end(), '\\', '/');
    return value;
}

std::string ToBackslashes(std::string value)
{
    std::replace(value.begin(), value.end(), '/', '\\');
    return value;
}

std::string PortableDirectoryBuildProps(std::string_view relIdeProps)
{
    const std::string escaped = XmlEscape(relIdeProps);
    return "<Project>\n"
           "  <!-- 模板独立配置；位于引擎仓库内或复制到其他目录时均不继承外层配置。 -->\n"
           "  <PropertyGroup>\n"
           "    <DefaultItemExcludes>$(DefaultItemExcludes);obj/**;bin/**</DefaultItemExcludes>\n"
           "  </PropertyGroup>\n"
           "  <Import Project=\"" + escaped + "\" Condition=\"'$(FlareEngineRoot)' == '' And Exists('" + escaped + "')\" />\n"
           "</Project>\n";
}

bool IsLegacyStubDirectoryBuildProps(std::string_view content)
{
    std::string compact;
    compact.reserve(content.size());
    for (const char ch : content)
    {
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')
            compact.push_back(ch);
    }
    return compact == "<Project/>" ||
           compact == "<Project></Project>" ||
           compact == "<Project><!--模板独立配置；位于引擎仓库内或复制到其他目录时均不继承外层配置。--></Project>" ||
           (content.find("<!-- 模板独立配置；位于引擎仓库内或复制到其他目录时均不继承外层配置。 -->") != std::string_view::npos &&
            content.find("DefaultItemExcludes") == std::string_view::npos);
}

bool WriteIfChanged(const fs::path& path, std::string_view text, bool& changed)
{
    changed = false;
    std::error_code ec;
    std::string existing;
    if (fs::is_regular_file(path, ec) && !ec && Read(path, existing) && existing == text)
        return true;
    if (!path.parent_path().empty())
        fs::create_directories(path.parent_path(), ec);
    if (!Write(path, text, true))
        return false;
    changed = true;
    return true;
}

std::string SanitizeSolutionStem(std::string_view rawName)
{
    std::string out;
    out.reserve(rawName.size());
    for (const unsigned char uc : rawName)
    {
        const char ch = static_cast<char>(uc);
        if (uc < 32 || ch == '<' || ch == '>' || ch == ':' || ch == '"' ||
            ch == '/' || ch == '\\' || ch == '|' || ch == '?' || ch == '*')
        {
            out.push_back('_');
        }
        else
        {
            out.push_back(ch);
        }
    }
    while (!out.empty() && (out.front() == ' ' || out.front() == '.'))
        out.erase(out.begin());
    while (!out.empty() && (out.back() == ' ' || out.back() == '.'))
        out.pop_back();
    if (out.empty() || out.size() > 120 || !FileName(out))
        return "FlareProject";
    return out;
}

std::string FormatDeterministicGuid(std::string_view seed)
{
    std::uint64_t h1 = 14695981039346656037ull;
    std::uint64_t h2 = 1099511628211ull ^ 0x9E3779B97F4A7C15ull;
    for (const unsigned char b : seed)
    {
        h1 = (h1 ^ b) * 1099511628211ull;
        h2 = (h2 ^ (static_cast<std::uint64_t>(b) + 0x55u)) * 14695981039346656037ull;
    }
    const std::uint32_t d1 = static_cast<std::uint32_t>(h1 >> 32);
    const std::uint16_t d2 = static_cast<std::uint16_t>((h1 >> 16) & 0xFFFFu);
    const std::uint16_t d3 = static_cast<std::uint16_t>(((h1 & 0x0FFFu) | 0x4000u));
    const std::uint16_t d4 = static_cast<std::uint16_t>((((h2 >> 48) & 0x3FFFu) | 0x8000u));
    const std::uint64_t d5 = h2 & 0xFFFFFFFFFFFFull;
    char buf[40]{};
    std::snprintf(buf, sizeof(buf), "{%08X-%04X-%04X-%04X-%012llX}",
                  d1, d2, d3, d4, static_cast<unsigned long long>(d5));
    return buf;
}

std::string BuildSolutionContent(std::string_view projName, std::string_view relCsprojWin)
{
    const std::string guid = FormatDeterministicGuid(relCsprojWin);
    std::string sln = "\xEF\xBB\xBF\r\n"
                      "Microsoft Visual Studio Solution File, Format Version 12.00\r\n"
                      "# Visual Studio Version 17\r\n"
                      "VisualStudioVersion = 17.0.31903.59\r\n"
                      "MinimumVisualStudioVersion = 10.0.40219.1\r\n"
                      "Project(\"{FAE04EC0-301F-11D3-BF4B-00C04F79EFBC}\") = \"";
    sln += projName;
    sln += "\", \"";
    sln += relCsprojWin;
    sln += "\", \"";
    sln += guid;
    sln += "\"\r\nEndProject\r\n"
           "Global\r\n"
           "\tGlobalSection(SolutionConfigurationPlatforms) = preSolution\r\n"
           "\t\tDebug|Any CPU = Debug|Any CPU\r\n"
           "\t\tDebug|x64 = Debug|x64\r\n"
           "\t\tRelease|Any CPU = Release|Any CPU\r\n"
           "\t\tRelease|x64 = Release|x64\r\n"
           "\tEndGlobalSection\r\n"
           "\tGlobalSection(ProjectConfigurationPlatforms) = postSolution\r\n"
           "\t\t" + guid + ".Debug|Any CPU.ActiveCfg = Debug|Any CPU\r\n"
           "\t\t" + guid + ".Debug|Any CPU.Build.0 = Debug|Any CPU\r\n"
           "\t\t" + guid + ".Debug|x64.ActiveCfg = Debug|Any CPU\r\n"
           "\t\t" + guid + ".Debug|x64.Build.0 = Debug|Any CPU\r\n"
           "\t\t" + guid + ".Release|Any CPU.ActiveCfg = Release|Any CPU\r\n"
           "\t\t" + guid + ".Release|Any CPU.Build.0 = Release|Any CPU\r\n"
           "\t\t" + guid + ".Release|x64.ActiveCfg = Release|Any CPU\r\n"
           "\t\t" + guid + ".Release|x64.Build.0 = Release|Any CPU\r\n"
           "\tEndGlobalSection\r\n"
           "\tGlobalSection(SolutionProperties) = preSolution\r\n"
           "\t\tHideSolutionNode = FALSE\r\n"
           "\tEndGlobalSection\r\n"
           "EndGlobal\r\n";
    return sln;
}

fs::path FindDotnetExecutable()
{
    wchar_t dotnetRoot[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"DOTNET_ROOT", dotnetRoot, MAX_PATH) > 0)
    {
        const fs::path cand = fs::path(dotnetRoot) / L"dotnet.exe";
        std::error_code ec;
        if (fs::is_regular_file(cand, ec)) return cand;
    }
    wchar_t foundPath[MAX_PATH]{};
    if (SearchPathW(nullptr, L"dotnet.exe", nullptr, MAX_PATH, foundPath, nullptr) > 0)
    {
        std::error_code ec;
        if (fs::is_regular_file(foundPath, ec)) return foundPath;
    }
    const fs::path stdPath = L"C:\\Program Files\\dotnet\\dotnet.exe";
    std::error_code ec;
    if (fs::is_regular_file(stdPath, ec)) return stdPath;
    return {};
}

bool IsVsCodeLikeEditor(const std::wstring& lowerName)
{
    return lowerName == L"code.exe" ||
           lowerName == L"code - insiders.exe" ||
           lowerName == L"cursor.exe" ||
           lowerName == L"antigravity.exe" ||
           lowerName == L"agy.exe" ||
           lowerName == L"windsurf.exe" ||
           lowerName == L"codium.exe";
}

bool IsRiderEditor(const std::wstring& lowerName)
{
    return lowerName == L"rider64.exe" || lowerName == L"rider.exe";
}

bool IsVisualStudioEditor(const std::wstring& lowerName)
{
    return lowerName == L"devenv.exe";
}

fs::path EnvDirectory(const wchar_t* name)
{
    wchar_t buf[MAX_PATH]{};
    const DWORD len = GetEnvironmentVariableW(name, buf, MAX_PATH);
    return (len > 0 && len < MAX_PATH) ? fs::path(buf) : fs::path{};
}

fs::path FindSolutionInProjectRoot(const fs::path& root)
{
    ProjectDescription project;
    std::vector<ProjectDiagnostic> errs;
    std::error_code ec;
    if (Safe(root, root / "project.json") && ReadProject(root / "project.json", project, errs))
    {
        const fs::path preferred = root / asset_io::FromUtf8(SanitizeSolutionStem(project.name) + ".sln");
        if (Safe(root, preferred) && fs::is_regular_file(preferred, ec))
            return preferred;
    }
    if (fs::is_directory(root, ec))
    {
        for (const auto& entry : fs::directory_iterator(root, ec))
        {
            if (entry.is_regular_file(ec) && Lower(entry.path().extension().wstring()) == L".sln" && Safe(root, entry.path()))
                return entry.path();
        }
    }
    return {};
}
}

bool EditorScriptTools::IsIdentifier(std::string_view value)
{
    static const std::set<std::string_view> keywords={"abstract","as","base","bool","break","byte","case","catch","char","checked","class","const","continue","decimal","default","delegate","do","double","else","enum","event","explicit","extern","false","finally","fixed","float","for","foreach","goto","if","implicit","in","int","interface","internal","is","lock","long","namespace","new","null","object","operator","out","override","params","private","protected","public","readonly","ref","return","sbyte","sealed","short","sizeof","stackalloc","static","string","struct","switch","this","throw","true","try","typeof","uint","ulong","unchecked","unsafe","ushort","using","virtual","void","volatile","while"};
    if(value.empty()||value.size()>128||keywords.contains(value)||value=="__arglist"||value=="__makeref"||value=="__reftype"||value=="__refvalue")return false;
    std::u32string decoded;if(!DecodeUtf8(value,decoded)||decoded.empty())return false;
    for(std::size_t index=0;index<decoded.size();++index)
    {
        const auto code=decoded[index];if(code==U'_')continue;
        if(code<128)
        {
            if((code>=U'a'&&code<=U'z')||(code>=U'A'&&code<=U'Z')||(index>0&&code>=U'0'&&code<=U'9'))continue;
            return false;
        }
        if(code>0xffff||code<32)return false;
        const auto character=static_cast<wchar_t>(code);WORD category=0,marks=0;
        if(!GetStringTypeW(CT_CTYPE1,&character,1,&category)||!GetStringTypeW(CT_CTYPE3,&character,1,&marks))return false;
        if((category&C1_ALPHA)&&!(marks&C3_NONSPACING))continue;
        if(index>0&&((category&C1_DIGIT)||(marks&C3_NONSPACING)))continue;
        return false;
    }
    return true;
}

bool EditorScriptTools::CreateBehaviour(const EditorScriptCreateRequest& request,EditorScriptCreateResult& result,
    std::string& errorKey,std::string& detail)
{
    result={};errorKey.clear();detail.clear();
    if(!IsIdentifier(request.className)||!Namespace(request.nameSpace)||!FileName(request.className))return Fail(errorKey,detail,"Script.TemplateIdentifierInvalid");
    struct Created {fs::path path;std::string text;};
    std::vector<Created> created;std::vector<fs::path> directories;fs::path root,buildPath;
    bool buildUpdated=false;std::string buildOriginal,buildReplacement;
    const auto rollback=[&]()
    {
        bool complete=true;
        if(buildUpdated)
        {
            std::string current;
            if(!Read(buildPath,current)||current!=buildReplacement||!Write(buildPath,buildOriginal,true))complete=false;
        }
        for(auto it=created.rbegin();it!=created.rend();++it)
        {
            std::string current;std::error_code error;
            if(!fs::exists(it->path,error)&&!error)continue;
            if(!Safe(root,it->path)||!Read(it->path,current)||current!=it->text||!fs::remove(it->path,error)||error)complete=false;
        }
        for(auto it=directories.rbegin();it!=directories.rend();++it)
        {
            // 直接尝试非递归删除：只删除本事务创建的空目录，避免预先枚举与删除之间的竞态。
            // RemoveDirectoryW 也不会误删被其他程序替换成普通文件的路径。
            if(!Safe(root,*it)){complete=false;detail=asset_io::Utf8(*it);continue;}
            if(!RemoveDirectoryW(it->c_str()))
            {
                const auto error=GetLastError();
                if(error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND)
                {complete=false;detail=asset_io::Utf8(*it);}
            }
        }
        if(!complete)errorKey="Script.TemplateRollbackIncomplete";result={};return false;
    };
    try
    {
        root=fs::absolute(request.projectRoot).lexically_normal();
        if(!Safe(root,root))return Fail(errorKey,detail,"Script.TemplatePathInvalid",root);
        ProjectDescription project;std::vector<ProjectDiagnostic> projectErrors;
        if(!Safe(root,root/"project.json")||!ReadProject(root/"project.json",project,projectErrors))return Fail(errorKey,detail,"Script.TemplateProjectInvalid",root/"project.json");
        buildPath=root/"build.json";
        if(!Safe(root,buildPath))return Fail(errorKey,detail,"Script.TemplatePathInvalid",buildPath);
        const bool hadBuild=fs::exists(buildPath);
        Json build={{"format","FlareEngine.Build"},{"version",1}};
        if(hadBuild){if(!Read(buildPath,buildOriginal))return Fail(errorKey,detail,"Script.TemplateProjectInvalid",buildPath);build=ParseBuild(buildOriginal);}
        const auto configured=build.value("scriptsProject",std::string{});const bool newProject=configured.empty();
        const auto scripts=root/"Scripts";
        fs::path projectPath=newProject?scripts/"FlareGame.Scripts.csproj":fs::absolute(root/asset_io::FromUtf8(configured)).lexically_normal();
        if(!Safe(root,projectPath)||projectPath.extension()!=L".csproj"||(!newProject&&!fs::is_regular_file(projectPath)))
            return Fail(errorKey,detail,"Script.TemplateProjectInvalid",projectPath);
        if(!asset_io::Within(scripts,projectPath.parent_path())&&!asset_io::Within(projectPath.parent_path(),scripts))
            return Fail(errorKey,detail,"Script.TemplateProjectInvalid",projectPath);
        if(newProject&&fs::exists(projectPath))return Fail(errorKey,detail,"Script.TemplateConflict",projectPath);
        fs::path sourceDirectory=asset_io::Within(scripts,projectPath.parent_path())?projectPath.parent_path():scripts;
        if(!request.targetDirectory.empty())
        {
            const fs::path candDir=fs::absolute(request.targetDirectory.is_absolute()?request.targetDirectory:root/request.targetDirectory).lexically_normal();
            if(Safe(root,candDir)&&asset_io::Within(scripts,candDir))
                sourceDirectory=candDir;
        }
        const auto source=sourceDirectory/asset_io::FromUtf8(request.className+".cs");
        if(!Safe(root,source)||!asset_io::Within(scripts,source))return Fail(errorKey,detail,"Script.TemplatePathInvalid",source);
        if(fs::exists(source))return Fail(errorKey,detail,"Script.TemplateConflict",source);
        const auto id=request.nameSpace.empty()?request.className:(request.nameSpace+"."+request.className);
        std::string text="using FlareEngine;\n\n";
        if(!request.nameSpace.empty())text+="namespace "+request.nameSpace+";\n\n";
        text+="public class "+request.className+" : F_Behaviour\n{\n    public override void F_Start()\n    {\n    }\n\n    public override void F_Update(float delta)\n    {\n    }\n}\n";
        const auto create=[&](const fs::path& path,std::string contents)
        {
            if(!Safe(root,path))return false;
            std::vector<fs::path> missing;
            for(auto directory=path.parent_path();!fs::exists(directory);directory=directory.parent_path())
            {if(!Safe(root,directory))return false;missing.push_back(directory);}
            for(auto directory=missing.rbegin();directory!=missing.rend();++directory)
                if(fs::create_directory(*directory))directories.push_back(*directory);
            created.push_back({path,std::move(contents)});
            if(!Write(path,created.back().text,false)){created.pop_back();return false;}return true;
        };
        if(!create(source,text)){Fail(errorKey,detail,"Script.TemplateWriteFailed",source);return rollback();}
        if(newProject)
        {
            if(!create(projectPath,std::string(ScriptProject))){Fail(errorKey,detail,"Script.TemplateWriteFailed",projectPath);return rollback();}
            const auto props=scripts/"Directory.Build.props";
            if(!Safe(root,props)){Fail(errorKey,detail,"Script.TemplatePathInvalid",props);return rollback();}
            if(!fs::exists(props)&&!create(props,PortableDirectoryBuildProps("../out/Editor/FlareEngine.Ide.props"))){Fail(errorKey,detail,"Script.TemplateWriteFailed",props);return rollback();}
            build["scriptsProject"]=asset_io::Utf8(projectPath.lexically_relative(root));
            if(build.value("scriptType",std::string{}).empty())build["scriptType"]=id;
            buildReplacement=build.dump(2)+"\n";
            if(hadBuild)
            {
                std::string current;if(!Read(buildPath,current)||current!=buildOriginal)
                {Fail(errorKey,detail,"Script.TemplateSourceChanged",buildPath);return rollback();}
                if(!Write(buildPath,buildReplacement,true)){Fail(errorKey,detail,"Script.TemplateWriteFailed",buildPath);return rollback();}
                buildUpdated=true;
            }
            else if(!create(buildPath,buildReplacement)){Fail(errorKey,detail,"Script.TemplateWriteFailed",buildPath);return rollback();}
        }
        EditorScriptCreateResult completed{source,projectPath,id};
        for(const auto& file:created)completed.created.push_back(file.path);
        if(buildUpdated)completed.updated.push_back(buildPath);
        result=std::move(completed);return true;
    }
    catch(const std::exception& failure){errorKey="Script.TemplateProjectInvalid";detail=failure.what();return rollback();}
}

bool EditorScriptTools::SyncIdeWorkspace(const fs::path& requestedRoot,const fs::path& requestedEngineRoot,
    const fs::path& requestedHostExecutableDir,bool restoreDesignTimeAssets,EditorIdeWorkspaceResult* outResult)
{
    if(outResult)*outResult={};
    try
    {
        const auto root=fs::absolute(requestedRoot).lexically_normal();
        if(!Safe(root,root))return false;

        std::string projectName=asset_io::Utf8(root.filename());
        ProjectDescription project;std::vector<ProjectDiagnostic> projectErrors;
        if(Safe(root,root/"project.json")&&ReadProject(root/"project.json",project,projectErrors)&&!project.name.empty())
            projectName=project.name;
        const std::string slnStem=SanitizeSolutionStem(projectName);

        std::error_code ec;
        fs::path csproj;
        if(Safe(root,root/"build.json")&&fs::is_regular_file(root/"build.json",ec)&&!ec)
        {
            std::string buildText;
            if(Read(root/"build.json",buildText))
            {
                const auto build=ParseBuild(buildText);
                const auto configured=build.value("scriptsProject",std::string{});
                if(!configured.empty())
                    csproj=fs::absolute(root/asset_io::FromUtf8(configured)).lexically_normal();
            }
        }
        if(csproj.empty())
        {
            const fs::path defaultCsproj=root/"Scripts"/"FlareGame.Scripts.csproj";
            if(Safe(root,defaultCsproj)&&fs::is_regular_file(defaultCsproj,ec)&&!ec)
                csproj=defaultCsproj;
        }
        if(csproj.empty()||!Safe(root,csproj)||Lower(csproj.extension().wstring())!=L".csproj"||!fs::is_regular_file(csproj,ec)||ec)
            return false;

        const fs::path engRoot=requestedEngineRoot.empty()
            ? EditorSessionStore::EngineRootFromExecutable()
            : fs::absolute(requestedEngineRoot).lexically_normal();
        const fs::path exeDir=requestedHostExecutableDir.empty()
            ? GetExecutableDirectory()
            : fs::absolute(requestedHostExecutableDir).lexically_normal();

        fs::path sdkRoot;
        if(!engRoot.empty()&&fs::is_regular_file(engRoot/"SDK"/"Managed"/"FlareEngine.Managed.dll",ec)&&!ec)
        {
            sdkRoot=engRoot;
        }
        else if(!engRoot.empty()&&fs::is_regular_file(engRoot/"Source"/"FunctionLayer"/"Scripting"/"Managed"/"Engine.Managed.csproj",ec)&&!ec)
        {
            sdkRoot=engRoot;
        }
        else if(!exeDir.empty()&&fs::is_regular_file(exeDir/"FlareEngine.Managed.dll",ec)&&!ec)
        {
            const fs::path stagedSdk=root/"out"/"Editor"/"IdeSdk";
            if(!Safe(root,stagedSdk))return false;
            fs::create_directories(stagedSdk/"SDK"/"Managed",ec);
            fs::copy_file(exeDir/"FlareEngine.Managed.dll",stagedSdk/"SDK"/"Managed"/"FlareEngine.Managed.dll",
                          fs::copy_options::overwrite_existing,ec);
            if(fs::is_regular_file(exeDir/"FlareEngine.Managed.xml",ec)&&!ec)
            {
                fs::copy_file(exeDir/"FlareEngine.Managed.xml",stagedSdk/"SDK"/"Managed"/"FlareEngine.Managed.xml",
                              fs::copy_options::overwrite_existing,ec);
            }
            sdkRoot=stagedSdk;
        }
        if(sdkRoot.empty())return false;

        const fs::path idePropsPath=root/"out"/"Editor"/"FlareEngine.Ide.props";
        if(!Safe(root,idePropsPath))return false;
        const std::string idePropsContent=
            "<Project>\n"
            "  <!-- 由 FlareEngine 自动生成以供外部 IDE 解析引擎 API 与智能提示；请勿手动提交此本地路径文件。 -->\n"
            "  <PropertyGroup>\n"
            "    <FlareEngineRoot Condition=\"'$(FlareEngineRoot)' == ''\">"+XmlEscape(asset_io::Utf8(sdkRoot))+"</FlareEngineRoot>\n"
            "    <ImplicitUsings>enable</ImplicitUsings>\n"
            "    <Nullable>enable</Nullable>\n"
            "  </PropertyGroup>\n"
            "  <ItemGroup>\n"
            "    <Using Include=\"FlareEngine\" />\n"
            "  </ItemGroup>\n"
            "</Project>\n";
        bool idePropsChanged=false;
        if(!WriteIfChanged(idePropsPath,idePropsContent,idePropsChanged))return false;

        const fs::path propsPath=csproj.parent_path()/"Directory.Build.props";
        if(Safe(root,propsPath))
        {
            const std::string relIdeProps=ToForwardSlashes(asset_io::Utf8(idePropsPath.lexically_relative(csproj.parent_path())));
            bool dummy=false;
            if(!fs::exists(propsPath,ec))
            {
                WriteIfChanged(propsPath,PortableDirectoryBuildProps(relIdeProps),dummy);
            }
            else
            {
                std::string curProps;
                if(Read(propsPath,curProps)&&IsLegacyStubDirectoryBuildProps(curProps))
                    WriteIfChanged(propsPath,PortableDirectoryBuildProps(relIdeProps),dummy);
            }
        }

        const fs::path slnPath=root/asset_io::FromUtf8(slnStem+".sln");
        if(!Safe(root,slnPath))return false;
        const std::string relCsprojWin=ToBackslashes(asset_io::Utf8(csproj.lexically_relative(root)));
        const std::string slnContent=BuildSolutionContent(asset_io::Utf8(csproj.stem()),relCsprojWin);
        bool slnChanged=false;
        if(!WriteIfChanged(slnPath,slnContent,slnChanged))return false;

        const fs::path vscodeSettings=root/".vscode"/"settings.json";
        if(Safe(root,vscodeSettings)&&!fs::exists(vscodeSettings,ec))
        {
            const Json vsJson={
                {"dotnet.defaultSolution",slnStem+".sln"},
                {"omnisharp.defaultLaunchSolution",slnStem+".sln"}
            };
            bool vsChanged=false;
            WriteIfChanged(vscodeSettings,vsJson.dump(2)+"\n",vsChanged);
        }

        bool restored=false;
        if(restoreDesignTimeAssets)
        {
            const fs::path assetsJson=csproj.parent_path()/"obj"/"project.assets.json";
            if(idePropsChanged||!fs::is_regular_file(assetsJson,ec)||ec)
            {
                const fs::path dotnetExe=FindDotnetExecutable();
                if(!dotnetExe.empty())
                {
                    ProcessRunResult procOut{};
                    const fs::path restoreLog=root/"out"/"Editor"/"ide-restore.log";
                    std::vector<std::wstring> args={
                        L"restore",csproj.wstring(),
                        L"--nologo",L"-v:quiet",
                        L"-p:NuGetAudit=false"
                    };
                    if(RunEditorProcess(dotnetExe.wstring(),args,root,restoreLog,nullptr,procOut)&&
                       procOut.exitCode==0&&fs::is_regular_file(assetsJson,ec))
                    {
                        restored=true;
                    }
                }
            }
        }

        if(outResult)
        {
            outResult->solutionPath=slnPath;
            outResult->scriptProjectPath=csproj;
            outResult->idePropsPath=idePropsPath;
            outResult->sdkRoot=sdkRoot;
            outResult->restoredDesignTimeAssets=restored;
        }
        return true;
    }
    catch(...){return false;}
}

fs::path EditorScriptTools::DetectInstalledCodeEditor()
{
    bool preferVisualStudio=false;
    wchar_t assocBuf[MAX_PATH]{};
    DWORD assocLen=MAX_PATH;
    if(SUCCEEDED(AssocQueryStringW(ASSOCF_NOTRUNCATE,ASSOCSTR_EXECUTABLE,L".cs",L"open",assocBuf,&assocLen))&&assocLen>0)
    {
        std::error_code ec;
        const fs::path assocExe=fs::absolute(assocBuf,ec).lexically_normal();
        if(!ec&&fs::is_regular_file(assocExe,ec))
        {
            const std::wstring lower=Lower(assocExe.filename().wstring());
            if(IsVsCodeLikeEditor(lower)||IsRiderEditor(lower)||IsVisualStudioEditor(lower))
                return assocExe;
            if(lower==L"vslauncher.exe")
                preferVisualStudio=true;
        }
    }

    const fs::path localApp=EnvDirectory(L"LOCALAPPDATA");
    const fs::path progFiles=EnvDirectory(L"ProgramFiles");

    std::vector<fs::path> vsCodeFamily;
    for(const fs::path& base:{localApp/L"Programs",progFiles})
    {
        if(base.empty())continue;
        vsCodeFamily.push_back(base/L"Microsoft VS Code"/L"Code.exe");
        vsCodeFamily.push_back(base/L"cursor"/L"Cursor.exe");
        vsCodeFamily.push_back(base/L"Antigravity"/L"Antigravity.exe");
        vsCodeFamily.push_back(base/L"Windsurf"/L"Windsurf.exe");
        vsCodeFamily.push_back(base/L"VSCodium"/L"VSCodium.exe");
    }

    std::vector<fs::path> visualStudioFamily;
    if(!progFiles.empty())
    {
        const fs::path vsBase=progFiles/L"Microsoft Visual Studio";
        for(const wchar_t* ver:{L"2022",L"18"})
        {
            for(const wchar_t* sku:{L"Community",L"Professional",L"Enterprise",L"Preview"})
                visualStudioFamily.push_back(vsBase/ver/sku/L"Common7"/L"IDE"/L"devenv.exe");
        }
    }

    std::vector<fs::path> riderFamily;
    if(!localApp.empty())
        riderFamily.push_back(localApp/L"Programs"/L"Rider"/L"bin"/L"rider64.exe");
    std::error_code ec;
    if(!progFiles.empty()&&fs::is_directory(progFiles/L"JetBrains",ec))
    {
        for(const auto& entry:fs::directory_iterator(progFiles/L"JetBrains",ec))
        {
            if(entry.is_directory(ec))
                riderFamily.push_back(entry.path()/L"bin"/L"rider64.exe");
        }
    }

    const auto pickFirst=[&](const std::vector<fs::path>& list)->fs::path
    {
        for(const auto& cand:list)
        {
            std::error_code checkEc;
            if(!cand.empty()&&fs::is_regular_file(cand,checkEc)&&!checkEc)
                return fs::absolute(cand,checkEc).lexically_normal();
        }
        return {};
    };

    if(preferVisualStudio)
    {
        if(const auto vs=pickFirst(visualStudioFamily);!vs.empty())return vs;
    }
    if(const auto code=pickFirst(vsCodeFamily);!code.empty())return code;
    if(const auto vs=pickFirst(visualStudioFamily);!vs.empty())return vs;
    if(const auto rider=pickFirst(riderFamily);!rider.empty())return rider;
    return {};
}

bool EditorScriptTools::PrepareOpenSource(const fs::path& requestedRoot,const fs::path& requestedFile,
    std::uint32_t line,std::uint32_t column,const CodeEditorSettings& settings,EditorCodeOpenCommand& command,
    std::string& errorKey,std::string& detail)
{
    command={};errorKey.clear();detail.clear();
    try
    {
        const auto root=fs::absolute(requestedRoot).lexically_normal();
        const auto file=fs::absolute(requestedFile.is_absolute()?requestedFile:root/requestedFile).lexically_normal();
        static const std::set<std::wstring> extensions={L".cs",L".csproj",L".sln",L".props",L".targets",L".json",L".hlsl",L".hlsli",L".txt",L".log"};
        if(!Safe(root,file)||!fs::is_regular_file(file)||!extensions.contains(Lower(file.extension().wstring())))
            return Fail(errorKey,detail,"Script.EditorFileInvalid",file);
        EditorCodeOpenCommand prepared;prepared.file=file;prepared.workingDirectory=root;
        if(settings.executable.empty()){prepared.useAssociation=true;command=std::move(prepared);return true;}
        const auto executable=fs::absolute(settings.executable).lexically_normal();const auto name=Lower(executable.filename().wstring());
        static const std::set<std::wstring> shells={L"cmd.exe",L"powershell.exe",L"pwsh.exe",L"wscript.exe",L"cscript.exe",L"mshta.exe",L"rundll32.exe"};
        if(!settings.executable.is_absolute()||!fs::is_regular_file(executable)||Lower(executable.extension().wstring())!=L".exe"||shells.contains(name))
            return Fail(errorKey,detail,"Script.EditorExecutableInvalid",settings.executable);
        prepared.executable=executable;prepared.arguments=settings.arguments;
        const fs::path slnPath=FindSolutionInProjectRoot(root);
        if(prepared.arguments.empty())
        {
            if(IsVsCodeLikeEditor(name))
                prepared.arguments={L"{project}",L"--goto",L"{file}:{line}:{column}"};
            else if(IsRiderEditor(name))
                prepared.arguments=slnPath.empty()
                    ? std::vector<std::wstring>{L"{project}",L"--line",L"{line}",L"--column",L"{column}",L"{file}"}
                    : std::vector<std::wstring>{L"{sln}",L"--line",L"{line}",L"--column",L"{column}",L"{file}"};
            else if(IsVisualStudioEditor(name))
                prepared.arguments=slnPath.empty()
                    ? std::vector<std::wstring>{L"/Edit",L"{file}"}
                    : std::vector<std::wstring>{L"{sln}",L"{file}"};
            else
                prepared.arguments={L"{file}"};
        }
        if(prepared.arguments.size()>64)return Fail(errorKey,detail,"Script.EditorArgumentsInvalid");
        bool fileIncluded=false;
        const fs::path slnOrRoot=slnPath.empty()?root:slnPath;
        for(auto& argument:prepared.arguments)
        {
            if(argument.size()>16384||argument.find_first_of(L"\r\n")!=std::wstring::npos||argument.find(L'\0')!=std::wstring::npos)return Fail(errorKey,detail,"Script.EditorArgumentsInvalid");
            std::wstring expanded;
            for(std::size_t i=0;i<argument.size();)
            {
                if(argument[i]!=L'{'){expanded+=argument[i++];continue;}
                const auto close=argument.find(L'}',i);if(close==std::wstring::npos)return Fail(errorKey,detail,"Script.EditorArgumentsInvalid");
                const auto placeholder=argument.substr(i,close-i+1);
                if(placeholder==L"{file}"){expanded+=file.wstring();fileIncluded=true;}
                else if(placeholder==L"{line}")expanded+=std::to_wstring((std::max)(line,1u));
                else if(placeholder==L"{column}")expanded+=std::to_wstring((std::max)(column,1u));
                else if(placeholder==L"{project}")expanded+=root.wstring();
                else if(placeholder==L"{sln}")expanded+=slnOrRoot.wstring();
                else return Fail(errorKey,detail,"Script.EditorArgumentsInvalid");
                i=close+1;
            }
            argument=std::move(expanded);
        }
        if(!fileIncluded||BuildProcessCommandLine(executable.wstring(),prepared.arguments).size()>32760)return Fail(errorKey,detail,"Script.EditorArgumentsInvalid");
        command=std::move(prepared);return true;
    }
    catch(const std::exception& failure){errorKey="Script.EditorFileInvalid";detail=failure.what();return false;}
}

bool EditorScriptTools::OpenSource(const fs::path& projectRoot,const fs::path& file,std::uint32_t line,std::uint32_t column,
    const CodeEditorSettings& settings,std::string& errorKey,std::string& detail)
{
    const std::wstring ext=Lower(file.extension().wstring());
    if(ext==L".cs"||ext==L".csproj"||ext==L".sln")
        SyncIdeWorkspace(projectRoot,{},{},true,nullptr);

    CodeEditorSettings effective=settings;
    if(effective.executable.empty()&&(ext==L".cs"||ext==L".csproj"||ext==L".sln"))
    {
        const auto detected=DetectInstalledCodeEditor();
        if(!detected.empty())effective.executable=detected;
    }

    EditorCodeOpenCommand command;if(!PrepareOpenSource(projectRoot,file,line,column,effective,command,errorKey,detail))return false;
    if(command.useAssociation)
    {
        return StartEditorAssociatedFile(command.file,command.workingDirectory,errorKey);
    }
    return StartEditorProcess(command.executable.wstring(),command.arguments,command.workingDirectory,errorKey,true);
}
}
