#include "EditorCreation.h"
#include "EditorAssetActions.h"
#include "EditorScriptTools.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Assets/AssetStore.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "FunctionLayer/Runtime/FeatureComponents.h"
#include "CoreLayer/Localization.h"
#include <algorithm>

namespace se
{
bool SpawnModelEntity(World& world, AssetStore& assets, const std::string& asset,
                      const std::string& name, const Transform& transform,
                      StableEntityId& createdId, std::string& error)
{
    createdId = 0;
    std::vector<AssetDiagnostic> diagnostics;
    const auto model = assets.LoadModel(asset, diagnostics);
    if (!model)
    {
        error = diagnostics.empty() ? "EditorUi.AnimationModelUnavailable" : diagnostics.back().key;
        return false;
    }
    const std::string resolvedName = name.empty() ? asset_io::Utf8(assets.Resolve(asset).stem()) : name;
    const EntityHandle entity = world.Create(resolvedName);
    if (!entity || !world.SetTransform(entity, transform) || !world.SetMesh(entity, {asset, {}}))
    {
        error = world.LastError();
        if (entity) { world.Destroy(entity); world.FlushCommands(); }
        return false;
    }
    if (!model->joints.empty() && !model->animations.empty())
    {
        const auto idle = std::find_if(model->animations.begin(), model->animations.end(),
                                       [](const auto& clip) { return clip.name == "Idle"; });
        const auto& clip = idle == model->animations.end() ? model->animations.front() : *idle;
        if (!AddComponent(world, entity, "actor", error) ||
            !WriteProperty(world, entity, "actor.asset", asset, error) ||
            !WriteProperty(world, entity, "actor.clip", clip.name, error))
        {
            const StableEntityId id = world.Get(entity)->stableId;
            world.Destroy(entity);
            world.FlushCommands();
            world.ForgetStableId(id);
            return false;
        }
    }
    if (!world.FlushCommands())
    {
        error = world.LastError();
        return false;
    }
    createdId = world.Get(entity)->stableId;
    error.clear();
    return true;
}

bool AddModelToScene(EditorSession& session, AssetStore& assets, const std::string& asset, std::string& error)
{
    if(session.IsPlaying()){error="Editor.EditWhilePlaying";return false;}
    if(!session.BeginEdit("Editor.Create")){error=session.LastError();return false;}
    StableEntityId id = 0;
    const std::string name = asset_io::Utf8(assets.Resolve(asset).stem());
    const bool ok = session.Apply("Editor.Create", [&](World& world) {
        return SpawnModelEntity(world, assets, asset, name, Transform{}, id, error);
    });
    if (ok && session.Select(id) && session.CommitEdit()) return true;
    if (error.empty()) error = session.LastError();
    session.CancelEdit();
    return false;
}

bool CreateEditorObject(EditorSession& session, std::string_view kind, StableEntityId parent,const Localization* text)
{
    const bool mesh = kind == "cube" || kind == "sphere" || kind == "physics-cube" || kind == "physics-sphere";
    const bool light=kind=="light"||kind=="point-light"||kind=="spot-light";
    const auto feature=kind=="render-settings"?"render":kind=="environment-settings"?"environmentmap":kind=="rectangle-light"?"rectangle":(kind=="physics-cube"||kind=="physics-sphere")?"body":kind=="trigger-box"?"collider":kind=="character"?"character":kind=="audio-source"?"audio":kind=="audio-listener"?"listener":kind=="particle-emitter"?"emitter":"";
    if (!mesh && kind != "empty" && kind != "camera" && !light&&!*feature) return false;
    if (!session.BeginEdit("Editor.Create")) return false;
    const char* name = kind == "cube" ? "Cube" : kind == "sphere" ? "Sphere" :
                       kind == "light" ? "Directional Light" : kind == "camera" ? "Camera" : "GameObject";
    const auto* descriptor=FindComponentDescriptor(feature);
    const char* localLabel=kind=="point-light"?"EditorUi.LightPoint":kind=="spot-light"?"EditorUi.LightSpot":kind=="physics-sphere"?"EditorUi.CreatePhysicsSphere":kind=="trigger-box"?"EditorUi.CreateTriggerBox":kind=="audio-source"?"EditorUi.CreateAudioSource":nullptr;
    const auto id = session.Create(localLabel?(text?text->GetText(localLabel):std::string(kind)):
        descriptor?(text?text->GetText(std::string(descriptor->labelKey)):std::string(descriptor->key)):name, parent);
    bool ready = id != 0;
    if (ready && mesh)
        ready = session.SetMesh(id, {(kind == "sphere" || kind == "physics-sphere") ? BuiltinSphereAssetId : BuiltinCubeAssetId, {}});
    if (ready && (kind == "camera" || light))
        ready = session.Apply("Editor.Create", [&](World& world) {
            const auto entity = world.FindByStableId(id);
            if(kind=="camera")return world.SetCamera(entity,{});
            LightComponent component;
            if(kind=="point-light")component.type=LightType::Point;
            if(kind=="spot-light")component.type=LightType::Spot;
            component.castsShadow=component.type!=LightType::Directional;
            return world.SetLight(entity,component);
        });
    if(ready&&*feature)ready=session.Apply("Feature.Edit",[&](World& world){
        std::string error;
        const auto target=world.FindByStableId(id);
        if(!AddComponent(world,target,feature,error))return false;
        if(kind=="physics-sphere")
            return AddComponent(world,target,"collider",error)&&WriteProperty(world,target,"collider.shape",1.0f,error);
        if(kind=="trigger-box")
            return WriteProperty(world,target,"collider.isTrigger",true,error);
        return true;
    });
    if (ready && session.CommitEdit()) return true;
    session.CancelEdit();
    return false;
}

bool CreateEditorAsset(std::string_view kind, const std::filesystem::path& project, AssetStore& assets,
                       std::filesystem::path& created, std::string& error)
{
    created.clear();
    if (kind != "material" && kind != "script"&&kind!="tod") { error = "Editor.OperationFailed"; return false; }
    for (unsigned index = 0; index < 10000; ++index)
    {
        const auto suffix = index ? std::to_string(index) : std::string{};
        if (kind == "script")
        {
            const auto name = "NewBehaviour" + suffix;
            EditorScriptCreateResult result;
            std::string detail;
            if (EditorScriptTools::CreateBehaviour({project, name, "Game"}, result, error, detail))
            { created = result.source; return true; }
            if (error == "Script.TemplateConflict" && asset_io::FromUtf8(detail).filename() == name + ".cs") continue;
            return false;
        }
        if(kind=="tod")
        {
            const auto path=assets.SourceRoot()/("DayCycle"+suffix+".tod");if(std::filesystem::exists(path))continue;
            if(!SaveTODProfile(path,TODProfile{},error))return false;AssetId id;std::vector<AssetDiagnostic> diagnostics;
            if(!assets.RegisterSource(path,{},id,diagnostics)){error=diagnostics.empty()?"Assets.WriteFailed":diagnostics.back().key;return false;}
            created=path;std::vector<EditorAssetIssue> issues;if(EditorAssetActions::SyncProjectAssets(project,assets,issues))return true;error=issues.empty()?"Assets.WriteFailed":issues.front().key;return false;
        }
        MaterialDocument material;
        material.name = "NewMaterial" + suffix;
        const auto path = assets.SourceRoot() / (material.name + ".material.json");
        AssetId id;
        std::vector<AssetDiagnostic> diagnostics;
        if (!assets.SaveMaterialSource(path, material, id, diagnostics, false))
        {
            error = diagnostics.empty() ? "Assets.WriteFailed" : diagnostics.back().key;
            if (error == "Assets.DestinationExists") continue;
            return false;
        }
        created = assets.Resolve(id);
        std::vector<EditorAssetIssue> issues;
        if (EditorAssetActions::SyncProjectAssets(project, assets, issues)) return true;
        error = issues.empty() ? "Assets.WriteFailed" : issues.front().key;
        return false;
    }
    error = "Assets.DestinationExists";
    return false;
}
}
