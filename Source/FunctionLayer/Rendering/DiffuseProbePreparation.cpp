#include "DiffuseProbePreparation.h"
#include "FunctionLayer/Runtime/World.h"
#include "FunctionLayer/Runtime/FeatureComponents.h"
#include "ResourceLayer/Assets/AssetStore.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "ResourceLayer/Assets/StaticModelPreparation.h"
#include "CoreLayer/Hash.h"
#include <algorithm>
#include <bit>
#include <map>
#include <set>

namespace se
{
namespace
{
bool Fail(std::vector<AssetDiagnostic>& diagnostics,const char* key,const std::string& detail)
{asset_io::Diagnostic(diagnostics,key,detail);return false;}
bool Active(const World& world,EntityHandle entity,ComponentTypeId type)
{const auto* component=world.GetComponent(world.FindComponent(entity,type));return component&&component->Active();}
bool StaticHierarchy(const World& world,EntityHandle entity)
{
    for(auto cursor=entity;cursor;cursor=world.Get(cursor)->parent)
    {
        if(Active(world,cursor,ActorSettingsComponent::TypeId)||Active(world,cursor,CharacterSettingsComponent::TypeId))return false;
        if(Active(world,cursor,PhysicsBodyComponent::TypeId))
        {const auto mass=FeatureValue(world,cursor,"body.mass");if(!std::holds_alternative<float>(mass)||std::get<float>(mass)>0)return false;}
        for(const auto handle:world.Components(cursor))
        {const auto* component=world.GetComponent(handle);if(component&&component->Active()&&component->Type()==ComponentTypes::Script)return false;}
    }
    return true;
}
bool Affine(const Mat4& m)
{return IsFinite(m)&&std::abs(m(3,0))<=1e-6f&&std::abs(m(3,1))<=1e-6f&&std::abs(m(3,2))<=1e-6f&&std::abs(m(3,3)-1)<=1e-6f&&Inverse(m).has_value();}
float Determinant(const Mat4& m)
{return Dot({m(0,0),m(1,0),m(2,0)},Cross({m(0,1),m(1,1),m(2,1)},{m(0,2),m(1,2),m(2,2)}));}
struct SourceRecord{std::filesystem::path absolute;std::string hash;};
using SourceMap=std::map<std::string,SourceRecord>;
bool Capture(const std::filesystem::path& path,AssetStore& assets,SourceMap& sources,std::vector<AssetDiagnostic>& diagnostics)
{
    const auto absolute=std::filesystem::weakly_canonical(path);
    if(!asset_io::Within(assets.SourceRoot(),absolute))return Fail(diagnostics,"DiffuseProbe.SceneInvalid","source outside content root");
    const auto relative="content/"+asset_io::Utf8(std::filesystem::relative(absolute,assets.SourceRoot()));
    std::vector<std::uint8_t> bytes;if(!asset_io::Read(absolute,bytes))return Fail(diagnostics,"Assets.ReadFailed",relative);
    const auto hash=Sha256(bytes);if(hash.empty())return Fail(diagnostics,"DiffuseProbe.SceneInvalid",relative);
    const auto found=sources.find(relative);
    if(found!=sources.end()&&found->second.hash!=hash)return Fail(diagnostics,"Assets.SourceChanged",relative);
    sources.insert_or_assign(relative,SourceRecord{absolute,hash});
    if(sources.size()>510)return Fail(diagnostics,"DiffuseProbe.BudgetExceeded","source count");
    return true;
}
bool CaptureAsset(const AssetId& id,AssetStore& assets,SourceMap& sources,std::vector<AssetDiagnostic>& diagnostics)
{
    const auto source=assets.Resolve(id);if(source.empty())return Fail(diagnostics,"DiffuseProbe.SceneInvalid",id);
    if(!Capture(source,assets,sources,diagnostics))return false;
    auto sidecar=source;sidecar+=".asset.json";
    return !std::filesystem::is_regular_file(sidecar)||Capture(sidecar,assets,sources,diagnostics);
}
struct UniformMaterial{Vec3 albedo{1,1,1};bool doubleSided=false;};
bool Skinned(const StaticModel& model)
{return !model.joints.empty()||!model.animations.empty()||std::any_of(model.meshes.begin(),model.meshes.end(),[](const auto& m){return m.skinned||!m.skinning.empty();});}
bool Material(const MaterialData& source,UniformMaterial& output,std::vector<AssetDiagnostic>& diagnostics)
{
    if(source.alphaMode!=MaterialAlphaMode::Opaque||source.baseColorTexture>=0||source.normalTexture>=0||source.metallicRoughnessTexture>=0||source.needsFallbackTexture)
        return Fail(diagnostics,"DiffuseProbe.UnsupportedSource","textured/non-Opaque model material: "+source.name);
    for(const auto c:source.baseColor)if(!IsFinite(c)||c<0||c>1)return Fail(diagnostics,"DiffuseProbe.SceneInvalid","material color");
    if(!IsFinite(source.metallic)||source.metallic<0||source.metallic>1)return Fail(diagnostics,"DiffuseProbe.SceneInvalid","material metallic");
    output={{source.baseColor[0]*(1-source.metallic),source.baseColor[1]*(1-source.metallic),source.baseColor[2]*(1-source.metallic)},source.doubleSided};return true;
}
bool Material(const MaterialDocument& source,UniformMaterial& output,std::vector<AssetDiagnostic>& diagnostics)
{
    if(!ValidateMaterialDocument(source))return Fail(diagnostics,"DiffuseProbe.SceneInvalid","material document");
    if(source.alphaMode!=MaterialAlphaMode::Opaque||!source.baseColorTexture.empty()||!source.normalTexture.empty()||!source.metallicRoughnessTexture.empty()||!source.shader.empty())
        return Fail(diagnostics,"DiffuseProbe.UnsupportedSource","textured/non-Opaque/custom-shader material: "+source.name);
    output={{source.baseColor[0]*(1-source.metallic),source.baseColor[1]*(1-source.metallic),source.baseColor[2]*(1-source.metallic)},source.doubleSided};return true;
}
void U32(std::vector<std::uint8_t>& bytes,std::uint32_t value)
{for(unsigned i=0;i<4;++i)bytes.push_back(static_cast<std::uint8_t>(value>>(8*i)));}
void F3(std::vector<std::uint8_t>& bytes,Vec3 value)
{for(const auto v:{value.x,value.y,value.z})U32(bytes,std::bit_cast<std::uint32_t>(v));}
}
bool PrepareDiffuseProbeScene(const World& world,AssetStore& assets,PreparedDiffuseProbeScene& output,std::vector<AssetDiagnostic>& diagnostics)
{
    try
    {
        PreparedDiffuseProbeScene prepared;SourceMap sources;std::set<std::uint64_t> ids;
        auto entities=world.Entities();
        std::sort(entities.begin(),entities.end(),[&](auto a,auto b){return world.Get(a)->stableId<world.Get(b)->stableId;});
        for(const auto entity:entities)
        {
            if(!world.IsEnabled(entity))continue;
            const auto id=world.Get(entity)->stableId;const auto label=std::to_string(id);
            const auto* component=world.GetMesh(entity);const auto* light=world.GetLight(entity);
            const bool hasMesh=component&&Active(world,entity,ComponentTypes::Mesh),hasLight=light&&Active(world,entity,ComponentTypes::Light);
            if(!hasMesh&&!hasLight)continue;
            if(!StaticHierarchy(world,entity))return Fail(diagnostics,"DiffuseProbe.UnsupportedSource","dynamic hierarchy: "+label);
            const auto entityWorld=world.GetWorldMatrix(entity);if(!Affine(entityWorld))return Fail(diagnostics,"DiffuseProbe.SceneInvalid","transform: "+label);
            ids.insert(id);
            if(hasLight)
            {
                if(light->type!=LightType::Point)return Fail(diagnostics,"DiffuseProbe.UnsupportedSource","non-point light: "+label);
                if(!IsFinite(light->color)||light->color.x<0||light->color.y<0||light->color.z<0||!IsFinite(light->intensity)||light->intensity<0
                    ||!IsFinite(light->range)||light->range<.05f||light->range>10000)return Fail(diagnostics,"DiffuseProbe.SceneInvalid","point light: "+label);
                const auto radiance=light->color*light->intensity;
                if(!IsFinite(radiance)||radiance.x>10000||radiance.y>10000||radiance.z>10000)return Fail(diagnostics,"DiffuseProbe.UnsupportedSource","point light requires runtime clamping: "+label);
                const auto position=TransformPoint(entityWorld,{});
                if(std::abs(position.x)>1e6f||std::abs(position.y)>1e6f||std::abs(position.z)>1e6f)return Fail(diagnostics,"DiffuseProbe.UnsupportedSource","point position requires runtime clamping: "+label);
                prepared.lights.push_back({position,radiance,light->range});
                if(prepared.lights.size()>256)return Fail(diagnostics,"DiffuseProbe.BudgetExceeded","point lights");
            }
            if(!hasMesh)continue;
            StaticModel builtin;std::shared_ptr<const StaticModel> owned;const StaticModel* model=nullptr;
            if(IsBuiltinMesh(component->meshAsset))
            {
                if(component->materials.empty()||component->materials[0].empty())return Fail(diagnostics,"DiffuseProbe.UnsupportedSource","builtin requires explicit uniform material: "+label);
                auto mesh=component->meshAsset==BuiltinCubeAssetId?MakeUnitCube():MakeUnitSphere();mesh.material=0;
                builtin.meshes.push_back(std::move(mesh));builtin.materials.emplace_back();ModelNode node;node.meshes={0};builtin.nodes.push_back(node);model=&builtin;
            }
            else
            {
                if(!CaptureAsset(component->meshAsset,assets,sources,diagnostics))return false;
                owned=assets.LoadModel(component->meshAsset,diagnostics);if(!owned)return Fail(diagnostics,"DiffuseProbe.SceneInvalid","model: "+label);model=owned.get();
                if(Skinned(*model))return Fail(diagnostics,"DiffuseProbe.UnsupportedSource","skinned/animated model: "+label);
                for(const auto& dependency:model->dependencies)
                {
                    std::filesystem::path path;std::string relative;
                    if(!asset_io::ResolveRelative(assets.Resolve(component->meshAsset),assets.SourceRoot(),dependency,path,relative)||!Capture(path,assets,sources,diagnostics))return false;
                }
            }
            if(Skinned(*model))
                return Fail(diagnostics,"DiffuseProbe.UnsupportedSource","skinned/animated model: "+label);
            StaticModelLayout layout;if(!BuildStaticModelLayout(*model,layout,diagnostics))return Fail(diagnostics,"DiffuseProbe.SceneInvalid","model layout: "+label);
            std::map<std::size_t,std::shared_ptr<const MaterialDocument>> overrides;
            for(std::size_t slot=0;slot<component->materials.size();++slot)
            {
                const auto& materialId=component->materials[slot];if(materialId.empty())continue;
                if(!CaptureAsset(materialId,assets,sources,diagnostics))return false;
                auto material=assets.LoadMaterial(materialId,diagnostics);if(!material)return Fail(diagnostics,"DiffuseProbe.SceneInvalid","material: "+label);
                overrides.emplace(slot,std::move(material));
            }
            for(const auto& part:layout.parts)
            {
                const auto& mesh=model->meshes[part.mesh];UniformMaterial material;
                if(mesh.material>=0)
                {
                    const auto slot=static_cast<std::size_t>(mesh.material);const auto found=overrides.find(slot);
                    if(found!=overrides.end()){if(!Material(*found->second,material,diagnostics))return false;}
                    else if(!Material(model->materials[slot],material,diagnostics))return false;
                }
                const auto matrix=entityWorld*part.modelFromMesh;if(!Affine(matrix))return Fail(diagnostics,"DiffuseProbe.SceneInvalid","mesh transform: "+label);
                const bool mirrored=Determinant(matrix)<0;
                for(std::size_t index=0;index<mesh.indices.size();index+=3)
                {
                    const auto position=[&](std::uint32_t vertex){const auto& p=mesh.vertices[vertex].position;return TransformPoint(matrix,{p[0],p[1],p[2]});};
                    DiffuseBakeTriangle triangle{position(mesh.indices[index]),position(mesh.indices[index+1]),position(mesh.indices[index+2]),material.albedo,material.doubleSided};
                    if(mirrored)std::swap(triangle.b,triangle.c);
                    const auto area=LengthSquared(Cross(triangle.b-triangle.a,triangle.c-triangle.a));
                    if(!IsFinite(triangle.a)||!IsFinite(triangle.b)||!IsFinite(triangle.c)||!IsFinite(area)||area<1e-16f)return Fail(diagnostics,"DiffuseProbe.SceneInvalid","degenerate triangle: "+label);
                    prepared.triangles.push_back(triangle);
                    if(prepared.triangles.size()>4096)return Fail(diagnostics,"DiffuseProbe.BudgetExceeded","triangles");
                }
            }
        }
        if(prepared.triangles.empty())return Fail(diagnostics,"DiffuseProbe.SceneInvalid","no static surfaces");
        std::vector<std::uint8_t> identity;const std::string algorithm=DiffuseProbeCookerVersion;identity.insert(identity.end(),algorithm.begin(),algorithm.end());
        prepared.sourceEntityIds.assign(ids.begin(),ids.end());
        for(const auto id:prepared.sourceEntityIds){U32(identity,static_cast<std::uint32_t>(id));U32(identity,static_cast<std::uint32_t>(id>>32));}
        for(const auto& [path,source]:sources)
        {
            std::vector<std::uint8_t> bytes;if(!asset_io::Read(source.absolute,bytes)||Sha256(bytes)!=source.hash)return Fail(diagnostics,"Assets.SourceChanged",path);
            prepared.sources.push_back({path,source.hash});U32(identity,static_cast<std::uint32_t>(path.size()));identity.insert(identity.end(),path.begin(),path.end());identity.insert(identity.end(),source.hash.begin(),source.hash.end());
        }
        U32(identity,static_cast<std::uint32_t>(prepared.triangles.size()));
        for(const auto& t:prepared.triangles){F3(identity,t.a);F3(identity,t.b);F3(identity,t.c);F3(identity,t.baseColor);identity.push_back(t.doubleSided?1:0);}
        U32(identity,static_cast<std::uint32_t>(prepared.lights.size()));for(const auto& light:prepared.lights){F3(identity,light.position);F3(identity,light.radiance);U32(identity,std::bit_cast<std::uint32_t>(light.range));}
        prepared.contentHash=Sha256(identity);if(prepared.contentHash.empty())return Fail(diagnostics,"DiffuseProbe.SceneInvalid","content hash");
        output=std::move(prepared);return true;
    }
    catch(const std::exception&){return Fail(diagnostics,"DiffuseProbe.SceneInvalid","preparation exception");}
}
}
