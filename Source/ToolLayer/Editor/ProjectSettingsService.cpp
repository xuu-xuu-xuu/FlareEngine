#include "ProjectSettingsService.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iterator>
#include <set>

namespace se
{
namespace
{
using Json=nlohmann::json;
bool ValidVideo(const BuildSettings& settings)
{
    const auto& size=settings.videoResolution;
    return (size=="640x360"||size=="1280x720"||size=="1920x1080")
        &&(settings.videoFps==24||settings.videoFps==30||settings.videoFps==60)
        &&settings.videoSeconds>=1&&settings.videoSeconds<=100000/settings.videoFps;
}
bool WriteAtomic(const std::filesystem::path& path,const std::string& text)
{
    return asset_io::WriteAtomic(path,{reinterpret_cast<const std::uint8_t*>(text.data()),text.size()});
}
bool ReadText(const std::filesystem::path& path,std::string& output,std::string& errorKey)
{
    std::error_code error;
    if(std::filesystem::file_size(path,error)>4*1024*1024||error){errorKey="Project.ReadFailed";return false;}
    std::vector<std::uint8_t> bytes;
    if(!asset_io::Read(path,bytes)){errorKey="Project.ReadFailed";return false;}
    output.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());return true;
}
bool Parse(std::string_view text,bool build,Json& output,std::string& errorKey)
{
    errorKey.clear();const auto invalid=build?"Project.InvalidBuildSettings":"Project.InvalidManifest";
    try
    {
        if(text.empty()||text.size()>4*1024*1024){errorKey=invalid;return false;}
        bool duplicate=false;std::vector<std::set<std::string>> keys;
        auto parsed=Json::parse(text.begin(),text.end(),[&](int depth,Json::parse_event_t event,Json& value){
            if(depth>32)throw std::runtime_error("Project.InvalidManifest");
            if(event==Json::parse_event_t::object_start)keys.emplace_back();
            else if(event==Json::parse_event_t::key&&!keys.empty()&&!keys.back().insert(value.get<std::string>()).second)duplicate=true;
            else if(event==Json::parse_event_t::object_end)keys.pop_back();return true;
        });
        const auto fmt=parsed.is_object()?parsed.value("format",std::string{}):std::string{};
        const bool validFormat=build?(fmt=="FlareEngine.Build"||fmt=="SelfEngine.Build"):(fmt=="FlareEngine.Project"||fmt=="SelfEngine.Project");
        if(duplicate||!parsed.is_object()||!validFormat)
        {errorKey=invalid;return false;}
        if(!parsed.contains("version")||!parsed.at("version").is_number_integer()||parsed.at("version")!=1)
        {errorKey="Project.UnsupportedVersion";return false;}
        output=std::move(parsed);return true;
    }
    catch(...){errorKey=invalid;return false;}
}
bool InvalidName(const std::string& name)
{
    return name.empty()||name.size()>128||name.find_first_of("<>:\"/\\|?*")!=std::string::npos;
}
std::filesystem::path FromUtf8(const std::string& text)
{
    if(text.empty())return {};
    const int size=MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0);
    if(size<=0)return {};
    std::wstring wide(static_cast<std::size_t>(size),L'\0');
    MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),wide.data(),size);
    return std::filesystem::path(wide);
}
bool RelativeInside(const std::filesystem::path& root,const std::string& value,std::filesystem::path& output)
{
    if(value.empty())return false;
    const std::filesystem::path requested=FromUtf8(value);
    if(requested.is_absolute()||requested.root_name()!=std::filesystem::path{})return false;
    std::error_code error;
    const auto combined=std::filesystem::weakly_canonical(root/requested,error);
    if(error||combined.empty())return false;
    const auto canonicalRoot=std::filesystem::weakly_canonical(root,error);
    if(error||canonicalRoot.empty())return false;
    auto rootText=canonicalRoot.native();auto pathText=combined.native();
    std::transform(rootText.begin(),rootText.end(),rootText.begin(),[](wchar_t ch){return static_cast<wchar_t>(towlower(ch));});
    std::transform(pathText.begin(),pathText.end(),pathText.begin(),[](wchar_t ch){return static_cast<wchar_t>(towlower(ch));});
    if(pathText.size()<=rootText.size()||pathText.compare(0,rootText.size(),rootText)!=0)return false;
    if(pathText[rootText.size()]!=L'\\'&&pathText[rootText.size()]!=L'/')return false;
    output=combined;
    return true;
}
}
bool ProjectSettingsService::DecodeProject(std::string_view text,ProjectSettings& output,std::string& errorKey)
{
    errorKey.clear();Json json;
    if(!Parse(text,false,json,errorKey))return false;
    try
    {
        ProjectSettings settings;
        settings.name=json.at("name").get<std::string>();
        settings.showPlayerQualityMenu=json.value("showPlayerQualityMenu",true);
        settings.playerSettingsId=json.value("playerSettingsId",std::string{});
        if(json.contains("qualityProfiles")&&!ReadQualityProfiles(json.at("qualityProfiles"),settings.qualityProfiles)){errorKey="PlayerQuality.InvalidProfiles";return false;}
        settings.contentRoot=json.at("contentRoot").get<std::string>();
        settings.entryScene=json.at("entryScene").get<std::string>();
        if(json.contains("assets")&&json.at("assets").is_array())
        {
            for(const auto& item:json.at("assets"))
            {
                if(!item.is_object())continue;
                const auto id=item.value("id",std::string{});
                const auto type=item.value("type",std::string{});
                if(!id.empty())settings.assets.push_back(type.empty()?id:id+" ("+type+")");
            }
        }
        output=std::move(settings);return true;
    }
    catch(...){errorKey="Project.InvalidManifest";return false;}
}
bool ProjectSettingsService::ReadProject(const std::filesystem::path& manifest,ProjectSettings& output,std::string& errorKey)
{
    errorKey.clear();std::string text;ProjectSettings settings;
    if(!ReadText(manifest,text,errorKey)||!DecodeProject(text,settings,errorKey))return false;
    if(settings.name.empty()||settings.contentRoot.empty()||settings.entryScene.empty()){errorKey="Project.InvalidManifest";return false;}
    output=std::move(settings);return true;
}
bool ProjectSettingsService::MergeProject(std::string_view original,const ProjectSettings& settings,std::string& output,std::string& errorKey)
{
    Json json;if(!Parse(original,false,json,errorKey))return false;
    try
    {
        json["name"]=settings.name;json["contentRoot"]=settings.contentRoot;json["entryScene"]=settings.entryScene;
        if(!settings.showPlayerQualityMenu||json.contains("showPlayerQualityMenu"))json["showPlayerQualityMenu"]=settings.showPlayerQualityMenu;
        if(settings.playerSettingsId.empty())json.erase("playerSettingsId");else json["playerSettingsId"]=settings.playerSettingsId;
        if(settings.qualityProfiles!=DefaultQualityProfiles||json.contains("qualityProfiles"))WriteQualityProfiles(settings.qualityProfiles,json["qualityProfiles"]);
        output=json.dump(2)+"\n";return true;
    }
    catch(...){errorKey="Project.InvalidManifest";return false;}
}
bool ProjectSettingsService::PrepareProjectSave(const std::filesystem::path& projectRoot,std::string_view original,
    const ProjectSettings& settings,std::string& output,std::string& errorKey)
{
    std::string merged;if(!MergeProject(original,settings,merged,errorKey))return false;
    std::filesystem::path entry;
    std::error_code error;
    const auto absoluteRoot=std::filesystem::absolute(projectRoot.empty()?std::filesystem::path("."):projectRoot,error);
    if(error||!RelativeInside(absoluteRoot,settings.entryScene,entry)||!std::filesystem::is_regular_file(entry,error)||error)
    {errorKey="Project.InvalidEntryScene";return false;}
    Json json;if(!Parse(merged,false,json,errorKey))return false;
    try
    {
        if(!json.contains("scenes"))json["scenes"]=Json::array();
        auto& scenes=json["scenes"];
        if(!scenes.is_array()){errorKey="Project.InvalidManifest";return false;}
        bool registered=false;
        for(const auto& value:scenes)
        {
            std::filesystem::path scene;
            if(!value.is_string()||!RelativeInside(absoluteRoot,value.get<std::string>(),scene))
            {errorKey="Project.InvalidManifest";return false;}
            if(CompareStringOrdinal(scene.c_str(),-1,entry.c_str(),-1,TRUE)==CSTR_EQUAL)registered=true;
        }
        if(!registered)
        {
            if(scenes.size()>=10000){errorKey="Project.InvalidManifest";return false;}
            const auto root=std::filesystem::weakly_canonical(absoluteRoot,error);
            if(error){errorKey="Project.InvalidEntryScene";return false;}
            const auto relative=entry.lexically_relative(root);
            if(relative.empty()||relative.is_absolute()||*relative.begin()=="..")
            {errorKey="Project.InvalidEntryScene";return false;}
            scenes.push_back(asset_io::Utf8(relative));
        }
        output=json.dump(2)+"\n";return true;
    }
    catch(...){errorKey="Project.InvalidManifest";return false;}
}
bool ProjectSettingsService::WriteProject(const std::filesystem::path& manifest,const ProjectSettings& settings,std::string& errorKey)
{
    errorKey.clear();
    if(InvalidName(settings.name)){errorKey="Project.InvalidName";return false;}
    std::string original,text;
    if(!ReadText(manifest,original,errorKey)||!PrepareProjectSave(manifest.parent_path(),original,settings,text,errorKey))return false;
    if(!WriteAtomic(manifest,text)){errorKey="Project.WriteFailed";return false;}
    return true;
}
bool ProjectSettingsService::ReadBuild(const std::filesystem::path& buildJson,BuildSettings& output,std::string& errorKey)
{
    errorKey.clear();
    std::error_code fileError;
    if(!std::filesystem::exists(buildJson,fileError)){errorKey=fileError?"Project.ReadFailed":"Project.BuildSettingsMissing";return false;}
    std::string text;if(!ReadText(buildJson,text,errorKey))return false;
    return DecodeBuild(text,output,errorKey);
}
bool ProjectSettingsService::DecodeBuild(std::string_view text,BuildSettings& output,std::string& errorKey)
{
    errorKey.clear();
    Json json;
    if(!Parse(text,true,json,errorKey))return false;
    try
    {
        BuildSettings settings;
        settings.scriptsProject=json.value("scriptsProject",std::string{});
        settings.scriptType=json.value("scriptType",std::string{});
        settings.bindings=json.value("bindings",std::string{});
        settings.entryUi=json.value("entryUi",std::string{});
        settings.recordVideo=json.value("recordVideo",false);
        settings.videoResolution=json.value("videoResolution",settings.videoResolution);
        for(const auto key:{"videoFps","videoSeconds"}){
            if(!json.contains(key))continue;
            const auto& value=json.at(key);
            if(!value.is_number_integer()||value<1||value>100000){
                errorKey="Project.InvalidBuildSettings";return false;}
        }
        settings.videoFps=json.value("videoFps",settings.videoFps);
        settings.videoSeconds=json.value("videoSeconds",settings.videoSeconds);
        if(!ValidVideo(settings)){errorKey="Project.InvalidBuildSettings";return false;}
        output=std::move(settings);return true;
    }
    catch(...){errorKey="Project.InvalidBuildSettings";return false;}
}
bool ProjectSettingsService::MergeBuild(std::string_view original,const BuildSettings& settings,std::string& output,std::string& errorKey)
{
    errorKey.clear();Json json={{"format","FlareEngine.Build"},{"version",1}};
    if(!original.empty()&&!Parse(original,true,json,errorKey))return false;
    try
    {
        json["scriptsProject"]=settings.scriptsProject;json["scriptType"]=settings.scriptType;
        json["bindings"]=settings.bindings;json["entryUi"]=settings.entryUi;
        if(!ValidVideo(settings)){errorKey="Project.InvalidBuildSettings";return false;}
        json["recordVideo"]=settings.recordVideo;json["videoResolution"]=settings.videoResolution;
        json["videoFps"]=settings.videoFps;json["videoSeconds"]=settings.videoSeconds;
        output=json.dump(2)+"\n";return true;
    }
    catch(...){errorKey="Project.InvalidBuildSettings";return false;}
}
bool ProjectSettingsService::WriteBuild(const std::filesystem::path& buildJson,const BuildSettings& settings,std::string& errorKey)
{
    errorKey.clear();
    std::error_code error;const bool exists=std::filesystem::exists(buildJson,error);
    if(error){errorKey="Project.ReadFailed";return false;}
    std::string original,text;
    if(exists&&(!ReadText(buildJson,original,errorKey)||original.empty()))
    {if(errorKey.empty())errorKey="Project.InvalidBuildSettings";return false;}
    if(!MergeBuild(original,settings,text,errorKey))return false;
    if(!WriteAtomic(buildJson,text)){errorKey="Project.WriteFailed";return false;}
    return true;
}
std::vector<std::string> ProjectSettingsService::MissingBuildFields(const BuildSettings& settings)
{
    std::vector<std::string> missing;
    if(settings.scriptsProject.empty())missing.push_back("scriptsProject");
    if(settings.scriptType.empty())missing.push_back("scriptType");
    return missing;
}
bool ProjectSettingsService::ValidateProject(const std::filesystem::path& projectRoot,const ProjectSettings& settings,
    std::string& errorKey,std::string& detail)
{
    errorKey.clear();detail.clear();
    if(InvalidName(settings.name)){errorKey="Project.InvalidName";detail=settings.name;return false;}
    std::filesystem::path contentRoot;
    if(!RelativeInside(projectRoot,settings.contentRoot,contentRoot)){errorKey="Project.InvalidContentRoot";detail=settings.contentRoot;return false;}
    std::error_code error;
    if(!std::filesystem::is_directory(contentRoot,error)){errorKey="Project.InvalidContentRoot";detail=settings.contentRoot;return false;}
    std::filesystem::path scene;
    if(!RelativeInside(projectRoot,settings.entryScene,scene)){errorKey="Project.InvalidEntryScene";detail=settings.entryScene;return false;}
    if(!std::filesystem::is_regular_file(scene,error)){errorKey="Project.InvalidEntryScene";detail=settings.entryScene;return false;}
    return true;
}
bool ProjectSettingsService::ValidateBuild(const std::filesystem::path& projectRoot,const BuildSettings& settings,
    std::string& errorKey,std::string& detail)
{
    errorKey.clear();detail.clear();
    std::error_code error;
    if(!settings.scriptsProject.empty())
    {
        std::filesystem::path scripts;
        if(!RelativeInside(projectRoot,settings.scriptsProject,scripts)
            ||scripts.extension()!=".csproj"||!std::filesystem::is_regular_file(scripts,error))
        {errorKey="Project.InvalidScriptsProject";detail=settings.scriptsProject;return false;}
        if(settings.scriptType.empty()){errorKey="Project.InvalidBuildSettings";detail="scriptType";return false;}
    }
    if(!settings.bindings.empty())
    {
        std::filesystem::path bindings;
        if(!RelativeInside(projectRoot,settings.bindings,bindings)||!std::filesystem::is_regular_file(bindings,error))
        {errorKey="Project.InvalidBindings";detail=settings.bindings;return false;}
    }
    if(!settings.entryUi.empty())
    {
        std::filesystem::path ui;
        if(!RelativeInside(projectRoot,settings.entryUi,ui)||!std::filesystem::is_regular_file(ui,error))
        {errorKey="Project.InvalidEntryUi";detail=settings.entryUi;return false;}
    }
    return true;
}

