#include "EditorScriptWorkflow.h"
#include "EditorApplication.h"
#include "EditorScriptTools.h"
#include "CoreLayer/Localization.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>

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

// 扫描 C# 源码（跳过注释与字符串/字符字面量），提取首个 [ScriptId("...")] 与首个顶层/命名空间内 class 声明类名区间。
void ScanCSharpMetadata(std::string_view src, std::string* outScriptId,
                        std::size_t* outClassStart, std::size_t* outClassLen)
{
    if (outScriptId) outScriptId->clear();
    if (outClassStart) *outClassStart = std::string_view::npos;
    if (outClassLen) *outClassLen = 0;

    bool foundScriptId = (outScriptId == nullptr);
    bool foundClass = (outClassStart == nullptr || outClassLen == nullptr);
    char prevPunct = '\0';

    std::size_t i = 0;
    while (i < src.size() && (!foundScriptId || !foundClass))
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

            if (!foundScriptId && (token == "ScriptId" || token == "ScriptIdAttribute"))
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
                            foundScriptId = true;
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
                        *outClassStart = nameStart;
                        *outClassLen = p - nameStart;
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
        compiledSignature_ = ComputeSourcesSignature();
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
        if (!build.is_object() || build.value("format", std::string{}) != "SelfEngine.Build") return false;
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
    SelectSource(targetPath);
    outNewSource = targetPath;
    return true;
}

void EditorScriptWorkflow::AppendRows(std::uint64_t entity, bool playing, std::vector<EditorProperty>& rows)
{
    Sources();
    if (!customClassEdited_)
    {
        for (unsigned idx = 0; idx < 1000; ++idx)
        {
            const std::string candidate = idx == 0 ? "NewBehaviour" : ("NewBehaviour" + std::to_string(idx));
            const bool taken = std::any_of(meta_.begin(), meta_.end(), [&](const ScriptSourceMeta& m) {
                return EqualsAsciiIgnoreCase(asset_io::Utf8(m.path.stem()), candidate) ||
                       EqualsAsciiIgnoreCase(m.className, candidate);
            });
            if (!taken)
            {
                className_ = candidate;
                break;
            }
        }
    }

    const std::string addSection = text_.GetText("EditorUi.AddComponent");
    std::string section = addSection;
    const auto append = [&](EditorProperty property) -> EditorProperty& {
        property.section = section;
        property.sectionInitiallyClosed = section == addSection;
        rows.push_back(std::move(property));
        return rows.back();
    };
    const auto action = [&](Json command, const std::string& label) {
        append({Key(std::move(command)), label, EditorPropertyType::Action, {}, playing});
    };
    append({"script.class", text_.GetText("Editor.ScriptClassName"), EditorPropertyType::String, className_, playing});
    if (entity != 0)
        action({{"Action", "CreateAndAttach"}}, text_.GetText("EditorUi.CreateAndAttachScript"));
    else
        action({{"Action", "Create"}}, text_.GetText("Editor.CreateBehaviour"));
    if (!selectedSource_.empty())
        action({{"Action", "OpenSelected"}},
               text_.GetText("Editor.ScriptOpenSource") + " " + asset_io::Utf8(selectedSource_.filename()));
    if (!hooks_ || !hooks_->scriptQuery || !entity) return;
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
            const std::string display = DisplayName(type);
            section = display + " (" + text_.GetText("EditorUi.ScriptComponent") + ")";
            action({{"Action", "Open"}, {"Type", type}}, text_.GetText("Editor.ScriptOpenSource") + " " + display);
            append({Key({{"Type", type}, {"Enabled", true}}), text_.GetText("Script.Enabled"),
                    EditorPropertyType::Boolean, script.value("Enabled", true) ? "true" : "false", playing})
                .sectionEnabled = true;
            if (script.value("Missing", false))
                append({Key({{"Missing", type}}), display, EditorPropertyType::String, text_.GetText("Script.TypeMissing"), true});
            for (const auto& field : script.at("Fields"))
            {
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
                        append({Key(axisCommand), name, EditorPropertyType::Number, value.at(axis).dump(), playing}).axis = *axis;
                    }
                }
                else
                {
                    const bool string = kind == "string" || kind == "entity";
                    append({Key(command), name,
                            kind == "bool" ? EditorPropertyType::Boolean
                                           : (string ? EditorPropertyType::String : EditorPropertyType::Number),
                            value.is_null() ? std::string{} : (string ? value.get<std::string>() : value.dump()), playing});
                }
            }
            action({{"Type", type}, {"Remove", true}}, text_.GetText("Script.RemoveBinding") + " " + display);
        }
        section = addSection;
        for (const auto& available : data.at("Available"))
        {
            const auto type = available.get<std::string>();
            action({{"Type", type}, {"Add", true}}, text_.GetText("Script.AddBinding") + " " + DisplayName(type));
        }
        action({{"Save", true}}, text_.GetText("Script.SaveBindings"));
        if (!undo_.empty()) action({{"Action", "Undo"}}, text_.GetText("EditorUi.Undo") + " " + text_.GetText("Script.Inspector"));
        if (!redo_.empty()) action({{"Action", "Redo"}}, text_.GetText("EditorUi.Redo") + " " + text_.GetText("Script.Inspector"));
    }
    catch (const Json::exception&)
    {
        status_ = text_.GetText("Script.MetadataFailed");
    }
}

