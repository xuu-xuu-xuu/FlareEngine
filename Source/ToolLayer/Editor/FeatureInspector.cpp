#include "FeatureInspector.h"
#include "EditorSession.h"
#include "FunctionLayer/Runtime/FeatureComponents.h"
#include "CoreLayer/Localization.h"
#include <charconv>

namespace se
{
namespace
{
using namespace ui::editor;
std::string Number(float value){char buffer[48];auto [end,error]=std::to_chars(buffer,buffer+sizeof(buffer),value);return error==std::errc{}?std::string(buffer,end):"0";}
}
void AppendFeatureInspector(const EditorSession& session,const Localization& text,StableEntityId id,std::vector<EditorProperty>& rows)
{
    const auto& world=session.EditWorld();const auto entity=world.FindByStableId(id);if(!entity)return;
    for(const auto& descriptor:ComponentMetadata())
    {
        if(!IsEngineFeature(descriptor.type))continue;
        const auto title=text.GetText(std::string(descriptor.labelKey));
        const auto handle=world.FindComponent(entity,descriptor.type);const auto* component=world.GetComponent(handle);
        if(!component){rows.push_back({"add."+std::string(descriptor.key),title,EditorPropertyType::Action,{},session.IsPlaying(),{},text.GetText("EditorUi.AddComponent"),true});continue;}
        rows.push_back({"feature.enabled."+std::string(descriptor.key),text.GetText("Feature.Enabled"),EditorPropertyType::Boolean,component->Enabled()?"true":"false",session.IsPlaying(),{},title});
        const bool rendering=descriptor.type==RenderSettingsComponent::TypeId;
        const bool instancing=rendering&&std::get<bool>(FeatureValue(world,entity,"render.instancing"));
        if(rendering||descriptor.type==EnvironmentSettingsComponent::TypeId)
            rows.push_back({"feature.scope",text.GetText("Feature.Tools.Scope"),EditorPropertyType::String,text.GetText(instancing?"Feature.Render.InstancingLimits":"Feature.Render.SceneScope"),true,{},title});
        for(std::size_t index=0;index<descriptor.properties.size();++index)
        {
            const auto& property=descriptor.properties[index];PropertyValue value;std::string error;
            if(!descriptor.read(world,handle,index,value,error))continue;
            const auto label=text.GetText(std::string(property.labelKey));
            const auto append=[&](std::string key,std::string raw,EditorPropertyType type,char axis=0){
                EditorProperty row{std::move(key),label,type,std::move(raw),session.IsPlaying()||!property.editable,{},title};
                if(rendering)
                {
                    const bool limited=property.key.starts_with("render.shadow")||property.key=="render.post"||property.key=="render.lod"||property.key=="render.frameGraph"||property.key=="render.aces"||property.key.starts_with("render.bloom")||property.key=="render.taa"||property.key=="render.fxaa"||property.key=="render.autoExposure"||property.key.starts_with("render.lut");
                    row.readOnly|=(instancing&&(limited||property.key.starts_with("render.hlod")))||(!instancing&&property.key.starts_with("render.occlusion"));
                }
                row.axis=axis;if(type==EditorPropertyType::Number){row.dragStep=.01f;row.dragMinimum=property.minimum;row.dragMaximum=property.maximum;}rows.push_back(std::move(row));};
            if(const auto* vector=std::get_if<Vec3>(&value))
            {const float values[]{vector->x,vector->y,vector->z};for(int axis=0;axis<3;++axis)append(std::string(property.key)+'.'+"xyz"[axis],Number(values[axis]),EditorPropertyType::Number,"XYZ"[axis]);}
            else if(const auto* number=std::get_if<float>(&value))append(std::string(property.key),Number(*number),EditorPropertyType::Number);
            else if(const auto* boolean=std::get_if<bool>(&value))append(std::string(property.key),*boolean?"true":"false",EditorPropertyType::Boolean);
            else if(const auto* string=std::get_if<std::string>(&value))append(std::string(property.key),*string,property.type==PropertyType::AssetId?EditorPropertyType::Asset:EditorPropertyType::String);
        }
        rows.push_back({"feature.remove."+std::string(descriptor.key),text.GetText("Feature.Remove"),EditorPropertyType::Action,{},session.IsPlaying(),{},title});
    }
}
std::optional<bool> ApplyFeatureInspector(EditorSession& session,StableEntityId id,const std::string& key,const std::string& raw)
{
    if(key.starts_with("feature.enabled.")||key.starts_with("feature.remove.")||key.starts_with("add."))
    {
        const auto name=key.substr(key.starts_with("feature.enabled.")?16:key.starts_with("feature.remove.")?15:4);
        const auto* descriptor=FindComponentDescriptor(name);if(!descriptor||!IsEngineFeature(descriptor->type))return {};
        return session.Apply("Feature.Edit",[&](World& world){const auto entity=world.FindByStableId(id);std::string error;
            if(key.starts_with("add."))return AddComponent(world,entity,name,error);
            if(key.starts_with("feature.remove."))return RemoveComponent(world,entity,name,error);
            return (raw=="true"||raw=="false")&&world.SetComponentEnabled(world.FindComponent(entity,descriptor->type),raw=="true");});
    }
    std::string propertyKey=key;const auto* property=FindProperty(propertyKey);char axis=0;
    if(!property&&key.size()>2&&key[key.size()-2]=='.')
    {propertyKey=key.substr(0,key.size()-2);property=FindProperty(propertyKey);axis=key.back();}
    const auto* descriptor=FindComponentDescriptor(propertyKey.substr(0,propertyKey.find('.')));
    if(!property||!descriptor||!IsEngineFeature(descriptor->type))return {};
    PropertyValue value;
    if(property->type==PropertyType::Boolean){if(raw!="true"&&raw!="false")return false;value=raw=="true";}
    else if(property->type==PropertyType::AssetId||property->type==PropertyType::Text)value=raw;
    else
    {
        float number=0;auto [end,error]=std::from_chars(raw.data(),raw.data()+raw.size(),number);
        if(error!=std::errc{}||end!=raw.data()+raw.size()||!IsFinite(number))return false;
        if(property->type==PropertyType::Vector3)
        {
            auto vector=std::get<Vec3>(FeatureValue(session.EditWorld(),session.EditWorld().FindByStableId(id),propertyKey));
            if(axis=='x')vector.x=number;else if(axis=='y')vector.y=number;else if(axis=='z')vector.z=number;else return false;value=vector;
        }
        else value=number;
    }
    return session.SetProperty(id,propertyKey,value);
}
}
