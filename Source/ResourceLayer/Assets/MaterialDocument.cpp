#include "MaterialDocument.h"
#include "MaterialMetadata.h"
#include "ResourceLayer/PropertyJson.h"
#include "AssetIO.h"
#include "CoreLayer/Utf.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>

namespace flare
{
namespace
{
bool AssetReference(const AssetId& id)
{
    return id.empty()||(id.size()==32&&std::all_of(id.begin(),id.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}));
}
}
bool IsMaterialDocumentPath(const std::filesystem::path& path)
{
    auto name=asset_io::Utf8(path.filename());
    std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    return name.size()>std::string_view(".material.json").size()&&name.ends_with(".material.json");
}
bool ValidateMaterialDocument(const MaterialDocument& material)
{
    std::u32string decoded;
    if(material.name.size()>1024||!DecodeUtf8(material.name,decoded))return false;
    for(float value:material.baseColor)if(!std::isfinite(value)||value<0||value>1)return false;
    if(!std::isfinite(material.metallic)||material.metallic<0||material.metallic>1
        ||!std::isfinite(material.roughness)||material.roughness<0||material.roughness>1||!std::isfinite(material.normalScale)
        ||!IsValidMaterialAlphaMode(material.alphaMode)||!std::isfinite(material.alphaCutoff)||material.alphaCutoff<0)return false;
    if(!std::isfinite(material.clearCoat.weight)||material.clearCoat.weight<0||material.clearCoat.weight>1
        ||!std::isfinite(material.clearCoat.roughness)||material.clearCoat.roughness<0||material.clearCoat.roughness>1
        ||!std::isfinite(material.clearCoat.normalScale))return false;
    if(!IsValidMaterialLayer(material.sheen)||!IsValidMaterialLayer(material.anisotropy)||!IsValidMaterialLayer(material.subsurface))return false;
    if(material.shaderParameters.size()>64)return false;
    for(const auto& [paramName,paramVal]:material.shaderParameters)
    {
        std::u32string paramDecoded;
        if(paramName.empty()||paramName.size()>64||!DecodeUtf8(paramName,paramDecoded))return false;
        if(!std::isfinite(paramVal.x)||!std::isfinite(paramVal.y)||!std::isfinite(paramVal.z)||!std::isfinite(paramVal.w))return false;
    }
    return AssetReference(material.baseColorTexture)&&AssetReference(material.normalTexture)
        &&AssetReference(material.metallicRoughnessTexture)&&AssetReference(material.shader)
        &&AssetReference(material.clearCoatWeightTexture)&&AssetReference(material.clearCoatRoughnessTexture)&&AssetReference(material.clearCoatNormalTexture)
        &&AssetReference(material.sheenColorTexture)&&AssetReference(material.sheenRoughnessTexture)&&AssetReference(material.anisotropyTexture);
}
std::vector<AssetId> MaterialTextureDependencies(const MaterialDocument& material)
{
    std::vector<AssetId> ids;
    for(const auto& id:{material.baseColorTexture,material.normalTexture,material.metallicRoughnessTexture,
        material.clearCoatWeightTexture,material.clearCoatRoughnessTexture,material.clearCoatNormalTexture,
        material.sheenColorTexture,material.sheenRoughnessTexture,material.anisotropyTexture})if(!id.empty())ids.push_back(id);
    std::sort(ids.begin(),ids.end());ids.erase(std::unique(ids.begin(),ids.end()),ids.end());return ids;
}
std::vector<AssetId> MaterialDependencies(const MaterialDocument& material)
{
    auto ids=MaterialTextureDependencies(material);
    if(!material.shader.empty())ids.push_back(material.shader);
    std::sort(ids.begin(),ids.end());ids.erase(std::unique(ids.begin(),ids.end()),ids.end());return ids;
}
bool SerializeMaterialDocument(const MaterialDocument& material,std::string& output,std::vector<AssetDiagnostic>& diagnostics)
{
    if(!ValidateMaterialDocument(material)){asset_io::Diagnostic(diagnostics,"Assets.InvalidMaterial");return false;}
    try
    {
        nlohmann::json json={{"type","FlareEngine.Material"},{"version",1}};
        for(const auto& field:MaterialProperties())
        {
            PropertyValue value;std::string error;if(!ReadMaterialProperty(material,field.key,value,error))return false;
            json[nlohmann::json::json_pointer("/"+std::string(SerializedPropertyKey(field)))]=WritePropertyJson(value);
        }
        if(!material.shaderParameters.empty())
        {
            nlohmann::json params=nlohmann::json::object();
            for(const auto& [k,v]:material.shaderParameters)
                params[k]={v.x,v.y,v.z,v.w};
            json["shaderParameters"]=std::move(params);
        }
        output=json.dump(2);return true;
    }
    catch(const std::exception& error){asset_io::Diagnostic(diagnostics,"Assets.InvalidMaterial",error.what());return false;}
}
bool DeserializeMaterialDocument(std::string_view text,MaterialDocument& output,std::vector<AssetDiagnostic>& diagnostics)
{
    try
    {
        if(text.size()>65536){asset_io::Diagnostic(diagnostics,"Assets.InvalidMaterial","document size");return false;}
        const auto json=nlohmann::json::parse(text);
        if((json.at("type")!="FlareEngine.Material"&&json.at("type")!="SelfEngine.Material")||json.at("version")!=1)
        {asset_io::Diagnostic(diagnostics,"Assets.MaterialVersionUnsupported");return false;}
        for(const char* key:{"clearCoat","sheen","anisotropy","subsurface","shaderParameters"})if(json.contains(key)&&!json.at(key).is_object())
        {asset_io::Diagnostic(diagnostics,"Assets.InvalidMaterial",key);return false;}
        MaterialDocument loaded;
        for(const auto& field:MaterialProperties())
        {
            const nlohmann::json::json_pointer path("/"+std::string(SerializedPropertyKey(field)));
            if(field.serializedOptional&&!json.contains(path))continue;
            PropertyValue value;std::string error;
            if(!json.contains(path)||!ReadPropertyJson(json.at(path),field,value)||!WriteMaterialProperty(loaded,field.key,value,error))
            {asset_io::Diagnostic(diagnostics,"Assets.InvalidMaterial",std::string(field.key));return false;}
        }
        if(json.contains("shaderParameters"))
        {
            for(auto it=json.at("shaderParameters").begin();it!=json.at("shaderParameters").end();++it)
            {
                Vec4 vec{0,0,0,1};
                if(it.value().is_number())vec.x=it.value().get<float>();
                else if(it.value().is_array()&&!it.value().empty()&&it.value().size()<=4)
                {
                    float* dst=&vec.x;
                    for(std::size_t i=0;i<it.value().size();++i)
                    {
                        if(!it.value()[i].is_number()){asset_io::Diagnostic(diagnostics,"Assets.InvalidMaterial","shaderParameters");return false;}
                        dst[i]=it.value()[i].get<float>();
                    }
                }
                else{asset_io::Diagnostic(diagnostics,"Assets.InvalidMaterial","shaderParameters");return false;}
                loaded.shaderParameters[it.key()]=vec;
            }
        }
        if(!ValidateMaterialDocument(loaded)){asset_io::Diagnostic(diagnostics,"Assets.InvalidMaterial");return false;}
        output=std::move(loaded);return true;
    }
    catch(const std::exception& error){asset_io::Diagnostic(diagnostics,"Assets.InvalidMaterial",error.what());return false;}
}
bool SaveMaterialDocument(const std::filesystem::path& path,const MaterialDocument& material,std::vector<AssetDiagnostic>& diagnostics)
{
    if(!IsMaterialDocumentPath(path)){asset_io::Diagnostic(diagnostics,"Assets.MaterialPathInvalid",asset_io::Utf8(path));return false;}
    std::string json;if(!SerializeMaterialDocument(material,json,diagnostics))return false;
    if(!asset_io::WriteAtomic(path,{reinterpret_cast<const std::uint8_t*>(json.data()),json.size()}))
    {asset_io::Diagnostic(diagnostics,"Assets.WriteFailed",asset_io::Utf8(path));return false;}
    return true;
}
bool LoadMaterialDocument(const std::filesystem::path& path,MaterialDocument& output,std::vector<AssetDiagnostic>& diagnostics)
{
    if(!IsMaterialDocumentPath(path)){asset_io::Diagnostic(diagnostics,"Assets.MaterialPathInvalid",asset_io::Utf8(path));return false;}
    std::vector<std::uint8_t> bytes;
    if(!asset_io::Read(path,bytes)){asset_io::Diagnostic(diagnostics,"Assets.ReadFailed",asset_io::Utf8(path));return false;}
    return DeserializeMaterialDocument(std::string_view(reinterpret_cast<const char*>(bytes.data()),bytes.size()),output,diagnostics);
}
}
