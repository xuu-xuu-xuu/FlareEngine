#include "EditorScriptWorkflow.h"
#include "EditorApplication.h"
#include "EditorProcessRunner.h"
#include "EditorScriptTools.h"
#include "ProjectWorkspaceSession.h"
#include "CoreLayer/Localization.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include <Windows.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>

namespace se
{
namespace
{
using Json = nlohmann::json;
using ui::editor::EditorProperty;
using ui::editor::EditorPropertyType;

std::string Key(Json command) { return "script." + command.dump(); }

bool IsIdentStart(char ch) noexcept
{
    const auto u = static_cast<unsigned char>(ch);
    return std::isalpha(u) != 0 || ch == '_' || u >= 0x80;
}

bool IsIdentChar(char ch) noexcept
{
    const auto u = static_cast<unsigned char>(ch);
    return std::isalnum(u) != 0 || ch == '_' || u >= 0x80;
}

bool EqualsAsciiIgnoreCase(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return true;
}

bool IsCsExtension(const std::filesystem::path& p)
{
    return EqualsAsciiIgnoreCase(asset_io::Utf8(p.extension()), ".cs");
}

bool IsValidScriptFileStem(std::string_view value)
{
    if (!EditorScriptTools::IsIdentifier(value)) return false;
    if (EqualsAsciiIgnoreCase(value, "con") || EqualsAsciiIgnoreCase(value, "prn") ||
        EqualsAsciiIgnoreCase(value, "aux") || EqualsAsciiIgnoreCase(value, "nul") ||
        EqualsAsciiIgnoreCase(value, "clock$"))
    {
        return false;
    }
    if (value.size() == 4 &&
        (EqualsAsciiIgnoreCase(value.substr(0, 3), "com") || EqualsAsciiIgnoreCase(value.substr(0, 3), "lpt")) &&
        value[3] >= '1' && value[3] <= '9')
    {
        return false;
    }
    return true;
}

// 跳过空白、// 单行注释与 /* ... */ 块注释。
std::size_t SkipCSharpTrivia(std::string_view src, std::size_t i) noexcept
{
    while (i < src.size())
    {
        const unsigned char c = static_cast<unsigned char>(src[i]);
        if (std::isspace(c) != 0)
        {
            ++i;
            continue;
        }
        if (i + 1 < src.size() && src[i] == '/' && src[i + 1] == '/')
        {
            i += 2;
            while (i < src.size() && src[i] != '\n') ++i;
            continue;
        }
        if (i + 1 < src.size() && src[i] == '/' && src[i + 1] == '*')
        {
            i += 2;
            while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) ++i;
            i = (i + 1 < src.size()) ? (i + 2) : src.size();
            continue;
        }
        break;
    }
    return i;
}

// 扫描 C# 源码（跳过注释与字符串/字符字面量），提取首个 [F_Script("...")]（兼容 [ScriptId("...")]）、namespace 以及首个 class 声明类名区间。
void ScanCSharpMetadata(std::string_view src, std::string* outScriptId,
                        std::size_t* outClassStart, std::size_t* outClassLen)
{
    if (outScriptId) outScriptId->clear();
    if (outClassStart) *outClassStart = std::string_view::npos;
    if (outClassLen) *outClassLen = 0;

    bool foundExplicitId = false;
    bool foundNamespace = false;
    bool foundClass = false;
    std::string parsedNamespace;
    std::string parsedClassName;
    char prevPunct = '\0';

    std::size_t i = 0;
    while (i < src.size() && (!foundClass || (outScriptId && !foundExplicitId && false)))
    {
        i = SkipCSharpTrivia(src, i);
        if (i >= src.size()) break;

        // 跳过 UTF-8 BOM。
        if (i == 0 && src.size() >= 3 &&
            static_cast<unsigned char>(src[0]) == 0xEF &&
            static_cast<unsigned char>(src[1]) == 0xBB &&
            static_cast<unsigned char>(src[2]) == 0xBF)
        {
            i += 3;
            continue;
        }

        // 逐字字符串 @"..." / $@"..." / @$"..."
        if ((src[i] == '@' && i + 1 < src.size() && src[i + 1] == '"') ||
            ((src[i] == '$' || src[i] == '@') && i + 2 < src.size() &&
             ((src[i] == '$' && src[i + 1] == '@') || (src[i] == '@' && src[i + 1] == '$')) && src[i + 2] == '"'))
        {
            i += (src[i + 1] == '"') ? 2 : 3;
            while (i < src.size())
            {
                if (src[i] == '"')
                {
                    if (i + 1 < src.size() && src[i + 1] == '"') { i += 2; continue; }
                    ++i;
                    break;
                }
                ++i;
            }
            prevPunct = '"';
            continue;
        }

        // 普通字符串与字符字面量 "..." / '...'
        if (src[i] == '"' || src[i] == '\'')
        {
            const char quote = src[i++];
            while (i < src.size())
            {
                if (src[i] == '\\') { i = std::min(src.size(), i + 2); continue; }
                if (src[i] == quote) { ++i; break; }
                ++i;
            }
            prevPunct = quote;
            continue;
        }

        if (IsIdentStart(src[i]))
        {
            const std::size_t tokStart = i;
            while (i < src.size() && IsIdentChar(src[i])) ++i;
            const std::string_view token = src.substr(tokStart, i - tokStart);

            if (!foundNamespace && !foundClass && token == "namespace")
            {
                std::size_t p = SkipCSharpTrivia(src, i);
                std::string ns;
                while (p < src.size() && IsIdentStart(src[p]))
                {
                    const std::size_t segStart = p;
                    while (p < src.size() && IsIdentChar(src[p])) ++p;
                    if (!ns.empty()) ns.push_back('.');
                    ns.append(src.substr(segStart, p - segStart));
                    p = SkipCSharpTrivia(src, p);
                    if (p < src.size() && src[p] == '.') p = SkipCSharpTrivia(src, p + 1);
                    else break;
                }
                if (!ns.empty() && p < src.size() && (src[p] == ';' || src[p] == '{'))
                {
                    parsedNamespace = std::move(ns);
                    foundNamespace = true;
                    i = p;
                }
            }
            else if (outScriptId && !foundExplicitId &&
                     (token == "F_Script" || token == "F_ScriptAttribute" || token == "ScriptId" || token == "ScriptIdAttribute"))
            {
                std::size_t p = SkipCSharpTrivia(src, i);
                if (p < src.size() && src[p] == '(')
                {
                    p = SkipCSharpTrivia(src, p + 1);
                    if (p < src.size() && src[p] == '"')
                    {
                        ++p;
                        std::string parsed;
                        while (p < src.size() && src[p] != '"')
                        {
                            if (src[p] == '\\' && p + 1 < src.size()) ++p;
                            parsed.push_back(src[p++]);
                        }
                        if (p < src.size() && src[p] == '"' && !parsed.empty())
                        {
                            *outScriptId = std::move(parsed);
                            foundExplicitId = true;
                        }
                    }
                }
            }
            else if (!foundClass && token == "class" && prevPunct != ':')
            {
                std::size_t p = SkipCSharpTrivia(src, i);
                if (p < src.size() && src[p] == '@') ++p;
                if (p < src.size() && IsIdentStart(src[p]))
                {
                    const std::size_t nameStart = p;
                    while (p < src.size() && IsIdentChar(src[p])) ++p;
                    const std::string_view ident = src.substr(nameStart, p - nameStart);
                    if (EditorScriptTools::IsIdentifier(ident))
                    {
                        if (outClassStart) *outClassStart = nameStart;
                        if (outClassLen) *outClassLen = p - nameStart;
                        parsedClassName.assign(ident);
                        foundClass = true;
                    }
                }
            }
            prevPunct = '\0';
            continue;
        }

        prevPunct = src[i];
        ++i;
    }

    if (outScriptId && !foundExplicitId && !parsedClassName.empty())
    {
        *outScriptId = parsedNamespace.empty() ? parsedClassName : (parsedNamespace + "." + parsedClassName);
    }
}

void ParseScriptMetadata(const std::filesystem::path& path, std::string& outScriptId, std::string& outClassName)
{
    outScriptId.clear();
    outClassName = asset_io::Utf8(path.stem());
    std::ifstream file(path, std::ios::binary);
    if (!file) return;
    const std::string text{std::istreambuf_iterator<char>(file), {}};
    std::size_t clsStart = std::string_view::npos;
    std::size_t clsLen = 0;
    ScanCSharpMetadata(text, &outScriptId, &clsStart, &clsLen);
    if (clsStart != std::string_view::npos && clsLen > 0)
        outClassName.assign(text.data() + clsStart, clsLen);
}

bool SameScriptFile(const std::filesystem::path& a, const std::filesystem::path& b)
{
    if (a.empty() || b.empty()) return false;
    if (a == b) return true;
    std::error_code ec;
    return std::filesystem::equivalent(a, b, ec) && !ec;
}
} // namespace