std::filesystem::path ProjectSettingsService::InputActionsPath(const std::filesystem::path& projectRoot,const ProjectSettings& settings)
{
    std::filesystem::path contentRoot;
    if(!RelativeInside(projectRoot,settings.contentRoot,contentRoot))return {};
    std::filesystem::path resolved;
    if(!RelativeInside(contentRoot,std::string(InputActionsRelativePath),resolved))return {};
    return resolved;
}

bool ProjectSettingsService::ReadInputActions(const std::filesystem::path& projectRoot,const ProjectSettings& settings,
    InputActionDocument& output,std::string& errorKey)
{
    const auto path=InputActionsPath(projectRoot,settings);
    if(path.empty()){errorKey="Project.InvalidContentRoot";return false;}
    std::error_code ec;
    if(!std::filesystem::exists(path,ec))
    {
        if(ec){errorKey="Input.ReadFailed";return false;}
        output=DefaultInputActionDocument();
        errorKey.clear();
        return true;
    }
    return ReadInputActionDocument(path,output,errorKey);
}

bool ProjectSettingsService::WriteInputActions(const std::filesystem::path& projectRoot,const ProjectSettings& settings,
    const InputActionDocument& document,std::string& errorKey)
{
    const auto path=InputActionsPath(projectRoot,settings);
    if(path.empty()){errorKey="Project.InvalidContentRoot";return false;}
    return WriteInputActionDocument(path,document,errorKey);
}

