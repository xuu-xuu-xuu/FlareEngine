#include "MaterialMetadata.h"
#include <array>
#include <limits>

namespace flare
{
namespace
{
const std::array fields{
    PropertyDescriptor{"material.name","EditorUi.Name",PropertyType::Text,std::string{},std::nullopt,std::nullopt},
    PropertyDescriptor{"material.baseColor","EditorUi.BaseColor",PropertyType::Vector4,Vec4{1,1,1,1},0.0f,1.0f,true,{},PropertyPresentation::Color},
    PropertyDescriptor{"material.metallic","EditorUi.Metallic",PropertyType::Number,0.0f,0.0f,1.0f,true,{},PropertyPresentation::Slider},
    PropertyDescriptor{"material.roughness","EditorUi.Roughness",PropertyType::Number,1.0f,0.0f,1.0f,true,{},PropertyPresentation::Slider},
    PropertyDescriptor{"material.doubleSided","EditorUi.DoubleSided",PropertyType::Boolean,false,std::nullopt,std::nullopt},
    PropertyDescriptor{"material.normalScale","Property.Material.NormalScale",PropertyType::Number,1.0f,std::nullopt,std::nullopt,true,{},PropertyPresentation::Default,true},
    PropertyDescriptor{"material.baseColorTexture","EditorUi.BaseColorTexture",PropertyType::AssetId,std::string{},std::nullopt,std::nullopt,true,"textures/baseColor"},
    PropertyDescriptor{"material.normalTexture","EditorUi.NormalTexture",PropertyType::AssetId,std::string{},std::nullopt,std::nullopt,true,"textures/normal"},
    PropertyDescriptor{"material.metallicRoughnessTexture","EditorUi.MetallicRoughnessTexture",PropertyType::AssetId,std::string{},std::nullopt,std::nullopt,true,"textures/metallicRoughness"},
    PropertyDescriptor{"material.shader","EditorUi.Shader",PropertyType::AssetId,std::string{},std::nullopt,std::nullopt,true,"shader",PropertyPresentation::Default,true},
    PropertyDescriptor{"material.alphaMode","EditorUi.AlphaMode",PropertyType::Text,std::string{"OPAQUE"},std::nullopt,std::nullopt,true,"alphaMode",PropertyPresentation::Default,true},
    PropertyDescriptor{"material.alphaCutoff","EditorUi.AlphaCutoff",PropertyType::Number,0.5f,0.f,std::nullopt,true,"alphaCutoff",PropertyPresentation::Default,true},
    PropertyDescriptor{"material.clearCoatWeight","Property.Material.ClearCoat.Weight",PropertyType::Number,ClearCoatParameters{}.weight,0.f,1.f,true,"clearCoat/weight",PropertyPresentation::Slider,true},
    PropertyDescriptor{"material.clearCoatRoughness","Property.Material.ClearCoat.Roughness",PropertyType::Number,ClearCoatParameters{}.roughness,0.f,1.f,true,"clearCoat/roughness",PropertyPresentation::Slider,true},
    PropertyDescriptor{"material.clearCoatNormalScale","Property.Material.ClearCoat.NormalScale",PropertyType::Number,ClearCoatParameters{}.normalScale,std::nullopt,std::nullopt,true,"clearCoat/normalScale",PropertyPresentation::Default,true},
    PropertyDescriptor{"material.clearCoatWeightTexture","Property.Material.ClearCoat.WeightTexture",PropertyType::AssetId,std::string{},std::nullopt,std::nullopt,true,"textures/clearCoatWeight",PropertyPresentation::Default,true},
    PropertyDescriptor{"material.clearCoatRoughnessTexture","Property.Material.ClearCoat.RoughnessTexture",PropertyType::AssetId,std::string{},std::nullopt,std::nullopt,true,"textures/clearCoatRoughness",PropertyPresentation::Default,true},
    PropertyDescriptor{"material.clearCoatNormalTexture","Property.Material.ClearCoat.NormalTexture",PropertyType::AssetId,std::string{},std::nullopt,std::nullopt,true,"textures/clearCoatNormal",PropertyPresentation::Default,true},
    PropertyDescriptor{"material.sheenColor","Property.Material.Sheen.Color",PropertyType::Vector3,SheenParameters{}.color,0.f,1.f,true,"sheen/color",PropertyPresentation::Color,true},
    PropertyDescriptor{"material.sheenRoughness","Property.Material.Sheen.Roughness",PropertyType::Number,SheenParameters{}.roughness,0.f,1.f,true,"sheen/roughness",PropertyPresentation::Slider,true},
    PropertyDescriptor{"material.sheenColorTexture","Property.Material.Sheen.ColorTexture",PropertyType::AssetId,std::string{},std::nullopt,std::nullopt,true,"textures/sheenColor",PropertyPresentation::Default,true},
    PropertyDescriptor{"material.sheenRoughnessTexture","Property.Material.Sheen.RoughnessTexture",PropertyType::AssetId,std::string{},std::nullopt,std::nullopt,true,"textures/sheenRoughness",PropertyPresentation::Default,true},
    PropertyDescriptor{"material.anisotropyStrength","Property.Material.Anisotropy.Strength",PropertyType::Number,AnisotropyParameters{}.strength,0.f,1.f,true,"anisotropy/strength",PropertyPresentation::Slider,true},
    PropertyDescriptor{"material.anisotropyRotation","Property.Material.Anisotropy.Rotation",PropertyType::Number,AnisotropyParameters{}.rotation,std::nullopt,std::nullopt,true,"anisotropy/rotation",PropertyPresentation::Default,true},
    PropertyDescriptor{"material.anisotropyTexture","Property.Material.Anisotropy.Texture",PropertyType::AssetId,std::string{},std::nullopt,std::nullopt,true,"textures/anisotropy",PropertyPresentation::Default,true},
    PropertyDescriptor{"material.subsurfaceWeight","Property.Material.Subsurface.Weight",PropertyType::Number,SubsurfaceParameters{}.weight,0.f,1.f,true,"subsurface/weight",PropertyPresentation::Slider,true},
    PropertyDescriptor{"material.subsurfaceRadius","Property.Material.Subsurface.Radius",PropertyType::Number,SubsurfaceParameters{}.radius,std::numeric_limits<float>::denorm_min(),1.f,true,"subsurface/radius",PropertyPresentation::Default,true},
    PropertyDescriptor{"material.subsurfaceColor","Property.Material.Subsurface.Color",PropertyType::Vector3,SubsurfaceParameters{}.color,0.f,1.f,true,"subsurface/color",PropertyPresentation::Color,true}};
}
std::span<const PropertyDescriptor> MaterialProperties(){return fields;}
const PropertyDescriptor* FindMaterialProperty(std::string_view key)
{for(const auto& field:fields)if(field.key==key)return &field;return nullptr;}
bool ReadMaterialProperty(const MaterialDocument& m,std::string_view key,PropertyValue& value,std::string& error)
{
    error.clear();if(!FindMaterialProperty(key)){error="Metadata.UnknownProperty";return false;}
    if(key=="material.name")value=m.name;
    else if(key=="material.baseColor")value=Vec4{m.baseColor[0],m.baseColor[1],m.baseColor[2],m.baseColor[3]};
    else if(key=="material.metallic")value=m.metallic;
    else if(key=="material.roughness")value=m.roughness;
    else if(key=="material.doubleSided")value=m.doubleSided;
    else if(key=="material.normalScale")value=m.normalScale;
    else if(key=="material.baseColorTexture")value=m.baseColorTexture;
    else if(key=="material.normalTexture")value=m.normalTexture;
    else if(key=="material.metallicRoughnessTexture")value=m.metallicRoughnessTexture;
    else if(key=="material.alphaMode")value=std::string(MaterialAlphaModeName(m.alphaMode));
    else if(key=="material.alphaCutoff")value=m.alphaCutoff;
    else if(key=="material.clearCoatWeight")value=m.clearCoat.weight;
    else if(key=="material.clearCoatRoughness")value=m.clearCoat.roughness;
    else if(key=="material.clearCoatNormalScale")value=m.clearCoat.normalScale;
    else if(key=="material.clearCoatWeightTexture")value=m.clearCoatWeightTexture;
    else if(key=="material.clearCoatRoughnessTexture")value=m.clearCoatRoughnessTexture;
    else if(key=="material.clearCoatNormalTexture")value=m.clearCoatNormalTexture;
    else if(key=="material.sheenColor")value=m.sheen.color;
    else if(key=="material.sheenRoughness")value=m.sheen.roughness;
    else if(key=="material.sheenColorTexture")value=m.sheenColorTexture;
    else if(key=="material.sheenRoughnessTexture")value=m.sheenRoughnessTexture;
    else if(key=="material.anisotropyStrength")value=m.anisotropy.strength;
    else if(key=="material.anisotropyRotation")value=m.anisotropy.rotation;
    else if(key=="material.anisotropyTexture")value=m.anisotropyTexture;
    else if(key=="material.subsurfaceWeight")value=m.subsurface.weight;
    else if(key=="material.subsurfaceRadius")value=m.subsurface.radius;
    else if(key=="material.subsurfaceColor")value=m.subsurface.color;
    else value=m.shader;
    return true;
}
bool WriteMaterialProperty(MaterialDocument& material,std::string_view key,const PropertyValue& value,std::string& error)
{
    error.clear();const auto* field=FindMaterialProperty(key);
    if(!field){error="Metadata.UnknownProperty";return false;}
    if(!field->editable||!ValidatePropertyValue(*field,value)){error="Metadata.InvalidValue";return false;}
    auto copy=material;
    if(key=="material.name")copy.name=std::get<std::string>(value);
    else if(key=="material.baseColor"){const auto v=std::get<Vec4>(value);copy.baseColor={v.x,v.y,v.z,v.w};}
    else if(key=="material.metallic")copy.metallic=std::get<float>(value);
    else if(key=="material.roughness")copy.roughness=std::get<float>(value);
    else if(key=="material.doubleSided")copy.doubleSided=std::get<bool>(value);
    else if(key=="material.normalScale")copy.normalScale=std::get<float>(value);
    else if(key=="material.baseColorTexture")copy.baseColorTexture=std::get<std::string>(value);
    else if(key=="material.normalTexture")copy.normalTexture=std::get<std::string>(value);
    else if(key=="material.metallicRoughnessTexture")copy.metallicRoughnessTexture=std::get<std::string>(value);
    else if(key=="material.alphaMode")
    {if(!ParseMaterialAlphaMode(std::get<std::string>(value),copy.alphaMode)){error="Metadata.InvalidValue";return false;}}
    else if(key=="material.alphaCutoff")copy.alphaCutoff=std::get<float>(value);
    else if(key=="material.clearCoatWeight")copy.clearCoat.weight=std::get<float>(value);
    else if(key=="material.clearCoatRoughness")copy.clearCoat.roughness=std::get<float>(value);
    else if(key=="material.clearCoatNormalScale")copy.clearCoat.normalScale=std::get<float>(value);
    else if(key=="material.clearCoatWeightTexture")copy.clearCoatWeightTexture=std::get<std::string>(value);
    else if(key=="material.clearCoatRoughnessTexture")copy.clearCoatRoughnessTexture=std::get<std::string>(value);
    else if(key=="material.clearCoatNormalTexture")copy.clearCoatNormalTexture=std::get<std::string>(value);
    else if(key=="material.sheenColor")copy.sheen.color=std::get<Vec3>(value);
    else if(key=="material.sheenRoughness")copy.sheen.roughness=std::get<float>(value);
    else if(key=="material.sheenColorTexture")copy.sheenColorTexture=std::get<std::string>(value);
    else if(key=="material.sheenRoughnessTexture")copy.sheenRoughnessTexture=std::get<std::string>(value);
    else if(key=="material.anisotropyStrength")copy.anisotropy.strength=std::get<float>(value);
    else if(key=="material.anisotropyRotation")copy.anisotropy.rotation=std::get<float>(value);
    else if(key=="material.anisotropyTexture")copy.anisotropyTexture=std::get<std::string>(value);
    else if(key=="material.subsurfaceWeight")copy.subsurface.weight=std::get<float>(value);
    else if(key=="material.subsurfaceRadius")copy.subsurface.radius=std::get<float>(value);
    else if(key=="material.subsurfaceColor")copy.subsurface.color=std::get<Vec3>(value);
    else copy.shader=std::get<std::string>(value);
    if(!ValidateMaterialDocument(copy)){error="Assets.InvalidMaterial";return false;}
    material=std::move(copy);return true;
}
}