EditorScriptWorkflow::EditorScriptWorkflow(std::filesystem::path root, EditorApplicationHooks* hooks,
                                           const Localization& text)
    : root_(std::move(root)), hooks_(hooks), text_(text)
{
}

EditorScriptWorkflow::~EditorScriptWorkflow()
{
    if (compileTask_.valid())
    {
        try { (void)compileTask_.get(); } catch (...) {}
    }
}

std::string EditorScriptWorkflow::NextSuggestedClassName()
{
    Sources();
    for (unsigned idx = 0; idx < 1000; ++idx)
    {
        const std::string candidate = idx == 0 ? "NewBehaviour" : ("NewBehaviour" + std::to_string(idx));
        const bool taken = std::any_of(meta_.begin(), meta_.end(), [&](const ScriptSourceMeta& m) {
            return EqualsAsciiIgnoreCase(asset_io::Utf8(m.path.stem()), candidate) ||
                   EqualsAsciiIgnoreCase(m.className, candidate);
        });
        if (!taken) return candidate;
    }
    return "NewBehaviour";
}

bool EditorScriptWorkflow::CanHotReloadInProcess() const noexcept
{
    if (!hooks_ || !hooks_->scriptApply) return false;
    return GetEnvironmentVariableW(L"SELFENGINE_EDITOR_SMOKE_WORKFLOW", nullptr, 0) == 0;
}