bool ProjectSettingsService::SetInputActionBinding(InputActionDocument& document,std::string_view actionName,
    InputActionKind kind,float deadZone,const InputBinding& binding,std::string& errorKey)
{
    if(actionName.empty()||!std::isfinite(deadZone)||deadZone<0.0f||deadZone>=1.0f)
    {errorKey="Input.InvalidDocument";return false;}
    InputActionDocument candidate=document;
    auto it=std::find_if(candidate.actions.begin(),candidate.actions.end(),
        [actionName](const InputActionDefinition& a){return a.name==actionName;});
    if(it==candidate.actions.end())
    {
        candidate.actions.push_back({std::string(actionName),kind,deadZone,{binding}});
    }
    else
    {
        it->kind=kind;
        it->deadZone=deadZone;
        auto existing=std::find_if(it->bindings.begin(),it->bindings.end(),[&](const InputBinding& b){
            if(b.device!=binding.device)return false;
            if(b.device==InputDeviceKind::Keyboard)return b.keyCode==binding.keyCode;
            if(b.device==InputDeviceKind::Mouse)return b.mouseControl==binding.mouseControl;
            return b.gamepadControl==binding.gamepadControl&&b.gamepadIndex==binding.gamepadIndex;
        });
        if(existing!=it->bindings.end())*existing=binding;
        else it->bindings.push_back(binding);
    }
    if(!ValidateInputActionDocument(candidate,errorKey))return false;
    document=std::move(candidate);
    return true;
}

