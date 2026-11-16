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
bool AddModelToScene(EditorSession& session, AssetStore& assets, const std::string& asset, std::string& error)
{
    if(session.IsPlaying()){error="Editor.EditWhilePlaying";return false;}
    std::vector<AssetDiagnostic> diagnostics;
    const auto model=assets.LoadModel(asset,diagnostics);
    if(!model){error=diagnostics.empty()?"EditorUi.AnimationModelUnavailable":diagnostics.back().key;return false;}
    if(!session.BeginEdit("Editor.Create")){error=session.LastError();return false;}
    const auto id=session.Create(asset_io::Utf8(assets.Resolve(asset).stem()));
    bool ready=id&&session.SetMesh(id,{asset,{}});
    if(ready&&!model->joints.empty()&&!model->animations.empty())
    {
        const auto idle=std::find_if(model->animations.begin(),model->animations.end(),[](const auto& clip){return clip.name=="Idle";});
        const auto& clip=idle==model->animations.end()?model->animations.front():*idle;
        ready=session.Apply("Feature.Edit",[&](World& world){
            const auto entity=world.FindByStableId(id);
            return AddComponent(world,entity,"actor",error)
                &&WriteProperty(world,entity,"actor.asset",asset,error)
                &&WriteProperty(world,entity,"actor.clip",clip.name,error);
        });
    }
    if(ready&&session.CommitEdit())return true;
    if(error.empty())error=session.LastError();
    session.CancelEdit();
    return false;
}

bool CreateEditorObject(EditorSession& session, std::string_view kind, StableEntityId parent,const Localization* text)
{
    const bool mesh = kind == "cube" || kind == "sphere" ||kind=="physics-cube";
    const auto feature=kind=="render-settings"?"render":kind=="environment-settings"?"environmentmap":kind=="rectangle-light"?"rectangle":kind=="physics-cube"?"body":kind=="character"?"character":"";
    if (!mesh && kind != "empty" && kind != "camera" && kind != "light"&&!*feature) return false;
    if (!session.BeginEdit("Editor.Create")) return false;
    const char* name = kind == "cube" ? "Cube" : kind == "sphere" ? "Sphere" :
                       kind == "light" ? "Directional Light" : kind == "camera" ? "Camera" : "GameObject";
    const auto* descriptor=FindComponentDescriptor(feature);
    const auto id = session.Create(descriptor?(text?text->GetText(std::string(descriptor->labelKey)):std::string(descriptor->key)):name, parent);
    bool ready = id != 0;
    if (ready && mesh)
        ready = session.SetMesh(id, {kind == "sphere" ? BuiltinSphereAssetId : BuiltinCubeAssetId, {}});
    if (ready && (kind == "camera" || kind == "light"))
        ready = session.Apply("Editor.Create", [&](World& world) {
            const auto entity = world.FindByStableId(id);
            return kind == "camera" ? world.SetCamera(entity, {}) : world.SetLight(entity, {});
        });
    if(ready&&*feature)ready=session.Apply("Feature.Edit",[&](World& world){std::string error;return AddComponent(world,world.FindByStableId(id),feature,error);});
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