bool EditorScriptWorkflow::StartInProcessCompile(std::uint64_t attachEntity, std::string attachScriptId)
{
    if (!CanHotReloadInProcess() || compileTask_.valid()) return false;
    EnsureProjectBindings(root_);
    const std::filesystem::path projectRoot = std::filesystem::absolute(root_).lexically_normal();
    const std::filesystem::path engineRoot = ProjectWorkspaceSession::EngineRootFromExecutable();
    const std::filesystem::path hostExeDir = GetExecutableDirectory();
    const std::uint64_t seq = ++hotReloadSeq_;

    compileTask_ = std::async(std::launch::async,
        [projectRoot, engineRoot, hostExeDir, seq, attachEntity, attachScriptId = std::move(attachScriptId)]() -> AsyncBuildOutput {
            AsyncBuildOutput out;
            out.attachEntity = attachEntity;
            out.attachScriptId = attachScriptId;
            try
            {
                std::vector<std::uint8_t> buildBytes;
                if (!asset_io::Read(projectRoot / "build.json", buildBytes))
                {
                    out.errorKey = "Preview.InvalidProject";
                    return out;
                }
                const auto buildDoc = Json::parse(buildBytes.begin(), buildBytes.end());
                const std::string relProj = buildDoc.value("scriptsProject", std::string{});
                if (relProj.empty())
                {
                    out.errorKey = "Preview.InvalidProject";
                    return out;
                }
                const auto csproj = std::filesystem::absolute(projectRoot / asset_io::FromUtf8(relProj)).lexically_normal();
                if (!asset_io::Within(projectRoot, csproj) || !std::filesystem::is_regular_file(csproj))
                {
                    out.errorKey = "Preview.InvalidProject";
                    return out;
                }

                // 解析 dotnet.exe（支持 PATH 被精简但标准目录或 DOTNET_ROOT 已安装的场景）。
                std::filesystem::path dotnetExe;
                wchar_t dotnetRoot[MAX_PATH]{};
                if (GetEnvironmentVariableW(L"DOTNET_ROOT", dotnetRoot, MAX_PATH) > 0)
                {
                    const std::filesystem::path cand = std::filesystem::path(dotnetRoot) / L"dotnet.exe";
                    if (std::filesystem::is_regular_file(cand)) dotnetExe = cand;
                }
                if (dotnetExe.empty())
                {
                    wchar_t foundPath[MAX_PATH]{};
                    if (SearchPathW(nullptr, L"dotnet.exe", nullptr, MAX_PATH, foundPath, nullptr) > 0)
                        dotnetExe = foundPath;
                }
                if (dotnetExe.empty())
                {
                    const std::filesystem::path stdPath = L"C:\\Program Files\\dotnet\\dotnet.exe";
                    if (std::filesystem::is_regular_file(stdPath)) dotnetExe = stdPath;
                }
                if (dotnetExe.empty())
                {
                    out.errorKey = "Export.SdkMissing";
                    return out;
                }

                const std::filesystem::path hotRoot = projectRoot / "out" / "Editor" / "HotReload";
                const std::filesystem::path objRoot = hotRoot / "obj";
                const std::filesystem::path binDir = hotRoot / ("bin-" + std::to_string(seq));
                std::error_code ec;
                std::filesystem::create_directories(objRoot, ec);
                std::filesystem::create_directories(binDir, ec);
                out.outDir = binDir;

                // 准备 SDK 引用目录（便携包自带 SDK/；源码开发态从当前宿主目录提取 API DLL）。
                std::filesystem::path sdkRoot = engineRoot;
                if (!std::filesystem::is_regular_file(sdkRoot / "SDK" / "Managed" / "FlareEngine.Managed.dll") ||
                    !std::filesystem::is_regular_file(sdkRoot / "SDK" / "FlareEngine.Scripts.targets"))
                {
                    sdkRoot = hotRoot / "dev-sdk";
                    std::filesystem::create_directories(sdkRoot / "SDK" / "Managed", ec);
                    const auto srcManaged = hostExeDir / "FlareEngine.Managed.dll";
                    if (std::filesystem::is_regular_file(srcManaged))
                        std::filesystem::copy_file(srcManaged, sdkRoot / "SDK" / "Managed" / "FlareEngine.Managed.dll",
                                                   std::filesystem::copy_options::overwrite_existing, ec);
                    const auto srcLegacy = hostExeDir / "SelfEngine.Managed.dll";
                    if (std::filesystem::is_regular_file(srcLegacy))
                        std::filesystem::copy_file(srcLegacy, sdkRoot / "SDK" / "Managed" / "SelfEngine.Managed.dll",
                                                   std::filesystem::copy_options::overwrite_existing, ec);
                    constexpr std::string_view kValidTargets =
                        "<Project>\n"
                        "  <ItemGroup>\n"
                        "    <ProjectReference Remove=\"$(FlareEngineRoot)/Source/FunctionLayer/Scripting/Managed/Engine.Managed.csproj;$(FlareEngineRoot)\\Source\\FunctionLayer\\Scripting\\Managed\\Engine.Managed.csproj;$(SelfEngineRoot)/Source/FunctionLayer/Scripting/Managed/Engine.Managed.csproj;$(SelfEngineRoot)\\Source\\FunctionLayer\\Scripting\\Managed\\Engine.Managed.csproj\"/>\n"
                        "    <Reference Remove=\"FlareEngine.Managed;SelfEngine.Managed\"/>\n"
                        "    <Reference Include=\"FlareEngine.Managed\"><HintPath>$(MSBuildThisFileDirectory)Managed/FlareEngine.Managed.dll</HintPath><Private>false</Private></Reference>\n"
                        "    <Using Include=\"FlareEngine\"/>\n"
                        "  </ItemGroup>\n"
                        "</Project>\n";
                    asset_io::WriteAtomic(sdkRoot / "SDK" / "FlareEngine.Scripts.targets",
                        {reinterpret_cast<const std::uint8_t*>(kValidTargets.data()), kValidTargets.size()}, true);
                    asset_io::WriteAtomic(sdkRoot / "SDK" / "SelfEngine.Scripts.targets",
                        {reinterpret_cast<const std::uint8_t*>(kValidTargets.data()), kValidTargets.size()}, true);
                }

                // 隔离中间目录到 out/Editor/HotReload/obj/，保持 Scripts/ 目录干净且跨次热重载增量复用。
                const std::filesystem::path hotProps = hotRoot / "HotReload.Directory.Build.props";
                std::string escapedObj = asset_io::Utf8(objRoot);
                const std::string propsXml =
                    "<Project>\n"
                    "  <PropertyGroup><_FlareOrigProps>$([MSBuild]::GetPathOfFileAbove('Directory.Build.props', '$(MSBuildProjectDirectory)'))</_FlareOrigProps></PropertyGroup>\n"
                    "  <Import Project=\"$(_FlareOrigProps)\" Condition=\"'$(_FlareOrigProps)' != ''\"/>\n"
                    "  <PropertyGroup>\n"
                    "    <BaseIntermediateOutputPath>" + escapedObj + "\\$(MSBuildProjectName)\\</BaseIntermediateOutputPath>\n"
                    "    <MSBuildProjectExtensionsPath>" + escapedObj + "\\$(MSBuildProjectName)\\</MSBuildProjectExtensionsPath>\n"
                    "    <DefaultItemExcludes>$(DefaultItemExcludes);**\\bin\\**;**\\obj\\**</DefaultItemExcludes>\n"
                    "  </PropertyGroup>\n"
                    "</Project>\n";
                asset_io::WriteAtomic(hotProps,
                    {reinterpret_cast<const std::uint8_t*>(propsXml.data()), propsXml.size()}, true);

                const std::string projName = asset_io::Utf8(csproj.stem());
                const bool hasAssetsJson = std::filesystem::is_regular_file(objRoot / asset_io::FromUtf8(projName) / "project.assets.json");
                const std::filesystem::path nugetCfg = std::filesystem::is_regular_file(engineRoot / "NuGet.Config")
                    ? (engineRoot / "NuGet.Config") : (engineRoot / "BuildConfig" / "NuGet.Config");

                auto runBuild = [&](bool noRestore, ProcessRunResult& procOut) {
                    std::vector<std::wstring> args = {
                        L"build", csproj.wstring(),
                        L"-c", L"Release",
                        L"-o", binDir.wstring(),
                        L"--nologo",
                        L"-v:minimal",
                        L"-consoleloggerparameters:NoSummary",
                        L"-p:GenerateDependencyFile=true",
                        L"-p:SelfContained=false",
                        L"-p:UseAppHost=false",
                        L"-p:NuGetAudit=false",
                        L"-p:UseSharedCompilation=false",
                        L"-nodeReuse:false",
                        L"-p:FlareEngineRoot=" + sdkRoot.wstring(),
                        L"-p:SelfEngineRoot=" + sdkRoot.wstring(),
                        L"-p:CustomAfterMicrosoftCommonTargets=" + (sdkRoot / L"SDK" / L"FlareEngine.Scripts.targets").wstring(),
                        L"-p:DirectoryBuildPropsPath=" + hotProps.wstring(),
                        L"-p:RestorePackagesPath=" + (projectRoot / L"out" / L"nuget" / L"packages").wstring(),
                    };
                    if (noRestore) args.push_back(L"--no-restore");
                    else if (std::filesystem::is_regular_file(nugetCfg))
                    {
                        args.push_back(L"--configfile");
                        args.push_back(nugetCfg.wstring());
                    }
                    const auto logFile = binDir / (noRestore ? L"build-fast.log" : L"build.log");
                    RunEditorProcess(dotnetExe.wstring(), args, projectRoot, logFile, nullptr, procOut);
                    std::vector<std::uint8_t> logBytes;
                    if (asset_io::Read(logFile, logBytes))
                        out.logText.assign(reinterpret_cast<const char*>(logBytes.data()), logBytes.size());
                    else
                        out.logText = procOut.output;
                };

                ProcessRunResult runRes;
                runBuild(hasAssetsJson, runRes);
                if ((!runRes.started || runRes.exitCode != 0) && hasAssetsJson)
                {
                    runRes = {};
                    runBuild(false, runRes);
                }
                if (!runRes.started || runRes.exitCode != 0)
                {
                    out.errorKey = "EditorUi.ScriptCompileFailed";
                    return out;
                }

                for (const auto& entry : std::filesystem::directory_iterator(binDir, ec))
                {
                    if (!entry.is_regular_file(ec)) continue;
                    const std::string fn = asset_io::Utf8(entry.path().filename());
                    if (fn.ends_with(".deps.json"))
                    {
                        const std::string dllName = fn.substr(0, fn.size() - 10) + ".dll";
                        const auto candidateDll = binDir / asset_io::FromUtf8(dllName);
                        if (std::filesystem::is_regular_file(candidateDll, ec))
                        {
                            out.dllPath = candidateDll;
                            break;
                        }
                    }
                }
                if (out.dllPath.empty())
                {
                    const auto fallbackDll = binDir / (csproj.stem().wstring() + L".dll");
                    if (std::filesystem::is_regular_file(fallbackDll, ec)) out.dllPath = fallbackDll;
                }
                if (out.dllPath.empty())
                {
                    out.errorKey = "EditorUi.ScriptCompileFailed";
                    return out;
                }
                out.success = true;
                return out;
            }
            catch (const std::exception& e)
            {
                out.errorKey = "EditorUi.ScriptCompileFailed";
                out.logText = e.what();
                return out;
            }
        });
    status_ = text_.GetText("EditorUi.ScriptHotReloading");
    return true;
}