bool ProjectSettingsService::RemoveInputAction(InputActionDocument& document,std::string_view actionName,std::string& errorKey)
{
    const auto before=document.actions.size();
    document.actions.erase(std::remove_if(document.actions.begin(),document.actions.end(),
        [actionName](const InputActionDefinition& a){return a.name==actionName;}),document.actions.end());
    if(document.actions.size()==before){errorKey="Input.ActionNotFound";return false;}
    errorKey.clear();
    return true;
}

bool ProjectSettingsService::ApplyInputActionSetting(const std::filesystem::path& projectRoot,const ProjectSettings& settings,
    std::string_view key,const std::string& value,std::string& errorKey)
{
    InputActionDocument doc;
    if(!ReadInputActions(projectRoot,settings,doc,errorKey))return false;
    if(key=="input.resetDefaults")
    {
        return WriteInputActions(projectRoot,settings,DefaultInputActionDocument(),errorKey);
    }
    if(key.starts_with("input.deadZone."))
    {
        const std::string_view actionName=key.substr(15);
        float dz=0.0f;
        const auto [ptr,ec]=std::from_chars(value.data(),value.data()+value.size(),dz);
        if(ec!=std::errc{}||ptr!=value.data()+value.size()||!std::isfinite(dz)||dz<0.0f||dz>=1.0f)
        {errorKey="Input.InvalidDocument";return false;}
        auto it=std::find_if(doc.actions.begin(),doc.actions.end(),
            [actionName](const InputActionDefinition& a){return a.name==actionName;});
        if(it==doc.actions.end()){errorKey="Input.ActionNotFound";return false;}
        it->deadZone=dz;
        return WriteInputActions(projectRoot,settings,doc,errorKey);
    }
    if(key.starts_with("input.key."))
    {
        const std::string_view actionName=key.substr(10);
        std::uint32_t code=0;
        const auto [ptr,ec]=std::from_chars(value.data(),value.data()+value.size(),code);
        if(ec!=std::errc{}||ptr!=value.data()+value.size()||code>=256u)
        {errorKey="Input.InvalidBinding";return false;}
        auto it=std::find_if(doc.actions.begin(),doc.actions.end(),
            [actionName](const InputActionDefinition& a){return a.name==actionName;});
        if(it==doc.actions.end()){errorKey="Input.ActionNotFound";return false;}
        auto kb=std::find_if(it->bindings.begin(),it->bindings.end(),
            [](const InputBinding& b){return b.device==InputDeviceKind::Keyboard;});
        if(kb!=it->bindings.end())kb->keyCode=code;
        else it->bindings.push_back(InputBinding{InputDeviceKind::Keyboard,code,MouseControl::None,GamepadControl::None,0,1.0f,0.0f,1.0f});
        return WriteInputActions(projectRoot,settings,doc,errorKey);
    }
    errorKey="Input.InvalidDocument";
    return false;
}
}
