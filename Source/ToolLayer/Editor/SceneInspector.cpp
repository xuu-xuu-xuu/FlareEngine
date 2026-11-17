#include "SceneInspector.h"
#include "FeatureInspector.h"
#include "EditorSession.h"
#include "FunctionLayer/Runtime/FeatureComponents.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Log.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Assets/AssetStore.h"
#include "ResourceLayer/Assets/Primitives.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <sstream>

namespace se
{
namespace
{
using ui::editor::EditorProperty;
using ui::editor::EditorPropertyType;
std::string Number(float value)
{
    if (value == 0.0f) return "0";
    char result[64];
    const auto [end, status] = std::to_chars(result, result + sizeof(result), value);
    return status == std::errc{} ? std::string(result, end) : std::to_string(value);
}

// Z、X、Y 顺序（列向量）。只在编辑器边界转换；场景继续序列化四元数。
Vec3 EulerDegrees(Quat orientation)
{
    const auto m = Rotation(orientation);
    const float cosine = std::hypot(m(1,0), m(1,1));
    const float x = std::atan2(-m(1,2), cosine);
    const bool singular = cosine < 0.00001f;
    const float y = singular ? std::atan2(-m(2,0), m(0,0)) : std::atan2(m(0,2), m(2,2));
    const float z = singular ? 0 : std::atan2(m(1,0), m(1,1));
    return (180.0f / Pi) * Vec3{x, y, z};
}

Quat FromEulerDegrees(Vec3 angles)
{
    const auto r = (Pi / 180.0f) * angles;
    return Normalize(AxisAngle({0,1,0}, r.y) * AxisAngle({1,0,0}, r.x) * AxisAngle({0,0,1}, r.z));
}

void Vector(std::vector<EditorProperty>& rows, const std::string& section, std::string_view key,
            const std::string& label, Vec3 v, bool locked)
{
    const float values[] = {v.x, v.y, v.z};
    for (int axis = 0; axis < 3; ++axis)
    {
        rows.push_back({std::string(key) + "." + "xyz"[axis], label, EditorPropertyType::Number,
                        Number(values[axis]), locked, {}, section});
        rows.back().axis = "XYZ"[axis];
        rows.back().dragStep = key == "rotation" ? .25f : .01f;
    }
}
}

void AppendSceneInspector(const EditorSession& session, const AssetStore& assets, const Localization& text,
                          std::uint64_t id, std::vector<EditorProperty>& rows)
{
    const auto& world = session.EditWorld();
    const auto entity = world.FindByStableId(id);
    const bool locked = session.IsPlaying();
    const auto add = [&](std::string key, const char* label, EditorPropertyType type, std::string value,
                         const char* section, bool folded = false) -> EditorProperty& {
        rows.push_back({std::move(key), text.GetText(label), type, std::move(value), locked, {},
                        section ? text.GetText(section) : std::string{}, folded});
        return rows.back();
    };
    if (const auto* record = world.Get(entity))
    {
        add("name", "EditorUi.Name", EditorPropertyType::String, record->name, nullptr);
        if (const auto* t = world.GetTransform(entity))
        {
            const auto group = text.GetText("Component.Transform");
            Vector(rows, group, "position", text.GetText("EditorUi.Position"), t->position, locked);
            Vector(rows, group, "rotation", text.GetText("EditorUi.Rotation"), EulerDegrees(t->rotation), locked);
            Vector(rows, group, "scale", text.GetText("EditorUi.Scale"), t->scale, locked);
        }
        if (const auto* c = world.GetCamera(entity))
        {
            // 度数仅属于属性界面；运行时、反射与场景格式继续使用弧度。
            add("camera.fov", "EditorUi.CameraFovDegrees", EditorPropertyType::Number,
                Number(c->verticalFovRadians * (180.0f / Pi)), "Component.Camera");
            add("camera.near", "EditorUi.CameraNearClip", EditorPropertyType::Number, Number(c->nearPlane), "Component.Camera");
            add("camera.far", "EditorUi.CameraFarClip", EditorPropertyType::Number, Number(c->farPlane), "Component.Camera");
            add("camera.primary", "Property.Camera.Primary", EditorPropertyType::Boolean,c->primary?"true":"false","Component.Camera");
            for (auto& row : rows)
            {
                if (row.key == "camera.fov") {row.dragStep = .1f; row.dragMinimum = .1f; row.dragMaximum = 179.9f;}
                if (row.key == "camera.near") {row.dragStep = .01f; row.dragMinimum = .0001f; row.dragMaximum = std::nextafter(c->farPlane, 0.0f);}
                if (row.key == "camera.far") {row.dragStep = .1f; row.dragMinimum = std::nextafter(c->nearPlane, INFINITY);}
            }
        }
        if (const auto* l = world.GetLight(entity))
        {
            auto& type = add("light.type", "EditorUi.LightType", EditorPropertyType::Combo,
                std::string(LightTypeName(l->type)), "Component.Light");
            type.options = {"directional", "point", "spot"};
            type.optionLabels = {text.GetText("EditorUi.LightDirectional"), text.GetText("EditorUi.LightPoint"),text.GetText("EditorUi.LightSpot")};
            type.displayValue = type.optionLabels[static_cast<std::size_t>(l->type)];
            add("light.color", "EditorUi.LightColor", EditorPropertyType::Color,
                Number(l->color.x) + " " + Number(l->color.y) + " " + Number(l->color.z), "Component.Light");
            const float channels[]{l->color.x, l->color.y, l->color.z};
            for (int channel = 0; channel < 3; ++channel)
            {
                auto& row = add(std::string("light.color.") + "rgb"[channel], "EditorUi.LightRgb", EditorPropertyType::Number,
                    Number(channels[channel]), "Component.Light");
                row.axis = "RGB"[channel]; row.dragStep = .01f; row.dragMinimum = 0.0f; row.dragMaximum = 1.0f;
            }
            auto& intensity = add("light.intensity", "EditorUi.LightIntensity", EditorPropertyType::Number, Number(l->intensity), "Component.Light");
            intensity.dragStep = .01f; intensity.dragMinimum = 0.0f;
            auto& range = add("light.range", "EditorUi.LightRange", EditorPropertyType::Number, Number(l->range), "Component.Light");
            range.dragStep = .05f; range.dragMinimum = .05f; range.dragMaximum = 10000.0f;
            if (l->type == LightType::Directional) {range.readOnly = true; range.displayValue = text.GetText("EditorUi.LightInfiniteRange");}
            else
                add("light.castsShadow","Property.Light.CastsShadow",EditorPropertyType::Boolean,l->castsShadow?"true":"false","Component.Light");
            if(l->type==LightType::Spot)
            {
                const auto angle=[&](const char* key,const char* label,float value,float minimum,float maximum){
                    auto& row=add(key,label,EditorPropertyType::Number,Number(value*180/Pi),"Component.Light");
                    row.dragStep=.25f;row.dragMinimum=minimum*180/Pi;row.dragMaximum=maximum*180/Pi;
                };
                angle("light.innerAngle","EditorUi.LightInnerDegrees",l->innerAngle,0,l->outerAngle);
                angle("light.outerAngle","EditorUi.LightOuterDegrees",l->outerAngle,std::max(.01f,l->innerAngle),1.55f);
            }
        }
        if (const auto* mesh = world.GetMesh(entity))
        {
            const auto reference = [&](const char* key, const char* label, const std::string& asset) {
                auto& field = add(key, label, EditorPropertyType::Asset, asset, "Component.Mesh");
                if (asset.empty()) field.displayValue = text.GetText("EditorUi.None");
                else if (asset == BuiltinCubeAssetId) field.displayValue = text.GetText("EditorUi.BuiltinCube");
                else if (asset == BuiltinSphereAssetId) field.displayValue = text.GetText("EditorUi.BuiltinSphere");
                else field.displayValue = asset_io::Utf8(assets.Resolve(asset).filename());
            };
            reference("mesh.asset", "EditorUi.MeshAsset", mesh->meshAsset);
            reference("mesh.material", "EditorUi.MaterialSlot", mesh->materials.empty() ? "" : mesh->materials.front());
            for(std::size_t slot=1;slot<mesh->materials.size();++slot)
            {
                reference(("mesh.material."+std::to_string(slot)).c_str(),"EditorUi.MaterialSlot",mesh->materials[slot]);
                rows.back().label+=" "+std::to_string(slot+1);
            }
            add("mesh.slot.add","Feature.Mesh.AddSlot",EditorPropertyType::Action,{},"Component.Mesh");
            if(mesh->materials.size()>1)add("mesh.slot.remove","Feature.Mesh.RemoveSlot",EditorPropertyType::Action,{},"Component.Mesh");
            add("mesh.castsShadow","Property.Mesh.CastsShadow",EditorPropertyType::Boolean,mesh->castsShadow?"true":"false","Component.Mesh");
            add("mesh.receivesShadow","Property.Mesh.ReceivesShadow",EditorPropertyType::Boolean,mesh->receivesShadow?"true":"false","Component.Mesh");
        }
        AppendFeatureInspector(session,text,id,rows);
        for(auto& field:rows)
        {
            if(field.key=="mesh.asset"||field.key=="actor.asset"){
                field.type=EditorPropertyType::Combo;
                if(field.value.empty())field.displayValue=text.GetText("EditorUi.None");
                else if(field.displayValue.empty()){
                    auto source=assets.Resolve(field.value);
                    field.displayValue=source.empty()?field.value:asset_io::Utf8(source.filename());}
            }else if(field.key=="actor.clip")field.type=EditorPropertyType::Combo;
        }
        add("stableId", "EditorUi.StableId", EditorPropertyType::String, std::to_string(id),
            "EditorUi.ObjectDetails", true).readOnly = true;
        auto component = [&](const char* key, const char* title) {
            add(key, title, EditorPropertyType::Action, {}, "EditorUi.AddComponent", true);
        };
        if (!world.GetCamera(entity)) component("add.camera", "Component.Camera");
        if (!world.GetMesh(entity)) component("add.mesh", "Component.Mesh");
        if (!world.GetLight(entity)) component("add.light", "Component.Light");
    }
    const auto settings = world.EnvironmentSettings();
    add("environment.todProfile", "EditorUi.TodProfile", EditorPropertyType::Asset, settings.todProfile,
        "EditorUi.SceneEnvironment", id != 0);
    add("environment.preserveTime", "EditorUi.TodPreserveTime", EditorPropertyType::Boolean,
        settings.preserveTime ? "true" : "false", "EditorUi.SceneEnvironment", id != 0);
}

std::string FillSceneInspectorChoices(const EditorSession& session,AssetStore& assets,const Localization& text,
                                      std::uint64_t id,ui::editor::EditorProperty& field)
{
    if(field.key=="mesh.asset"||field.key=="actor.asset")
    {
        const bool mesh=field.key=="mesh.asset";
        field.options={""};field.optionLabels={text.GetText("EditorUi.None")};
        if(mesh){field.options.insert(field.options.end(),{BuiltinCubeAssetId,BuiltinSphereAssetId});
            field.optionLabels.insert(field.optionLabels.end(),{text.GetText("EditorUi.BuiltinCube"),text.GetText("EditorUi.BuiltinSphere")});}
        std::vector<std::pair<std::string,AssetId>> entries;
        for(const auto& asset:assets.ListIds()){
            const auto path=assets.Resolve(asset);auto ext=asset_io::Utf8(path.extension());
            std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char ch){return char(std::tolower(ch));});
            if(ext==".glb"||ext==".gltf"||(mesh&&ext==".fbx"))
                entries.emplace_back(asset_io::Utf8(path.lexically_relative(assets.SourceRoot())),asset);}
        std::sort(entries.begin(),entries.end());
        for(const auto& [label,asset]:entries){field.options.push_back(asset);field.optionLabels.push_back(label);}
    }
    else if(field.key=="actor.clip")
    {
        const auto& world=session.EditWorld();const auto entity=world.FindByStableId(id);
        if(!entity)return text.GetText("EditorUi.AnimationChooseModel");
        const auto asset=std::get<std::string>(FeatureValue(world,entity,"actor.asset"));
        if(asset.empty())return text.GetText("EditorUi.AnimationChooseModel");
        std::vector<AssetDiagnostic> diagnostics;auto model=assets.LoadModel(asset,diagnostics);
        if(!model)return text.GetText("EditorUi.AnimationModelUnavailable");
        for(const auto& clip:model->animations)
            if(!clip.name.empty()&&std::find(field.options.begin(),field.options.end(),clip.name)==field.options.end())field.options.push_back(clip.name);
        if(field.options.empty())return text.GetText("EditorUi.AnimationNoClips");
    }
    return {};
}

