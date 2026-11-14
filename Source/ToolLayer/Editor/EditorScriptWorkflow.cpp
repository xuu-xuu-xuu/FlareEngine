#include "EditorScriptWorkflow.h"
#include "EditorApplication.h"
#include "EditorScriptTools.h"
#include "CoreLayer/Localization.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iterator>

namespace se
{
namespace
{
using Json=nlohmann::json;
using ui::editor::EditorProperty;
using ui::editor::EditorPropertyType;
std::string Key(Json command){return "script."+command.dump();}
}
EditorScriptWorkflow::EditorScriptWorkflow(std::filesystem::path root,EditorApplicationHooks* hooks,const Localization& text)
    :root_(std::move(root)),hooks_(hooks),text_(text){}

const std::vector<std::filesystem::path>& EditorScriptWorkflow::Sources()
{
    const auto now=std::chrono::steady_clock::now();
    if(now-scanned_<std::chrono::seconds(1))return sources_;
    scanned_=now;sources_.clear();
    std::error_code error;
    std::filesystem::recursive_directory_iterator cursor(root_/"Scripts",error),end;
    for(;!error&&cursor!=end;cursor.increment(error))
    {
        if(cursor->is_symlink(error)){cursor.disable_recursion_pending();continue;}
        if(cursor->is_directory(error))
        {
            if(cursor->path().filename()=="bin"||cursor->path().filename()=="obj")cursor.disable_recursion_pending();
            continue;
        }
        if(cursor->path().extension()==".cs")sources_.push_back(cursor->path());
    }
    return sources_;
}

void EditorScriptWorkflow::AppendRows(std::uint64_t entity,bool playing,std::vector<EditorProperty>& rows)
{
    const std::string addSection = text_.GetText("EditorUi.AddComponent");
    std::string section = addSection;
    const auto append = [&](EditorProperty property) -> EditorProperty& {
        property.section = section;
        property.sectionInitiallyClosed = section == addSection;
        rows.push_back(std::move(property));
        return rows.back();
    };
    const auto action=[&](Json command,const std::string& label){
        append({Key(std::move(command)),label,EditorPropertyType::Action,{},playing});
    };
    append({"script.class",text_.GetText("Editor.ScriptClassName"),EditorPropertyType::String,className_,playing});
    action({{"Action","Create"}},text_.GetText("Editor.CreateBehaviour"));
    if(!selectedSource_.empty())action({{"Action","OpenSelected"}},text_.GetText("Editor.ScriptOpenSource")+" "+asset_io::Utf8(selectedSource_.filename()));
    if(!hooks_||!hooks_->scriptQuery||!entity)return;
    std::string document,error;
    if(!hooks_->scriptQuery(entity,document,error))
    {
        section = text_.GetText("Script.Inspector");
        append({"script.error",text_.GetText("Script.Inspector"),EditorPropertyType::String,text_.GetText(error),true});
        return;
    }
    try
    {
        const auto data=Json::parse(document);
        for(const auto& script:data.at("Scripts"))
        {
            const auto type=script.at("Type").get<std::string>();
            section = type + " (" + text_.GetText("EditorUi.ScriptComponent") + ")";
            action({{"Action","Open"},{"Type",type}},text_.GetText("Editor.ScriptOpenSource")+" "+type);
            append({Key({{"Type",type},{"Enabled",true}}),text_.GetText("Script.Enabled"),EditorPropertyType::Boolean,
                    script.value("Enabled",true)?"true":"false",playing}).sectionEnabled = true;
            if(script.value("Missing",false))
                append({Key({{"Missing",type}}),type,EditorPropertyType::String,text_.GetText("Script.TypeMissing"),true});
            for(const auto& field:script.at("Fields"))
            {
                const auto name=field.at("Name").get<std::string>(),kind=field.at("Kind").get<std::string>();
                const auto value=field.at("Value");
                const Json command={{"Type",type},{"Field",name},{"Kind",kind}};
                if(kind=="vec3")
                {
                    for(const auto* axis:{"X","Y","Z"})
                    {
                        auto axisCommand=command;axisCommand["Axis"]=axis;axisCommand["Value"]=value;
                        append({Key(axisCommand),name,EditorPropertyType::Number,value.at(axis).dump(),playing}).axis = *axis;
                    }
                }
                else
                {
                    const bool string=kind=="string"||kind=="entity";
                    append({Key(command),name,kind=="bool"?EditorPropertyType::Boolean:
                        string?EditorPropertyType::String:EditorPropertyType::Number,
                        value.is_null()?std::string{}:string?value.get<std::string>():value.dump(),playing});
                }
            }
            action({{"Type",type},{"Remove",true}},text_.GetText("Script.RemoveBinding")+" "+type);
        }
        section = addSection;
        for(const auto& available:data.at("Available"))
        {
            const auto type=available.get<std::string>();
            action({{"Type",type},{"Add",true}},text_.GetText("Script.AddBinding")+" "+type);
        }
        action({{"Save",true}},text_.GetText("Script.SaveBindings"));
        if(!undo_.empty())action({{"Action","Undo"}},text_.GetText("EditorUi.Undo")+" "+text_.GetText("Script.Inspector"));
        if(!redo_.empty())action({{"Action","Redo"}},text_.GetText("EditorUi.Redo")+" "+text_.GetText("Script.Inspector"));
    }
    catch(const Json::exception&){status_=text_.GetText("Script.MetadataFailed");}
}

bool EditorScriptWorkflow::Open(const std::filesystem::path& source,const CodeEditorSettings& settings)
{
    std::string error,detail;
    if(EditorScriptTools::OpenSource(root_,source,1,1,settings,error,detail))return true;
    status_=text_.GetText(error)+" "+detail;return false;
}

bool EditorScriptWorkflow::Snapshot(RecoveryDocument& document)
{
    if(!hooks_||!hooks_->externalDocuments.snapshot)return false;
    std::vector<EditorExternalDocument> documents;std::string error;
    if(!hooks_->externalDocuments.snapshot(documents,error)){status_=text_.GetText(error);return false;}
    for(const auto& entry:documents)if(entry.document.kind=="bindings"){document=entry.document;return true;}
    return false;
}

bool EditorScriptWorkflow::Save()
{
    if(!hooks_||!hooks_->externalDocuments.snapshot)return true;
    std::vector<EditorExternalDocument> documents;std::string error;
    if(!hooks_->externalDocuments.snapshot(documents,error)){status_=text_.GetText(error);return false;}
    for(const auto& document:documents)
    {
        if(document.document.kind!="bindings"||!document.dirty)continue;
        if(!hooks_->externalDocuments.save||!hooks_->externalDocuments.save(document.document.documentId,error))
        {status_=text_.GetText(error.empty()?"Script.DocumentInvalid":error);return false;}
    }
    return true;
}

bool EditorScriptWorkflow::Restore(bool redo)
{
    auto& from=redo?redo_:undo_;
    auto& to=redo?undo_:redo_;
    if(from.empty()||!hooks_||!hooks_->externalDocuments.restore)return false;
    RecoveryDocument current;
    if(!Snapshot(current))return false;
    const auto& change=from.back();
    const auto& expected=redo?change.before:change.after;
    if(current.content!=expected.content)
    {undo_.clear();redo_.clear();status_=text_.GetText("Script.DocumentExternalChanged");return false;}
    auto target=redo?change.after:change.before;
    target.sourceDigest=current.sourceDigest;
    std::string error;
    if(!hooks_->externalDocuments.restore(target,true,error)){status_=text_.GetText(error);return false;}
    to.push_back(change);from.pop_back();return true;
}

bool EditorScriptWorkflow::Apply(std::uint64_t entity,bool playing,const std::string& key,const std::string& value,
                                 const CodeEditorSettings& settings)
{
    if(playing)return false;
    status_.clear();
    if(key=="script.class"){className_=value;return true;}
    try
    {
        auto command=Json::parse(key.substr(7));
        if(command.contains("Action"))
        {
            if(command.at("Action")=="Undo")return Restore(false);
            if(command.at("Action")=="Redo")return Restore(true);
            if(command.at("Action")=="OpenSelected")return Open(selectedSource_,settings);
            if(command.at("Action")=="Create")
            {
                EditorScriptCreateResult result;std::string error,detail;
                if(!EditorScriptTools::CreateBehaviour({root_,className_,"Game"},result,error,detail))
                {status_=text_.GetText(error)+" "+detail;return false;}
                status_=text_.GetText("Editor.ScriptCreated")+" "+asset_io::Utf8(result.source)+" "+
                        text_.GetText("Editor.ScriptRebuildHint");
                return Open(result.source,settings);
            }
            const auto type=command.at("Type").get<std::string>();
            for(const auto& path:Sources())
            {
                std::ifstream file(path,std::ios::binary);
                const std::string source{std::istreambuf_iterator<char>(file),{}};
                if(source.find("\""+type+"\"")!=std::string::npos)return Open(path,settings);
            }
            status_=text_.GetText("Script.TypeMissing");return false;
        }
        if(command.contains("Enabled"))command["Enabled"]=value=="true";
        if(command.contains("Kind"))
        {
            const auto kind=command.at("Kind").get<std::string>();
            if(kind=="string"||kind=="entity")command["Value"]=value;
            else if(command.contains("Axis"))command["Value"][command.at("Axis").get<std::string>()]=Json::parse(value);
            else command["Value"]=Json::parse(value);
            command.erase("Kind");command.erase("Axis");
        }
        std::string error;
        RecoveryDocument before;
        const bool changing=!command.contains("Save");
        if(changing&&!Snapshot(before))return false;
        if(hooks_&&hooks_->scriptApply&&hooks_->scriptApply(entity,command.dump(),error))
        {
            RecoveryDocument after;
            if(changing&&Snapshot(after)&&before.content!=after.content)
            {
                undo_.push_back({std::move(before),std::move(after)});redo_.clear();
                if(undo_.size()>100)undo_.erase(undo_.begin());
            }
            return true;
        }
        status_=text_.GetText(error.empty()?"Script.MetadataFailed":error);
    }
    catch(const Json::exception&){status_=text_.GetText("Script.FieldValueInvalid");}
    return false;
}
}