EditorScriptWorkflow::HotReloadResult EditorScriptWorkflow::PollInProcessCompile()
{
    HotReloadResult result;
    if (!compileTask_.valid()) return result;
    if (compileTask_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return result;

    AsyncBuildOutput built = compileTask_.get();
    result.finished = true;

    const std::string rootSlash = [&]() {
        std::string r = asset_io::Utf8(std::filesystem::absolute(root_).lexically_normal());
        for (char& ch : r) if (ch == '\\') ch = '/';
        if (!r.empty() && r.back() != '/') r.push_back('/');
        return r;
    }();
    auto normalizeLogLine = [&](std::string s) {
        std::string slash = s;
        for (char& ch : slash) if (ch == '\\') ch = '/';
        if (!rootSlash.empty())
        {
            for (;;)
            {
                auto it = std::search(slash.begin(), slash.end(), rootSlash.begin(), rootSlash.end(),
                    [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
                if (it == slash.end()) break;
                const auto pos = static_cast<std::size_t>(it - slash.begin());
                slash.erase(pos, rootSlash.size());
                s.erase(pos, rootSlash.size());
            }
        }
        for (char& ch : s) if (ch == '\\') ch = '/';
        if (const auto bracket = s.rfind(" ["); bracket != std::string::npos && s.ends_with(".csproj]"))
            s.erase(bracket);
        while (!s.empty() && s.front() == ' ') s.erase(s.begin());
        return s;
    };

    auto collectDiagnostics = [&](const std::string& logText, bool includeFallbackOnFail) {
        std::istringstream stream(logText);
        std::string line;
        std::vector<std::string> fallbackLines;
        std::size_t matchedErrors = 0;
        while (std::getline(stream, line))
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty()) continue;
            if (line.find(": error ") != std::string::npos || line.find(" error CS") != std::string::npos ||
                line.find("error MSB") != std::string::npos || line.find("error NETSDK") != std::string::npos)
            {
                const std::string cleaned = normalizeLogLine(line);
                if (result.consoleLines.empty() || result.consoleLines.back() != "[Error] " + cleaned)
                    result.consoleLines.push_back("[Error] " + cleaned);
                ++matchedErrors;
            }
            else if (line.find(": warning ") != std::string::npos)
            {
                const std::string cleaned = normalizeLogLine(line);
                if (result.consoleLines.empty() || result.consoleLines.back() != "[Warning] " + cleaned)
                    result.consoleLines.push_back("[Warning] " + cleaned);
            }
            else if (includeFallbackOnFail && fallbackLines.size() < 6)
            {
                fallbackLines.push_back(normalizeLogLine(line));
            }
        }
        if (includeFallbackOnFail && matchedErrors == 0)
        {
            for (const auto& fb : fallbackLines)
                result.consoleLines.push_back("[Error] " + fb);
        }
    };

    if (!built.success)
    {
        hasCompileError_ = true;
        collectDiagnostics(built.logText, true);
        result.statusMessage = text_.GetText(built.errorKey.empty() ? "EditorUi.ScriptCompileFailed" : built.errorKey);
        status_ = result.statusMessage;
        if (result.consoleLines.empty())
            result.consoleLines.push_back("[Error] " + result.statusMessage);
        return result;
    }

    collectDiagnostics(built.logText, false);

    RecoveryDocument before;
    const bool hasBefore = built.attachEntity != 0 && !built.attachScriptId.empty() && Snapshot(before);
    std::string applyErr;
    Json reloadCmd{{"ReloadAssembly", asset_io::Utf8(built.dllPath)}};
    if (built.attachEntity != 0 && !built.attachScriptId.empty())
        reloadCmd["AttachScript"] = built.attachScriptId;

    if (!hooks_ || !hooks_->scriptApply || !hooks_->scriptApply(built.attachEntity, reloadCmd.dump(), applyErr))
    {
        hasCompileError_ = true;
        result.statusMessage = text_.GetText(applyErr.empty() ? "Script.MetadataFailed" : applyErr);
        status_ = result.statusMessage;
        result.consoleLines.push_back("[Error] " + result.statusMessage);
        return result;
    }

    hasCompileError_ = false;
    if (hasBefore)
    {
        RecoveryDocument after;
        if (Snapshot(after) && before.content != after.content)
        {
            undo_.push_back({std::move(before), std::move(after)});
            redo_.clear();
            if (undo_.size() > 100) undo_.erase(undo_.begin());
        }
    }

    Save();
    MarkSourcesCompiled();

    // 清理旧的 bin-* 热重载输出目录（流式加载无文件锁，仅保留最近 2 次便于排障）。
    std::error_code ec;
    const auto hotRoot = std::filesystem::absolute(root_).lexically_normal() / "out" / "Editor" / "HotReload";
    if (std::filesystem::is_directory(hotRoot, ec))
    {
        std::vector<std::filesystem::path> bins;
        for (const auto& entry : std::filesystem::directory_iterator(hotRoot, ec))
        {
            if (entry.is_directory(ec) && asset_io::Utf8(entry.path().filename()).starts_with("bin-"))
                bins.push_back(entry.path());
        }
        std::sort(bins.begin(), bins.end());
        if (bins.size() > 2)
        {
            for (std::size_t i = 0; i + 2 < bins.size(); ++i)
            {
                if (asset_io::Within(hotRoot, bins[i]))
                    std::filesystem::remove_all(bins[i], ec);
            }
        }
    }

    result.success = true;
    if (built.attachEntity != 0 && !built.attachScriptId.empty())
    {
        result.attachedEntity = built.attachEntity;
        result.attachedDisplayName = DisplayName(built.attachScriptId);
        result.statusMessage = text_.GetText("EditorUi.ScriptAttached") + ": " + result.attachedDisplayName;
    }
    else
    {
        result.statusMessage = text_.GetText("EditorUi.ScriptHotReloaded");
    }
    status_ = result.statusMessage;
    result.consoleLines.push_back("[Info] " + result.statusMessage);
    return result;
}

const std::vector<std::filesystem::path>& EditorScriptWorkflow::Sources()
{
    const auto now = std::chrono::steady_clock::now();
    if (now - scanned_ < std::chrono::seconds(1)) return sources_;
    scanned_ = now;
    sources_.clear();
    meta_.clear();
    std::error_code error;
    std::filesystem::recursive_directory_iterator cursor(root_ / "Scripts", error), end;
    for (; !error && cursor != end; cursor.increment(error))
    {
        if (cursor->is_symlink(error))
        {
            cursor.disable_recursion_pending();
            continue;
        }
        if (cursor->is_directory(error))
        {
            if (cursor->path().filename() == "bin" || cursor->path().filename() == "obj")
                cursor.disable_recursion_pending();
            continue;
        }
        if (IsCsExtension(cursor->path()))
        {
            sources_.push_back(cursor->path());
            ScriptSourceMeta item;
            item.path = cursor->path();
            std::error_code timeEc;
            item.modified = std::filesystem::last_write_time(item.path, timeEc);
            ParseScriptMetadata(item.path, item.scriptId, item.className);
            meta_.push_back(std::move(item));
        }
    }
    std::sort(sources_.begin(), sources_.end());
    std::sort(meta_.begin(), meta_.end(), [](const ScriptSourceMeta& a, const ScriptSourceMeta& b) {
        return a.path < b.path;
    });
    if (!compiledSignatureInitialized_)
    {
        const bool uncompiledColdStart = !sources_.empty() &&
            GetEnvironmentVariableW(L"FLARE_EDITOR_UNCOMPILED_SCRIPTS", nullptr, 0) > 0;
        compiledSignature_ = uncompiledColdStart ? std::string("<uncompiled>") : ComputeSourcesSignature();
        compiledSignatureInitialized_ = true;
    }
    return sources_;
}

std::string EditorScriptWorkflow::ComputeSourcesSignature()
{
    std::string sig;
    for (const auto& m : meta_)
    {
        sig += asset_io::Utf8(m.path.lexically_relative(root_));
        sig.push_back('|');
        sig += std::to_string(m.modified.time_since_epoch().count());
        sig.push_back('\n');
    }
    return sig;
}

bool EditorScriptWorkflow::SourcesNeedCompile()
{
    Sources();
    if (!sources_.empty() && (!hooks_ || !hooks_->scriptQuery)) return true;
    return ComputeSourcesSignature() != compiledSignature_;
}

void EditorScriptWorkflow::MarkSourcesCompiled()
{
    SetEnvironmentVariableW(L"FLARE_EDITOR_UNCOMPILED_SCRIPTS", nullptr);
    RefreshSources();
    Sources();
    compiledSignature_ = ComputeSourcesSignature();
}

std::string EditorScriptWorkflow::DisplayName(std::string_view scriptId)
{
    Sources();
    for (const auto& m : meta_)
    {
        if (!m.scriptId.empty() && m.scriptId == scriptId && !m.className.empty())
            return m.className;
    }
    return std::string(scriptId);
}

std::string EditorScriptWorkflow::ScriptIdForSource(const std::filesystem::path& sourcePath)
{
    Sources();
    for (const auto& m : meta_)
    {
        if (SameScriptFile(m.path, sourcePath)) return m.scriptId;
    }
    std::string id, cls;
    ParseScriptMetadata(sourcePath, id, cls);
    return id;
}

std::string EditorScriptWorkflow::ClassNameForSource(const std::filesystem::path& sourcePath)
{
    Sources();
    for (const auto& m : meta_)
    {
        if (SameScriptFile(m.path, sourcePath)) return m.className;
    }
    std::string id, cls;
    ParseScriptMetadata(sourcePath, id, cls);
    return cls;
}

bool EditorScriptWorkflow::EnsureProjectBindings(const std::filesystem::path& projectRoot)
{
    try
    {
        if (projectRoot.empty()) return false;
        const auto rootNorm = std::filesystem::absolute(projectRoot).lexically_normal();
        const auto buildPath = rootNorm / "build.json";
        std::vector<std::uint8_t> bytes;
        if (!asset_io::Read(buildPath, bytes)) return false;
        auto build = Json::parse(bytes.begin(), bytes.end());
        const auto format = build.is_object() ? build.value("format", std::string{}) : std::string{};
        if (format != "FlareEngine.Build" && format != "SelfEngine.Build") return false;
        if (build.value("scriptsProject", std::string{}).empty()) return false;

        std::string bindingsRel = build.value("bindings", std::string{});
        const bool wroteDefaultRel = bindingsRel.empty();
        if (wroteDefaultRel) bindingsRel = "Scripts/Main.bindings.json";

        const auto bindingsSubpath = asset_io::FromUtf8(bindingsRel);
        if (bindingsSubpath.empty() || bindingsSubpath.is_absolute()) return false;
        const auto bindingsPath = std::filesystem::absolute(rootNorm / bindingsSubpath).lexically_normal();
        if (!asset_io::Within(rootNorm, bindingsPath)) return false;

        if (wroteDefaultRel)
        {
            build["bindings"] = bindingsRel;
            const std::string dumped = build.dump(2) + "\n";
            if (!asset_io::WriteAtomic(buildPath,
                    {reinterpret_cast<const std::uint8_t*>(dumped.data()), dumped.size()}, true))
            {
                return false;
            }
        }
        std::error_code ec;
        if (!std::filesystem::exists(bindingsPath, ec))
        {
            std::filesystem::create_directories(bindingsPath.parent_path(), ec);
            if (ec) return false;
            constexpr std::string_view emptyBindings = "{\n  \"Version\": 1,\n  \"Scripts\": []\n}\n";
            if (!asset_io::WriteAtomic(bindingsPath,
                    {reinterpret_cast<const std::uint8_t*>(emptyBindings.data()), emptyBindings.size()}, false))
            {
                return false;
            }
        }
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void EditorScriptWorkflow::QueuePendingAttach(const std::filesystem::path& crashDir, std::uint64_t entity,
                                              std::string_view scriptId)
{
    if (crashDir.empty() || entity == 0 || scriptId.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(crashDir, ec);
    const Json payload{{"entity", entity}, {"scriptId", std::string(scriptId)}};
    const std::string text = payload.dump(2) + "\n";
    asset_io::WriteAtomic(crashDir / ".pending-script-attach.json",
                          {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}, true);
}

bool EditorScriptWorkflow::ConsumePendingAttachOnStartup(const std::filesystem::path& crashDir,
                                                         std::uint64_t& outEntity,
                                                         std::string& outDisplayName)
{
    outEntity = 0;
    outDisplayName.clear();
    if (crashDir.empty()) return false;
    const auto file = crashDir / ".pending-script-attach.json";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) return false;
    std::vector<std::uint8_t> bytes;
    const bool readOk = asset_io::Read(file, bytes);
    std::filesystem::remove(file, ec);
    if (!readOk) return false;
    try
    {
        const auto payload = Json::parse(bytes.begin(), bytes.end());
        const std::uint64_t entity = payload.value("entity", std::uint64_t{0});
        const std::string scriptId = payload.value("scriptId", std::string{});
        bool needsCompile = false;
        if (entity != 0 && !scriptId.empty() && AttachScript(entity, scriptId, needsCompile))
        {
            Save();
            outEntity = entity;
            outDisplayName = DisplayName(scriptId);
            return true;
        }
    }
    catch (...)
    {
    }
    return false;
}

bool EditorScriptWorkflow::ConsumeCreatedPendingAttach(std::uint64_t& outEntity, std::string& outScriptId,
                                                       std::filesystem::path& outSource)
{
    if (pendingAttachEntity_ == 0 || pendingAttachScriptId_.empty()) return false;
    outEntity = pendingAttachEntity_;
    outScriptId = std::move(pendingAttachScriptId_);
    outSource = std::move(pendingCreatedSource_);
    pendingAttachEntity_ = 0;
    pendingAttachScriptId_.clear();
    pendingCreatedSource_.clear();
    return true;
}

bool EditorScriptWorkflow::AttachScript(std::uint64_t entity, const std::string& scriptId, bool& outNeedsCompile)
{
    outNeedsCompile = false;
    status_.clear();
    if (entity == 0 || scriptId.empty()) return false;
    if (!hooks_ || !hooks_->scriptApply)
    {
        outNeedsCompile = true;
        return false;
    }
    if (hooks_->scriptQuery)
    {
        std::string queryDoc, queryErr;
        if (hooks_->scriptQuery(entity, queryDoc, queryErr))
        {
            try
            {
                const auto parsed = Json::parse(queryDoc);
                bool foundInLoaded = false;
                if (parsed.contains("Available") && parsed["Available"].is_array())
                {
                    for (const auto& avail : parsed["Available"])
                        if (avail.is_string() && avail.get<std::string>() == scriptId) { foundInLoaded = true; break; }
                }
                if (!foundInLoaded)
                {
                    outNeedsCompile = true;
                    return false;
                }
            }
            catch (...) {}
        }
    }
    RecoveryDocument before;
    const bool hasBefore = Snapshot(before);
    std::string error;
    const Json cmd{{"Type", scriptId}, {"Add", true}};
    if (hooks_->scriptApply(entity, cmd.dump(), error))
    {
        RecoveryDocument after;
        if (hasBefore && Snapshot(after) && before.content != after.content)
        {
            undo_.push_back({std::move(before), std::move(after)});
            redo_.clear();
            if (undo_.size() > 100) undo_.erase(undo_.begin());
        }
        return true;
    }
    if (error == "Script.TypeMissing")
    {
        outNeedsCompile = true;
        return false;
    }
    status_ = text_.GetText(error.empty() ? "Script.MetadataFailed" : error);
    return false;
}

bool EditorScriptWorkflow::RenameScriptSource(const std::filesystem::path& oldSource, std::string_view newClassName,
                                              std::filesystem::path& outNewSource)
{
    outNewSource.clear();
    status_.clear();
    if (!IsValidScriptFileStem(newClassName))
    {
        status_ = text_.GetText("Script.TemplateIdentifierInvalid");
        return false;
    }
    const auto scriptsDir = std::filesystem::absolute(root_ / "Scripts").lexically_normal();
    const auto normalizedOld = std::filesystem::absolute(oldSource).lexically_normal();
    if (!asset_io::Within(scriptsDir, normalizedOld) || !std::filesystem::is_regular_file(normalizedOld))
    {
        status_ = text_.GetText("Script.EditorFileInvalid");
        return false;
    }
    const auto targetPath = normalizedOld.parent_path() / asset_io::FromUtf8(std::string(newClassName) + ".cs");
    const bool sameDiskFile = SameScriptFile(normalizedOld, targetPath);
    const bool exactFileNameMatch = (normalizedOld.filename().wstring() == targetPath.filename().wstring());
    std::error_code ec;
    if (!sameDiskFile && std::filesystem::exists(targetPath, ec))
    {
        status_ = text_.GetText("Script.TemplateConflict");
        return false;
    }
    std::vector<std::uint8_t> bytes;
    if (!asset_io::Read(normalizedOld, bytes))
    {
        status_ = text_.GetText("Script.TemplateWriteFailed");
        return false;
    }
    const std::string oldScriptId = ScriptIdForSource(normalizedOld);
    std::string content(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::size_t clsStart = std::string_view::npos;
    std::size_t clsLen = 0;
    ScanCSharpMetadata(content, nullptr, &clsStart, &clsLen);
    if (clsStart == std::string_view::npos || clsLen == 0)
    {
        status_ = text_.GetText("Script.TemplateIdentifierInvalid");
        return false;
    }
    const std::string_view currentClass(content.data() + clsStart, clsLen);
    if (sameDiskFile && exactFileNameMatch && currentClass == newClassName)
    {
        outNewSource = normalizedOld;
        return true;
    }
    content.replace(clsStart, clsLen, newClassName);

    if (sameDiskFile)
    {
        if (!asset_io::WriteAtomic(normalizedOld,
                {reinterpret_cast<const std::uint8_t*>(content.data()), content.size()}, true))
        {
            status_ = text_.GetText("Script.TemplateWriteFailed");
            return false;
        }
        if (!exactFileNameMatch)
        {
            const auto tempPath = normalizedOld.parent_path() /
                                  (normalizedOld.filename().wstring() + L".renametmp-" +
                                   asset_io::FromUtf8(asset_io::NewId()).wstring());
            std::filesystem::rename(normalizedOld, tempPath, ec);
            if (ec)
            {
                status_ = text_.GetText("Script.TemplateWriteFailed");
                return false;
            }
            std::filesystem::rename(tempPath, targetPath, ec);
            if (ec)
            {
                std::error_code rollbackEc;
                std::filesystem::rename(tempPath, normalizedOld, rollbackEc);
                status_ = text_.GetText("Script.TemplateWriteFailed");
                return false;
            }
        }
    }
    else
    {
        if (!asset_io::WriteAtomic(targetPath,
                {reinterpret_cast<const std::uint8_t*>(content.data()), content.size()}, false))
        {
            status_ = text_.GetText("Script.TemplateWriteFailed");
            return false;
        }
        if (!std::filesystem::remove(normalizedOld, ec) || ec)
        {
            std::error_code rollbackEc;
            std::filesystem::remove(targetPath, rollbackEc);
            status_ = text_.GetText("Script.TemplateWriteFailed");
            return false;
        }
    }
    RefreshSources();
    Sources();
    const std::string newScriptId = ScriptIdForSource(targetPath);
    if (!oldScriptId.empty() && !newScriptId.empty() && oldScriptId != newScriptId && hooks_ && hooks_->scriptApply)
    {
        std::string ignoredErr;
        const Json renameCmd{{"RenameFrom", oldScriptId}, {"RenameTo", newScriptId}};
        (void)hooks_->scriptApply(0, renameCmd.dump(), ignoredErr);
    }
    SelectSource(targetPath);
    outNewSource = targetPath;
    return true;
}

void EditorScriptWorkflow::AppendRows(std::uint64_t entity, bool playing, std::vector<EditorProperty>& rows,
                                      const std::function<std::string(std::uint64_t)>& resolveEntityName)
{
    Sources();
    if (!customClassEdited_)
    {
        className_ = NextSuggestedClassName();
    }

    const std::string addSection = text_.GetText("EditorUi.AddComponent");
    std::string section = addSection;
    const auto append = [&](EditorProperty property) -> EditorProperty& {
        property.section = section;
        property.sectionInitiallyClosed = section == addSection;
        rows.push_back(std::move(property));
        return rows.back();
    };
    const auto action = [&](Json command, const std::string& label, bool menuAction = false) -> EditorProperty& {
        auto& item = append({Key(std::move(command)), label, EditorPropertyType::Action, {}, playing});
        item.headerMenuAction = menuAction;
        return item;
    };
    if (entity == 0)
    {
        append({"script.class", text_.GetText("Editor.ScriptClassName"), EditorPropertyType::String, className_, playing});
        action({{"Action", "Create"}}, text_.GetText("Editor.CreateBehaviour"));
        if (!selectedSource_.empty())
            action({{"Action", "OpenSelected"}},
                   text_.GetText("Editor.ScriptOpenSource") + " " + asset_io::Utf8(selectedSource_.filename())).readOnly = false;
        return;
    }
    if (!hooks_ || !hooks_->scriptQuery) return;
    std::string document, error;
    if (!hooks_->scriptQuery(entity, document, error))
    {
        section = text_.GetText("Script.Inspector");
        append({"script.error", text_.GetText("Script.Inspector"), EditorPropertyType::String, text_.GetText(error), true});
        return;
    }
    try
    {
        const auto data = Json::parse(document);
        for (const auto& script : data.at("Scripts"))
        {
            const auto type = script.at("Type").get<std::string>();
            const std::string metaCls = script.value("ClassName", std::string{});
            const std::string display = !metaCls.empty() ? metaCls : DisplayName(type);
            section = display + " (" + text_.GetText("EditorUi.ScriptComponent") + ")";
            action({{"Action", "Open"}, {"Type", type}}, text_.GetText("EditorUi.ComponentEditScript") + " (" + display + ")", true).readOnly = false;
            action({{"Type", type}, {"Remove", true}}, text_.GetText("EditorUi.ComponentRemove"), true);
            append({Key({{"Type", type}, {"Enabled", true}}), text_.GetText("Script.Enabled"),
                    EditorPropertyType::Boolean, script.value("Enabled", true) ? "true" : "false", false})
                .sectionEnabled = true;
            if (script.value("Missing", false))
                append({Key({{"Missing", type}}), display, EditorPropertyType::String, text_.GetText("Script.TypeMissing"), true});
            for (const auto& field : script.at("Fields"))
            {
                if (field.contains("Header") && field["Header"].is_string())
                {
                    const std::string hdr = field["Header"].get<std::string>();
                    if (!hdr.empty())
                        append({"script.header." + type + "." + field.at("Name").get<std::string>(),
                                hdr, EditorPropertyType::Subheader, {}, true});
                }
                const auto name = field.at("Name").get<std::string>(), kind = field.at("Kind").get<std::string>();
                const auto value = field.at("Value");
                const Json command = {{"Type", type}, {"Field", name}, {"Kind", kind}};
                if (kind == "vec3")
                {
                    for (const auto* axis : {"X", "Y", "Z"})
                    {
                        auto axisCommand = command;
                        axisCommand["Axis"] = axis;
                        axisCommand["Value"] = value;
                        auto& row = append({Key(axisCommand), name, EditorPropertyType::Number, value.at(axis).dump(), false});
                        row.axis = *axis;
                        row.dragStep = 0.05f;
                    }
                }
                else if (kind == "entity")
                {
                    const std::string rawId = value.is_null() ? "0" : (value.is_string() ? value.get<std::string>() : value.dump());
                    const std::uint64_t refId = static_cast<std::uint64_t>(std::strtoull(rawId.c_str(), nullptr, 10));
                    auto& row = append({Key(command), name, EditorPropertyType::EntityRef, rawId, false});
                    if (refId == 0) row.displayValue = text_.GetText("EditorUi.NoEntityRef");
                    else
                    {
                        const std::string resolved = resolveEntityName ? resolveEntityName(refId) : std::string{};
                        row.displayValue = !resolved.empty()
                            ? (resolved + " (#" + std::to_string(refId) + ")")
                            : (text_.GetText("EditorUi.MissingEntityRef") + " (#" + std::to_string(refId) + ")");
                    }
                }
                else
                {
                    const bool isStr = (kind == "string");
                    const EditorPropertyType propType = kind == "bool" ? EditorPropertyType::Boolean
                                                      : (isStr ? EditorPropertyType::String : EditorPropertyType::Number);
                    auto& row = append({Key(command), name, propType,
                                        value.is_null() ? std::string{} : (isStr ? value.get<std::string>() : value.dump()), false});
                    if (propType == EditorPropertyType::Number)
                    {
                        row.dragStep = (kind == "i32") ? 1.0f : 0.05f;
                        if (field.contains("Min") && field["Min"].is_number() &&
                            field.contains("Max") && field["Max"].is_number())
                        {
                            const float lo = field["Min"].get<float>();
                            const float hi = field["Max"].get<float>();
                            row.dragMinimum = lo;
                            row.dragMaximum = hi;
                            if (hi > lo)
                            {
                                row.slider = true;
                                if (kind != "i32") row.dragStep = std::max(0.001f, (hi - lo) * 0.005f);
                            }
                        }
                    }
                }
            }
        }
        section = addSection;
        std::vector<std::string> knownTypes;
        for (const auto& script : data.at("Scripts"))
        {
            if (script.contains("Type") && script["Type"].is_string())
                knownTypes.push_back(script["Type"].get<std::string>());
        }
        const Json availNames = (data.contains("AvailableNames") && data["AvailableNames"].is_object())
            ? data["AvailableNames"] : Json::object();
        for (const auto& available : data.at("Available"))
        {
            const auto type = available.get<std::string>();
            knownTypes.push_back(type);
            std::string labelName = availNames.value(type, std::string{});
            if (labelName.empty()) labelName = DisplayName(type);
            action({{"Type", type}, {"Add", true}}, labelName + " (C#)");
        }
        for (const auto& m : meta_)
        {
            if (m.scriptId.empty()) continue;
            if (std::find(knownTypes.begin(), knownTypes.end(), m.scriptId) != knownTypes.end()) continue;
            knownTypes.push_back(m.scriptId);
            action({{"Type", m.scriptId}, {"Add", true}},
                   (m.className.empty() ? m.scriptId : m.className) + " (C#)");
        }
    }
    catch (const Json::exception&)
    {
        status_ = text_.GetText("Script.MetadataFailed");
    }
}

bool EditorScriptWorkflow::Open(const std::filesystem::path& source, std::uint32_t line, std::uint32_t column,
                                const CodeEditorSettings& settings)
{
    std::string error, detail;
    if (EditorScriptTools::OpenSource(root_, source, std::max(1u, line), std::max(1u, column), settings, error, detail)) return true;
    status_ = text_.GetText(error) + " " + detail;
    return false;
}

bool EditorScriptWorkflow::OpenSourceAtLine(const std::filesystem::path& sourceOrRelative, std::uint32_t line,
                                            std::uint32_t column, const CodeEditorSettings& settings)
{
    std::error_code ec;
    std::filesystem::path target = sourceOrRelative;
    if (!target.is_absolute())
    {
        const auto inProject = root_ / target;
        const auto inScripts = root_ / "Scripts" / target;
        if (std::filesystem::is_regular_file(inProject, ec)) target = inProject;
        else if (std::filesystem::is_regular_file(inScripts, ec)) target = inScripts;
        else
        {
            Sources();
            for (const auto& m : meta_)
            {
                if (m.path.filename() == target.filename()) { target = m.path; break; }
            }
        }
    }
    return Open(target, line, column, settings);
}

bool EditorScriptWorkflow::Snapshot(RecoveryDocument& document) const
{
    if (!hooks_ || !hooks_->externalDocuments.snapshot) return false;
    std::vector<EditorExternalDocument> documents;
    std::string error;
    if (!hooks_->externalDocuments.snapshot(documents, error)) return false;
    for (const auto& entry : documents)
    {
        if (entry.document.kind == "bindings")
        {
            document = entry.document;
            return true;
        }
    }
    return false;
}

bool EditorScriptWorkflow::Dirty() const
{
    if (!hooks_ || !hooks_->externalDocuments.snapshot) return false;
    std::vector<EditorExternalDocument> documents;
    std::string error;
    if (!hooks_->externalDocuments.snapshot(documents, error)) return false;
    for (const auto& entry : documents)
    {
        if (entry.document.kind == "bindings" && entry.dirty) return true;
    }
    return false;
}

bool EditorScriptWorkflow::Save()
{
    if (!hooks_ || !hooks_->externalDocuments.snapshot) return true;
    std::vector<EditorExternalDocument> documents;
    std::string error;
    if (!hooks_->externalDocuments.snapshot(documents, error))
    {
        status_ = text_.GetText(error);
        return false;
    }
    for (const auto& document : documents)
    {
        if (document.document.kind != "bindings") continue;
        if (document.dirty)
        {
            if (!hooks_->externalDocuments.save || !hooks_->externalDocuments.save(document.document.documentId, error))
            {
                status_ = text_.GetText(error.empty() ? "Script.DocumentInvalid" : error);
                return false;
            }
        }
        try
        {
            const auto bindDoc = Json::parse(document.document.content);
            if (bindDoc.contains("Scripts") && bindDoc["Scripts"].is_array() && !bindDoc["Scripts"].empty())
            {
                const std::string firstBound = bindDoc["Scripts"][0].value("Type", std::string{});
                std::vector<std::uint8_t> buildBytes;
                const auto buildPath = root_ / "build.json";
                if (!firstBound.empty() && asset_io::Read(buildPath, buildBytes))
                {
                    auto buildJson = Json::parse(buildBytes.begin(), buildBytes.end());
                    const std::string curType = buildJson.value("scriptType", std::string{});
                    bool curFound = false;
                    for (const auto& s : bindDoc["Scripts"])
                        if (s.value("Type", std::string{}) == curType) { curFound = true; break; }
                    if (!curFound)
                    {
                        buildJson["scriptType"] = firstBound;
                        const std::string dumped = buildJson.dump(2) + "\n";
                        asset_io::WriteAtomic(buildPath, {reinterpret_cast<const std::uint8_t*>(dumped.data()), dumped.size()}, true);
                    }
                }
            }
        }
        catch (...) {}
    }
    return true;
}

bool EditorScriptWorkflow::Restore(bool redo)
{
    auto& from = redo ? redo_ : undo_;
    auto& to = redo ? undo_ : redo_;
    if (from.empty() || !hooks_ || !hooks_->externalDocuments.restore) return false;
    RecoveryDocument current;
    if (!Snapshot(current)) return false;
    const auto& change = from.back();
    const auto& expected = redo ? change.before : change.after;
    if (current.content != expected.content)
    {
        undo_.clear();
        redo_.clear();
        status_ = text_.GetText("Script.DocumentExternalChanged");
        return false;
    }
    auto target = redo ? change.after : change.before;
    target.sourceDigest = current.sourceDigest;
    std::string error;
    if (!hooks_->externalDocuments.restore(target, true, error))
    {
        status_ = text_.GetText(error);
        return false;
    }
    to.push_back(change);
    from.pop_back();
    return true;
}

bool EditorScriptWorkflow::Apply(std::uint64_t entity, bool playing, const std::string& key, const std::string& value,
                                 const CodeEditorSettings& settings)
{
    status_.clear();
    if (key == "script.class")
    {
        if (playing) return false;
        className_ = value;
        customClassEdited_ = true;
        return true;
    }
    if (!key.starts_with("script.")) return false;
    try
    {
        auto command = Json::parse(key.substr(7));
        if (command.contains("Action"))
        {
            const std::string act = command.at("Action").get<std::string>();
            if (act == "Open")
            {
                const auto type = command.at("Type").get<std::string>();
                Sources();
                for (const auto& m : meta_)
                {
                    if (m.scriptId == type || m.className == type) return Open(m.path, 1, 1, settings);
                }
                for (const auto& path : sources_)
                {
                    std::ifstream file(path, std::ios::binary);
                    const std::string source{std::istreambuf_iterator<char>(file), {}};
                    if (source.find("\"" + type + "\"") != std::string::npos) return Open(path, 1, 1, settings);
                }
                status_ = text_.GetText("Script.TypeMissing");
                return false;
            }
            if (act == "OpenSelected") return Open(selectedSource_, 1, 1, settings);
            if (playing) return false;
            if (act == "Undo") return Restore(false);
            if (act == "Redo") return Restore(true);
            if (act == "Create" || act == "CreateAndAttach")
            {
                const std::string requestedName = command.contains("Class") && command["Class"].is_string() &&
                                                  !command["Class"].get<std::string>().empty()
                    ? command["Class"].get<std::string>()
                    : className_;
                EditorScriptCreateResult result;
                std::string error, detail;
                if (!EditorScriptTools::CreateBehaviour({root_, requestedName, ""}, result, error, detail))
                {
                    status_ = text_.GetText(error) + " " + detail;
                    return false;
                }
                EnsureProjectBindings(root_);
                RefreshSources();
                Sources();
                customClassEdited_ = false;
                status_ = text_.GetText("Editor.ScriptCreated") + " " + asset_io::Utf8(result.source);
                if (act == "CreateAndAttach" && entity != 0)
                {
                    pendingAttachEntity_ = entity;
                    pendingAttachScriptId_ = result.scriptId;
                    pendingCreatedSource_ = result.source;
                    return true;
                }
                return Open(result.source, 1, 1, settings);
            }
            return false;
        }
        if (playing)
        {
            if (command.contains("Add") || command.contains("Remove") || command.contains("Save")) return false;
            if (!command.contains("Kind") && !command.contains("Enabled")) return false;
            if (command.contains("Enabled")) command["Enabled"] = value == "true";
            if (command.contains("Kind"))
            {
                const auto kind = command.at("Kind").get<std::string>();
                if (kind == "string" || kind == "entity") command["Value"] = value;
                else if (command.contains("Axis")) command["Value"][command.at("Axis").get<std::string>()] = Json::parse(value);
                else command["Value"] = Json::parse(value);
                command.erase("Kind");
                command.erase("Axis");
            }
            command["Live"] = true;
            std::string error;
            if (hooks_ && hooks_->scriptApply && hooks_->scriptApply(entity, command.dump(), error)) return true;
            status_ = text_.GetText(error.empty() ? "Script.MetadataFailed" : error);
            return false;
        }
        if (command.contains("Enabled")) command["Enabled"] = value == "true";
        if (command.contains("Kind"))
        {
            const auto kind = command.at("Kind").get<std::string>();
            if (kind == "string" || kind == "entity") command["Value"] = value;
            else if (command.contains("Axis")) command["Value"][command.at("Axis").get<std::string>()] = Json::parse(value);
            else command["Value"] = Json::parse(value);
            command.erase("Kind");
            command.erase("Axis");
        }
        std::string error;
        RecoveryDocument before;
        const bool changing = !command.contains("Save");
        const bool haveSnapshot = changing ? Snapshot(before) : true;
        if (haveSnapshot && hooks_ && hooks_->scriptApply && hooks_->scriptApply(entity, command.dump(), error))
        {
            RecoveryDocument after;
            if (changing && Snapshot(after) && before.content != after.content)
            {
                undo_.push_back({std::move(before), std::move(after)});
                redo_.clear();
                if (undo_.size() > 100) undo_.erase(undo_.begin());
            }
            return true;
        }
        if (command.contains("Add") && command.contains("Type") && command["Type"].is_string() && entity != 0)
        {
            const std::string reqType = command["Type"].get<std::string>();
            Sources();
            for (const auto& m : meta_)
            {
                if (m.scriptId == reqType)
                {
                    pendingAttachEntity_ = entity;
                    pendingAttachScriptId_ = reqType;
                    pendingCreatedSource_ = m.path;
                    return true;
                }
            }
        }
        status_ = text_.GetText(error.empty() ? "Script.MetadataFailed" : error);
    }
    catch (const Json::exception&)
    {
        status_ = text_.GetText("Script.FieldValueInvalid");
    }
    return false;
}
}