bool EditorScriptWorkflow::Open(const std::filesystem::path& source, const CodeEditorSettings& settings)
{
    std::string error, detail;
    if (EditorScriptTools::OpenSource(root_, source, 1, 1, settings, error, detail)) return true;
    status_ = text_.GetText(error) + " " + detail;
    return false;
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
        if (document.document.kind != "bindings" || !document.dirty) continue;
        if (!hooks_->externalDocuments.save || !hooks_->externalDocuments.save(document.document.documentId, error))
        {
            status_ = text_.GetText(error.empty() ? "Script.DocumentInvalid" : error);
            return false;
        }
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
    if (playing) return false;
    status_.clear();
    if (key == "script.class")
    {
        className_ = value;
        customClassEdited_ = true;
        return true;
    }
    try
    {
        auto command = Json::parse(key.substr(7));
        if (command.contains("Action"))
        {
            const std::string act = command.at("Action").get<std::string>();
            if (act == "Undo") return Restore(false);
            if (act == "Redo") return Restore(true);
            if (act == "OpenSelected") return Open(selectedSource_, settings);
            if (act == "Create" || act == "CreateAndAttach")
            {
                EditorScriptCreateResult result;
                std::string error, detail;
                if (!EditorScriptTools::CreateBehaviour({root_, className_, "Game"}, result, error, detail))
                {
                    status_ = text_.GetText(error) + " " + detail;
                    return false;
                }
                EnsureProjectBindings(root_);
                RefreshSources();
                Sources();
                customClassEdited_ = false;
                status_ = text_.GetText("Editor.ScriptCreated") + " " + asset_io::Utf8(result.source) + " " +
                          text_.GetText("Editor.ScriptRebuildHint");
                if (act == "CreateAndAttach" && entity != 0)
                {
                    pendingAttachEntity_ = entity;
                    pendingAttachScriptId_ = result.scriptId;
                    pendingCreatedSource_ = result.source;
                    Open(result.source, settings);
                    return true;
                }
                return Open(result.source, settings);
            }
            const auto type = command.at("Type").get<std::string>();
            for (const auto& path : Sources())
            {
                std::ifstream file(path, std::ios::binary);
                const std::string source{std::istreambuf_iterator<char>(file), {}};
                if (source.find("\"" + type + "\"") != std::string::npos) return Open(path, settings);
            }
            status_ = text_.GetText("Script.TypeMissing");
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
        if (changing && !Snapshot(before)) return false;
        if (hooks_ && hooks_->scriptApply && hooks_->scriptApply(entity, command.dump(), error))
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
        status_ = text_.GetText(error.empty() ? "Script.MetadataFailed" : error);
    }
    catch (const Json::exception&)
    {
        status_ = text_.GetText("Script.FieldValueInvalid");
    }
    return false;
}
}