bool ApplySceneInspector(EditorSession& session, std::uint64_t id, const std::string& key, const std::string& value)
{
    if (session.IsPlaying()) return false;
    const auto& world = session.EditWorld();
    const auto entity = world.FindByStableId(id);
    if(const auto result=ApplyFeatureInspector(session,id,key,value))return *result;
    if(key=="light.castsShadow"&&(!world.GetLight(entity)||world.GetLight(entity)->type==LightType::Directional))return false;
    if(key=="camera.primary"||key=="mesh.castsShadow"||key=="mesh.receivesShadow"||key=="light.castsShadow")
        return (value=="true"||value=="false")&&session.SetProperty(id,key,value=="true");
    if(key.starts_with("mesh.material.")||key.starts_with("mesh.slot."))
    {
        const auto* mesh=world.GetMesh(entity);if(!mesh)return false;auto next=*mesh;
        if(key=="mesh.slot.add"){if(next.materials.size()>=256)return false;next.materials.push_back({});}
        else if(key=="mesh.slot.remove"){if(next.materials.size()<2)return false;next.materials.pop_back();}
        else {std::size_t slot=0;const auto raw=std::string_view(key).substr(14);const auto [end,error]=std::from_chars(raw.data(),raw.data()+raw.size(),slot);if(error!=std::errc{}||end!=raw.data()+raw.size()||slot>=next.materials.size())return false;next.materials[slot]=value;}
        return session.SetMesh(id,next);
    }
    if (key == "name")
    {
        const bool saved = session.Rename(id, value);
        if (saved) Log(LogLevel::Info, "EditorUi.ToolkitProperty: name");
        return saved;
    }
    if (key.starts_with("add."))
        return session.Apply("EditorUi.AddComponent", [&](World& edit) {
            const auto target = edit.FindByStableId(id);
            if (key == "add.camera") return !edit.GetCamera(target) && edit.SetCamera(target, {});
            if (key == "add.light") return !edit.GetLight(target) && edit.SetLight(target, {});
            return key == "add.mesh" && !edit.GetMesh(target) && edit.SetMesh(target, {});
        });
    if (key == "mesh.asset" || key == "mesh.material")
    {
        const auto* mesh = world.GetMesh(entity);
        if (!mesh) return false;
        auto next = *mesh;
        if (key == "mesh.asset") next.meshAsset = value;
        else if (next.materials.empty()) next.materials.push_back(value);
        else next.materials.front() = value;
        return session.SetMesh(id, next);
    }
    if (key == "environment.todProfile" || key == "environment.preserveTime")
    {
        auto settings = world.EnvironmentSettings();
        if (key == "environment.todProfile") settings.todProfile = value;
        else settings.preserveTime = value == "true";
        return session.Apply("TOD.Editor", [&](World& edit) { return edit.SetEnvironmentSettings(settings); });
    }
    if (key == "light.type")
    {
        return session.SetProperty(id, "light.kind", value);
    }
    if (key == "light.color")
    {
        Vec3 color;std::istringstream input(value);input.imbue(std::locale::classic());
        if(!(input>>color.x>>color.y>>color.z)||(input>>std::ws,!input.eof())||!IsFinite(color)||
            color.x<0||color.y<0||color.z<0||color.x>1||color.y>1||color.z>1)return false;
        return session.SetProperty(id,key,color);
    }
    float number{};
    const auto end = value.data() + value.size();
    const auto parsed = std::from_chars(value.data(), end, number);
    if (parsed.ec != std::errc{} || parsed.ptr != end || !std::isfinite(number)) return false;
    if (key == "camera.fov" || key == "camera.near" || key == "camera.far")
    {
        if (key == "camera.fov") number *= Pi / 180.0f;
        // 共用反射属性校验与撤销路径，World 再检查 near/far 的相互约束。
        return session.SetProperty(id, key, number);
    }
    if (key == "light.intensity" || key == "light.range")
    {
        const auto* light = world.GetLight(entity);
        if (!light) return false;
        if (key == "light.range" && (light->type == LightType::Directional || number < .05f || number > 10000.0f)) return false;
        return session.SetProperty(id, key, number);
    }
    if(key=="light.innerAngle"||key=="light.outerAngle")
    {
        const auto* light=world.GetLight(entity);
        return light&&light->type==LightType::Spot&&session.SetProperty(id,key,number*Pi/180);
    }
    if (key == "light.color.r" || key == "light.color.g" || key == "light.color.b")
    {
        const auto* light = world.GetLight(entity);
        if (!light || number < 0 || number > 1) return false;
        auto color = light->color;
        if (key.back() == 'r') color.x = number;
        else if (key.back() == 'g') color.y = number;
        else color.z = number;
        return session.SetProperty(id, "light.color", color);
    }
    const auto* transform = world.GetTransform(entity);
    if (!transform || key.size() < 3 || key[key.size()-2] != '.') return false;
    auto next = *transform;
    auto angles = EulerDegrees(next.rotation);
    const auto family = std::string_view(key).substr(0, key.size()-2);
    Vec3* vector = nullptr;
    if (family == "position") vector = &next.position;
    else if (family == "scale") vector = &next.scale;
    else if (family == "rotation") vector = &angles;
    else return false;
    switch (key.back())
    {
    case 'x': vector->x = number; break;
    case 'y': vector->y = number; break;
    case 'z': vector->z = number; break;
    default: return false;
    }
    if (family == "rotation") next.rotation = FromEulerDegrees(angles);
    return session.SetTransform(id, next);
}
}
