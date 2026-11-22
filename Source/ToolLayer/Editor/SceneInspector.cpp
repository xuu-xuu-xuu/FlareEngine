#include "SceneInspector.h"
#include "FeatureInspector.h"
#include "EditorSession.h"
#include "EditorViewportCamera.h"
#include "PrefabWorkflow.h"
#include "FunctionLayer/Runtime/FeatureComponents.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Log.h"
#include "CoreLayer/Math/TransformDecomposition.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Assets/AssetStore.h"
#include "ResourceLayer/Assets/Primitives.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <sstream>
#include <unordered_set>

namespace flare
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

Quat FromEulerDegrees(Vec3 angles)
{
    const auto r = (Pi / 180.0f) * angles;
    return Normalize(AxisAngle({0, 1, 0}, r.y) * AxisAngle({1, 0, 0}, r.x) * AxisAngle({0, 0, 1}, r.z));
}

void Vector(std::vector<EditorProperty>& rows, const std::string& section, std::string_view key,
            const std::string& label, Vec3 v, bool locked, const std::string& tip = {})
{
    const float values[] = {v.x, v.y, v.z};
    for (int axis = 0; axis < 3; ++axis)
    {
        rows.push_back({std::string(key) + "." + "xyz"[axis], label, EditorPropertyType::Number,
                        Number(values[axis]), locked, {}, section});
        rows.back().axis = "XYZ"[axis];
        rows.back().dragStep = key == "rotation" ? .25f : .01f;
        rows.back().tooltip = tip;
    }
}

bool ParseSceneNumberOrRelative(std::string_view raw, float current, float& outNumber)
{
    if (raw.starts_with("@rel:") && raw.size() >= 8 && raw[6] == ':')
    {
        const char op = raw[5];
        float rhs = 0.0f;
        const auto [end, ec] = std::from_chars(raw.data() + 7, raw.data() + raw.size(), rhs);
        if (ec != std::errc{} || end != raw.data() + raw.size() || !std::isfinite(rhs)) return false;
        switch (op)
        {
        case '+': outNumber = current + rhs; break;
        case '-': outNumber = current - rhs; break;
        case '*': outNumber = current * rhs; break;
        case '/': if (std::abs(rhs) < 1e-12f) return false; outNumber = current / rhs; break;
        default: return false;
        }
        return std::isfinite(outNumber);
    }
    const auto end = raw.data() + raw.size();
    const auto parsed = std::from_chars(raw.data(), end, outNumber);
    return parsed.ec == std::errc{} && parsed.ptr == end && std::isfinite(outNumber);
}

std::string FormatVec3Compact(Vec3 v)
{
    auto clean = [](float x) {
        const float r = std::round(x * 100.0f) / 100.0f;
        return Number(std::abs(r) < 1e-4f ? 0.0f : r);
    };
    return "(" + clean(v.x) + ", " + clean(v.y) + ", " + clean(v.z) + ")";
}

std::string LayerOptionLabel(const Localization& text, std::uint32_t layer)
{
    switch (layer)
    {
    case 0: return text.GetText("EditorUi.LayerDefault");
    case 1: return text.GetText("EditorUi.LayerTransparentFX");
    case 2: return text.GetText("EditorUi.LayerIgnoreRaycast");
    case 4: return text.GetText("EditorUi.LayerWater");
    case 5: return text.GetText("EditorUi.LayerUI");
    case 6: return text.GetText("EditorUi.LayerPlayer");
    case 7: return text.GetText("EditorUi.LayerEnemy");
    case 8: return text.GetText("EditorUi.LayerProjectile");
    case 9: return text.GetText("EditorUi.LayerEnvironment");
    default: return std::to_string(layer) + ": Layer " + std::to_string(layer);
    }
}

std::string StaticFlagsDisplayLabel(const Localization& text, std::uint32_t flags)
{
    if ((flags & EntityStaticFlags::Explicit) == 0)
        return text.GetText("EditorUi.EntityStatic") + ": " + text.GetText("EditorUi.StaticAuto");
    const std::uint32_t bits = flags & EntityStaticFlags::All;
    if (bits == 0) return text.GetText("EditorUi.StaticNothing");
    if (bits == EntityStaticFlags::All) return text.GetText("EditorUi.EntityStatic");
    return text.GetText("EditorUi.EntityStatic") + " (" + text.GetText("EditorUi.StaticMixed") + ")";
}

std::uint32_t EffectiveStaticBits(const World& world, EntityHandle entity, const EntityRecord& rec)
{
    if ((rec.staticFlags & EntityStaticFlags::Explicit) != 0)
        return rec.staticFlags & EntityStaticFlags::All;
    for (auto p = entity; p;)
    {
        const auto* r = world.Get(p);
        if (!r) break;
        const auto activeOn = [&](ComponentTypeId type) {
            const auto* c = world.GetComponent(world.FindComponent(p, type));
            return c && c->Active();
        };
        if (activeOn(ComponentTypes::Script) || activeOn(CharacterSettingsComponent::TypeId) ||
            activeOn(ActorSettingsComponent::TypeId) || activeOn(NavigationAgentComponent::TypeId))
            return 0u;
        if (activeOn(PhysicsBodyComponent::TypeId) && std::get<float>(FeatureValue(world, p, "body.mass")) > 0.0f)
            return 0u;
        p = r->parent;
    }
    return EntityStaticFlags::All;
}
} // namespace

void AppendSceneInspector(const EditorSession& session, const AssetStore& assets, const Localization& text,
                          std::uint64_t id, std::vector<EditorProperty>& rows)
{
    const bool playing = session.IsPlaying();
    const auto& world = (playing && session.PlayWorld()) ? *session.PlayWorld() : session.EditWorld();
    const auto entity = world.FindByStableId(id);
    const auto add = [&](std::string key, const char* label, EditorPropertyType type, std::string value,
                         const char* section, bool folded = false) -> EditorProperty& {
        rows.push_back({std::move(key), text.GetText(label), type, std::move(value), false, {},
                        section ? text.GetText(section) : std::string{}, folded});
        return rows.back();
    };
    const auto addMenu = [&](std::string key, const char* labelKey, const char* section, bool ro) {
        auto& item = add(std::move(key), labelKey, EditorPropertyType::Action, {}, section);
        item.readOnly = ro;
        item.headerMenuAction = true;
    };

    if (const auto* record = world.Get(entity))
    {
        const std::string scrubTip = text.GetText("EditorUi.TooltipDragScrub");
        const bool isPrefab = world.GetPrefabLink(entity) != nullptr;

        // ─── Layer 1: Entity Header（仅暴露跨子系统世界级契约） ───
        add("entity.enabled", "EditorUi.EntityEnabled", EditorPropertyType::Boolean,
            record->enabled ? "true" : "false", nullptr).readOnly = false;
        add("name", "EditorUi.Name", EditorPropertyType::String, record->name, nullptr).readOnly = playing;

        {
            auto& st = add("entity.staticFlags", "EditorUi.EntityStatic", EditorPropertyType::Combo,
                           std::to_string(record->staticFlags), nullptr);
            st.readOnly = playing;
            st.multiFlag = true;
            st.deferredCommit = true;
            st.unit = "#" + std::to_string(id);
            st.displayValue = StaticFlagsDisplayLabel(text, record->staticFlags);
            const std::uint32_t effBits = EffectiveStaticBits(world, entity, *record);
            const bool isAuto = (record->staticFlags & EntityStaticFlags::Explicit) == 0;
            const bool isNone = !isAuto && (record->staticFlags & EntityStaticFlags::All) == 0;
            const bool isAll = !isAuto && (record->staticFlags & EntityStaticFlags::All) == EntityStaticFlags::All;
            st.options = {"auto", "nothing", "everything", "toggle:1", "toggle:2", "toggle:4", "toggle:8"};
            st.optionLabels = {
                std::string(isAuto ? "✓ " : "   ") + text.GetText("EditorUi.StaticAuto"),
                std::string(isNone ? "✓ " : "   ") + text.GetText("EditorUi.StaticNothing"),
                std::string(isAll ? "✓ " : "   ") + text.GetText("EditorUi.StaticEverything"),
                std::string((effBits & EntityStaticFlags::Batching) ? "✓ " : "   ") + text.GetText("EditorUi.StaticBatching"),
                std::string((effBits & EntityStaticFlags::Navigation) ? "✓ " : "   ") + text.GetText("EditorUi.StaticNavigation"),
                std::string((effBits & EntityStaticFlags::GlobalIllumination) ? "✓ " : "   ") + text.GetText("EditorUi.StaticGI"),
                std::string((effBits & EntityStaticFlags::Occluder) ? "✓ " : "   ") + text.GetText("EditorUi.StaticOccluder"),
            };
        }
        {
            auto& tagRow = add("entity.tag", "EditorUi.EntityTag", EditorPropertyType::Combo,
                               record->tag.empty() ? "Untagged" : record->tag, nullptr);
            tagRow.readOnly = false;
            constexpr std::array<std::pair<const char*, const char*>, 8> kTags{{
                {"Untagged", "EditorUi.TagUntagged"},
                {"Player", "EditorUi.TagPlayer"},
                {"MainCamera", "EditorUi.TagMainCamera"},
                {"Enemy", "EditorUi.TagEnemy"},
                {"Interactable", "EditorUi.TagInteractable"},
                {"Trigger", "EditorUi.TagTrigger"},
                {"Respawn", "EditorUi.TagRespawn"},
                {"Finish", "EditorUi.TagFinish"},
            }};
            bool known = false;
            for (const auto& [rawTag, labelKey] : kTags)
            {
                tagRow.options.push_back(rawTag);
                tagRow.optionLabels.push_back(text.GetText(labelKey));
                if (tagRow.value == rawTag)
                {
                    tagRow.displayValue = rawTag;
                    known = true;
                }
            }
            if (!known)
            {
                tagRow.options.insert(tagRow.options.begin(), tagRow.value);
                tagRow.optionLabels.insert(tagRow.optionLabels.begin(), tagRow.value);
                tagRow.displayValue = tagRow.value;
            }
        }
        {
            const std::uint32_t clampedLayer = std::min<std::uint32_t>(record->layer, 31u);
            auto& layerRow = add("entity.layer", "EditorUi.EntityLayer", EditorPropertyType::Combo,
                                 std::to_string(clampedLayer), nullptr);
            layerRow.readOnly = false;
            layerRow.displayValue = LayerOptionLabel(text, clampedLayer);
            for (std::uint32_t li = 0; li < 32u; ++li)
            {
                layerRow.options.push_back(std::to_string(li));
                layerRow.optionLabels.push_back(LayerOptionLabel(text, li));
            }
        }

        // Prefab 实例状态栏紧贴在 Entity Header 下方。
        if (const PrefabLink* prefabLink = world.GetPrefabLink(entity))
        {
            const std::filesystem::path prefabFile = assets.Resolve(prefabLink->prefabId);
            const std::string prefabLabel = prefabFile.empty() ? prefabLink->prefabId : asset_io::Utf8(prefabFile.filename());
            auto& pfAsset = add("prefab.asset", "EditorUi.PrefabAsset", EditorPropertyType::String, prefabLabel,
                                "EditorUi.PrefabInstance");
            pfAsset.readOnly = true;
            pfAsset.tooltip = prefabLink->prefabId;
            add("prefab.select", "EditorUi.PrefabSelectAsset", EditorPropertyType::Action, {}, "EditorUi.PrefabInstance").readOnly = false;
            add("prefab.revert", "EditorUi.PrefabRevert", EditorPropertyType::Action, {}, "EditorUi.PrefabInstance").readOnly = playing;
            add("prefab.apply", "EditorUi.PrefabApply", EditorPropertyType::Action, {}, "EditorUi.PrefabInstance").readOnly = playing;
        }

        // ─── Layer 2: Transform（锁定首位空间锚点 + Local/World 切换 + 连续欧拉角适配） ───
        if (const auto* t = world.GetTransform(entity))
        {
            const auto group = text.GetText("Component.Transform");
            const bool useWorld = session.TransformWorldSpace();
            auto& spaceRow = add("transform.space", "EditorUi.TransformSpace", EditorPropertyType::Combo,
                                 useWorld ? "world" : "local", "Component.Transform");
            spaceRow.options = {"local", "world"};
            spaceRow.optionLabels = {text.GetText("EditorUi.SpaceLocal"), text.GetText("EditorUi.SpaceWorld")};
            spaceRow.displayValue = useWorld ? spaceRow.optionLabels[1] : spaceRow.optionLabels[0];
            spaceRow.spatialGizmoKey = "gizmo.transform";

            Vec3 shownPos = t->position;
            Vec3 shownEuler = session.CachedEulerDegrees(id, t->rotation, false);
            Vec3 shownScale = t->scale;
            bool rotScaleLocked = false;
            if (useWorld && record->parent)
            {
                const Mat4 wm = world.GetWorldMatrix(entity);
                shownPos = {wm(0, 3), wm(1, 3), wm(2, 3)};
                Transform worldTrs{};
                if (TryDecomposeTrs(wm, worldTrs))
                {
                    shownPos = worldTrs.position;
                    shownEuler = session.CachedEulerDegrees(id, worldTrs.rotation, true);
                    shownScale = worldTrs.scale;
                }
                else
                {
                    rotScaleLocked = true;
                    EditorProperty shearNotice{"diag.transform.shear", text.GetText("EditorUi.WorldShearReadOnly"),
                                               EditorPropertyType::String, {}, true, {}, group};
                    shearNotice.diagnosticLevel = 1;
                    rows.push_back(std::move(shearNotice));
                }
            }
            else if (useWorld)
            {
                shownEuler = session.CachedEulerDegrees(id, t->rotation, true);
            }

            Vector(rows, group, "position", text.GetText("EditorUi.Position"), shownPos, false, scrubTip);
            Vector(rows, group, "rotation", text.GetText("EditorUi.Rotation"), shownEuler, rotScaleLocked, scrubTip);
            Vector(rows, group, "scale", text.GetText("EditorUi.Scale"), shownScale, rotScaleLocked, scrubTip);

            addMenu("reset.transform", "EditorUi.ResetTransform", "Component.Transform", false);
            addMenu("copy.transform", "EditorUi.ComponentCopy", "Component.Transform", false);
            if (session.ComponentClipboard().componentKey == "transform")
                addMenu("paste.transform", "EditorUi.ComponentPasteValues", "Component.Transform", false);
            if (!playing && isPrefab)
            {
                addMenu("prefab.revertComponent:transform", "EditorUi.PrefabRevertComponent", "Component.Transform", false);
                addMenu("prefab.applyComponent:transform", "EditorUi.PrefabApplyComponent", "Component.Transform", false);
            }
        }

        // ─── Layer 3 & 4: 组件栈按实体真实槽位顺序渲染 + 条件显隐 + 物理单位 + 原地校验修复 ───
        std::size_t activePrimaryCameras = 0;
        for (const EntityHandle cand : world.Entities())
        {
            if (!world.IsEnabled(cand)) continue;
            const auto cc = world.FindComponent(cand, ComponentTypes::Camera);
            const auto* cBase = world.GetComponent(cc);
            const auto* cVal = world.GetCamera(cand);
            if (cVal && (!cc || (cBase && cBase->Enabled())) && cVal->primary) ++activePrimaryCameras;
        }

        auto appendBuiltinMenu = [&](const char* compKey, const char* sectionKey) {
            const std::string k(compKey);
            addMenu("reset." + k, "EditorUi.ComponentReset", sectionKey, false);
            addMenu("moveUp." + k, "EditorUi.ComponentMoveUp", sectionKey, playing);
            addMenu("moveDown." + k, "EditorUi.ComponentMoveDown", sectionKey, playing);
            addMenu("copy." + k, "EditorUi.ComponentCopy", sectionKey, false);
            if (session.ComponentClipboard().componentKey == k)
                addMenu("paste." + k, "EditorUi.ComponentPasteValues", sectionKey, false);
            if (!playing && isPrefab)
            {
                addMenu("prefab.revertComponent:" + k, "EditorUi.PrefabRevertComponent", sectionKey, false);
                addMenu("prefab.applyComponent:" + k, "EditorUi.PrefabApplyComponent", sectionKey, false);
            }
            addMenu("remove." + k, "EditorUi.ComponentRemove", sectionKey, playing);
        };

        for (const ComponentHandle ch : world.Components(entity))
        {
            const ComponentBase* compBase = world.GetComponent(ch);
            if (!compBase || compBase->Type() == ComponentTypes::Transform || compBase->Type() == ComponentTypes::Script)
                continue;

            if (compBase->Type() == ComponentTypes::Camera)
            {
                const auto* c = world.GetCamera(entity);
                if (!c) continue;
                auto& enRow = add("camera.enabled", "Feature.Enabled", EditorPropertyType::Boolean,
                                  compBase->Enabled() ? "true" : "false", "Component.Camera");
                enRow.sectionEnabled = true;
                enRow.spatialGizmoKey = "gizmo.camera";

                if (activePrimaryCameras == 0 || (activePrimaryCameras > 1 && c->primary && compBase->Enabled()))
                {
                    EditorProperty warn{"diag.camera.primary",
                        text.GetText(activePrimaryCameras == 0 ? "EditorUi.WarnNoPrimaryCamera" : "EditorUi.WarnMultiPrimaryCamera"),
                        EditorPropertyType::String, {}, true, {}, text.GetText("Component.Camera")};
                    warn.diagnosticLevel = 2;
                    warn.fixActionKey = "fix.solePrimaryCamera";
                    warn.fixActionLabel = text.GetText("EditorUi.FixSetSolePrimary");
                    rows.push_back(std::move(warn));
                }

                auto& fovRow = add("camera.fov", "EditorUi.CameraFovDegrees", EditorPropertyType::Number,
                                   Number(c->verticalFovRadians * (180.0f / Pi)), "Component.Camera");
                fovRow.dragStep = .1f; fovRow.dragMinimum = .1f; fovRow.dragMaximum = 179.9f;
                fovRow.unit = "\xC2\xB0"; fovRow.tooltip = text.GetText("EditorUi.TooltipCameraFov");

                auto& nearRow = add("camera.near", "EditorUi.CameraNearClip", EditorPropertyType::Number,
                                    Number(c->nearPlane), "Component.Camera");
                nearRow.dragStep = .01f; nearRow.dragMinimum = .0001f; nearRow.dragMaximum = std::nextafter(c->farPlane, 0.0f);
                nearRow.unit = "m"; nearRow.tooltip = scrubTip;

                auto& farRow = add("camera.far", "EditorUi.CameraFarClip", EditorPropertyType::Number,
                                   Number(c->farPlane), "Component.Camera");
                farRow.dragStep = .1f; farRow.dragMinimum = std::nextafter(c->nearPlane, INFINITY);
                farRow.unit = "m"; farRow.tooltip = scrubTip;

                add("camera.primary", "Property.Camera.Primary", EditorPropertyType::Boolean,
                    c->primary ? "true" : "false", "Component.Camera");
                appendBuiltinMenu("camera", "Component.Camera");
            }
            else if (compBase->Type() == ComponentTypes::Light)
            {
                const auto* l = world.GetLight(entity);
                if (!l) continue;
                auto& enRow = add("light.enabled", "Feature.Enabled", EditorPropertyType::Boolean,
                                  compBase->Enabled() ? "true" : "false", "Component.Light");
                enRow.sectionEnabled = true;
                enRow.spatialGizmoKey = "gizmo.light";

                auto& type = add("light.type", "EditorUi.LightType", EditorPropertyType::Combo,
                                 std::string(LightTypeName(l->type)), "Component.Light");
                type.options = {"directional", "point", "spot"};
                type.optionLabels = {text.GetText("EditorUi.LightDirectional"), text.GetText("EditorUi.LightPoint"),
                                     text.GetText("EditorUi.LightSpot")};
                type.displayValue = type.optionLabels[static_cast<std::size_t>(l->type)];

                auto& colRow = add("light.color", "EditorUi.LightColor", EditorPropertyType::Color,
                                   Number(l->color.x) + " " + Number(l->color.y) + " " + Number(l->color.z), "Component.Light");
                colRow.colorSpace = "Linear";

                const float channels[]{l->color.x, l->color.y, l->color.z};
                for (int channel = 0; channel < 3; ++channel)
                {
                    auto& row = add(std::string("light.color.") + "rgb"[channel], "EditorUi.LightRgb", EditorPropertyType::Number,
                                    Number(channels[channel]), "Component.Light");
                    row.axis = "RGB"[channel]; row.dragStep = .01f; row.dragMinimum = 0.0f; row.dragMaximum = 1.0f;
                    row.tooltip = scrubTip;
                }
                auto& intensity = add("light.intensity", "EditorUi.LightIntensity", EditorPropertyType::Number,
                                      Number(l->intensity), "Component.Light");
                intensity.dragStep = .01f; intensity.dragMinimum = 0.0f; intensity.tooltip = scrubTip;
                intensity.unit = l->type == LightType::Directional ? "lx" : "lm";

                auto& range = add("light.range", "EditorUi.LightRange", EditorPropertyType::Number,
                                  Number(l->range), "Component.Light");
                range.dragStep = .05f; range.dragMinimum = .05f; range.dragMaximum = 10000.0f;
                range.tooltip = text.GetText("EditorUi.TooltipLightRange");
                if (l->type == LightType::Directional)
                {
                    range.readOnly = true;
                    range.displayValue = text.GetText("EditorUi.LightInfiniteRange");
                }
                else
                {
                    range.unit = "m";
                    add("light.castsShadow", "Property.Light.CastsShadow", EditorPropertyType::Boolean,
                        l->castsShadow ? "true" : "false", "Component.Light");
                }
                if (l->type == LightType::Spot)
                {
                    const auto angle = [&](const char* key, const char* label, float value, float minimum, float maximum, const std::string& tip) {
                        auto& row = add(key, label, EditorPropertyType::Number, Number(value * 180 / Pi), "Component.Light");
                        row.dragStep = .25f; row.dragMinimum = minimum * 180 / Pi; row.dragMaximum = maximum * 180 / Pi;
                        row.unit = "\xC2\xB0"; row.tooltip = tip;
                    };
                    angle("light.innerAngle", "EditorUi.LightInnerDegrees", l->innerAngle, 0, l->outerAngle, scrubTip);
                    angle("light.outerAngle", "EditorUi.LightOuterDegrees", l->outerAngle, std::max(.01f, l->innerAngle),
                          1.55f, text.GetText("EditorUi.TooltipLightOuterAngle"));
                }
                appendBuiltinMenu("light", "Component.Light");
            }
            else if (compBase->Type() == ComponentTypes::Mesh)
            {
                const auto* mesh = world.GetMesh(entity);
                if (!mesh) continue;
                auto& enRow = add("mesh.enabled", "Feature.Enabled", EditorPropertyType::Boolean,
                                  compBase->Enabled() ? "true" : "false", "Component.Mesh");
                enRow.sectionEnabled = true;

                const bool missingMesh = mesh->meshAsset.empty() ||
                    (!IsBuiltinMesh(mesh->meshAsset) && assets.Resolve(mesh->meshAsset).empty());
                if (missingMesh)
                {
                    EditorProperty warn{"diag.mesh.asset", text.GetText("EditorUi.WarnMissingMesh"),
                                        EditorPropertyType::String, {}, true, {}, text.GetText("Component.Mesh")};
                    warn.diagnosticLevel = 2;
                    warn.fixActionKey = "fix.meshUseCube";
                    warn.fixActionLabel = text.GetText("EditorUi.FixUseBuiltinCube");
                    rows.push_back(std::move(warn));
                }
                bool invalidMaterial = false;
                for (const auto& matId : mesh->materials)
                {
                    if (!matId.empty() && assets.Resolve(matId).empty()) { invalidMaterial = true; break; }
                }
                if (invalidMaterial)
                {
                    EditorProperty warn{"diag.mesh.mat", text.GetText("EditorUi.WarnMissingMaterial"),
                                        EditorPropertyType::String, {}, true, {}, text.GetText("Component.Mesh")};
                    warn.diagnosticLevel = 2;
                    warn.fixActionKey = "fix.meshClearInvalidMaterial";
                    warn.fixActionLabel = text.GetText("EditorUi.FixClearInvalidSlot");
                    rows.push_back(std::move(warn));
                }

                const auto reference = [&](const char* key, const char* label, const std::string& asset, const char* acceptedKind) -> EditorProperty& {
                    auto& field = add(key, label, EditorPropertyType::Asset, asset, "Component.Mesh");
                    field.acceptedAssetKind = acceptedKind;
                    if (asset.empty()) field.displayValue = text.GetText("EditorUi.None");
                    else if (asset == BuiltinCubeAssetId) field.displayValue = text.GetText("EditorUi.BuiltinCube");
                    else if (asset == BuiltinSphereAssetId) field.displayValue = text.GetText("EditorUi.BuiltinSphere");
                    else
                    {
                        const auto resolved = assets.Resolve(asset);
                        field.displayValue = resolved.empty() ? asset : asset_io::Utf8(resolved.filename());
                    }
                    return field;
                };
                reference("mesh.asset", "EditorUi.MeshAsset", mesh->meshAsset, "model").deferredCommit = true;
                reference("mesh.material", "EditorUi.MaterialSlot", mesh->materials.empty() ? "" : mesh->materials.front(), "material");
                for (std::size_t slot = 1; slot < mesh->materials.size(); ++slot)
                {
                    reference(("mesh.material." + std::to_string(slot)).c_str(), "EditorUi.MaterialSlot", mesh->materials[slot], "material")
                        .label += " " + std::to_string(slot + 1);
                }
                add("mesh.slot.add", "Feature.Mesh.AddSlot", EditorPropertyType::Action, {}, "Component.Mesh").readOnly = playing;
                if (mesh->materials.size() > 1)
                    add("mesh.slot.remove", "Feature.Mesh.RemoveSlot", EditorPropertyType::Action, {}, "Component.Mesh").readOnly = playing;
                add("mesh.castsShadow", "Property.Mesh.CastsShadow", EditorPropertyType::Boolean,
                    mesh->castsShadow ? "true" : "false", "Component.Mesh");
                add("mesh.receivesShadow", "Property.Mesh.ReceivesShadow", EditorPropertyType::Boolean,
                    mesh->receivesShadow ? "true" : "false", "Component.Mesh");
                appendBuiltinMenu("mesh", "Component.Mesh");
            }
            else if (IsEngineFeature(compBase->Type()))
            {
                if (const ComponentDescriptor* desc = FindComponentDescriptor(compBase->Type()))
                    AppendSingleFeatureComponent(session, text, id, *desc, ch, rows, &assets);
            }
        }

        for (auto& field : rows)
        {
            if (field.key == "mesh.asset" || field.key == "actor.asset")
            {
                field.type = EditorPropertyType::Combo;
                field.acceptedAssetKind = "model";
                field.deferredCommit = true;
                if (field.value.empty()) field.displayValue = text.GetText("EditorUi.None");
                else if (field.displayValue.empty())
                {
                    auto source = assets.Resolve(field.value);
                    field.displayValue = source.empty() ? field.value : asset_io::Utf8(source.filename());
                }
            }
            else if (field.key == "actor.clip") field.type = EditorPropertyType::Combo;
        }

        // Prefab 实例覆盖标记（包含实体头、Transform、内建组件与 Feature 组件）。
        if (const PrefabLink* prefabLink = world.GetPrefabLink(entity))
        {
            std::unordered_set<std::string> overriddenKeys;
            overriddenKeys.reserve(prefabLink->overrides.size());
            for (const PropertyOverride& item : prefabLink->overrides) overriddenKeys.insert(item.key);
            const std::string prefabSection = text.GetText("EditorUi.PrefabInstance");
            for (EditorProperty& row : rows)
            {
                if (row.section == prefabSection || row.type == EditorPropertyType::Action || row.diagnosticLevel != 0) continue;
                if (overriddenKeys.contains(CanonicalPrefabPropertyKey(row.key)))
                    row.overridden = true;
            }
        }

        // Add Component 候选列表 + Paste Component As New。
        auto addCandidate = [&](std::string key, std::string title) {
            rows.push_back({std::move(key), std::move(title), EditorPropertyType::Action, {}, playing,
                            {}, text.GetText("EditorUi.AddComponent"), true});
        };
        if (!world.GetCamera(entity)) addCandidate("add.camera", text.GetText("Component.Camera"));
        if (!world.GetMesh(entity)) addCandidate("add.mesh", text.GetText("Component.Mesh"));
        if (!world.GetLight(entity)) addCandidate("add.light", text.GetText("Component.Light"));
        AppendMissingFeatureAddActions(session, text, id, rows);
        if (!playing && session.ComponentClipboard().Valid())
        {
            const auto& cb = session.ComponentClipboard();
            if (const ComponentDescriptor* cbDesc = FindComponentDescriptor(cb.typeId);
                cbDesc && cbDesc->optional && !world.FindComponent(entity, cbDesc->type))
            {
                addCandidate("pasteNew.component",
                    text.GetText("EditorUi.ComponentPasteNew") + ": " + text.GetText(std::string(cbDesc->labelKey)));
            }
        }

        // ─── Layer 4: 只读运行与派生诊断区（严格隔离创作参数与引擎计算态，默认折叠） ───
        {
            auto addDiag = [&](std::string key, const char* labelKey, std::string val) {
                auto& r = add(std::move(key), labelKey, EditorPropertyType::String, std::move(val),
                              "EditorUi.RuntimeDiagnostics", true);
                r.readOnly = true;
            };
            addDiag("stableId", "EditorUi.StableId", "#" + std::to_string(id));
            addDiag("diag.active", "EditorUi.DiagActiveInHierarchy",
                    text.GetText(world.IsEnabled(entity) ? "EditorUi.DiagYes" : "EditorUi.DiagNo"));
            std::size_t depth = 0, childCount = 0;
            for (auto p = record->parent; p; p = world.Get(p) ? world.Get(p)->parent : EntityHandle{}) ++depth;
            for (const EntityHandle cand : world.Entities())
                if (const auto* cr = world.Get(cand); cr && cr->parent == entity) ++childCount;
            addDiag("diag.hierarchy", "EditorUi.DiagHierarchyDepth",
                    std::to_string(depth) + " / " + std::to_string(childCount));
            const bool effStatic = EffectiveStaticBits(world, entity, *record) != 0;
            addDiag("diag.static", "EditorUi.DiagStaticEffective",
                    text.GetText(effStatic ? "EditorUi.DiagYes" : "EditorUi.DiagNo"));

            const Mat4 wm = world.GetWorldMatrix(entity);
            addDiag("diag.worldPos", "EditorUi.DiagWorldPosition", FormatVec3Compact({wm(0, 3), wm(1, 3), wm(2, 3)}));
            Transform wTrs{};
            if (TryDecomposeTrs(wm, wTrs))
                addDiag("diag.worldScale", "EditorUi.DiagWorldScale", FormatVec3Compact(wTrs.scale));
            if (const auto* mesh = world.GetMesh(entity))
            {
                if (const auto bounds = EditorEntityBounds(world, const_cast<AssetStore&>(assets), entity))
                    addDiag("diag.bounds", "EditorUi.DiagWorldBounds", FormatVec3Compact(bounds->maximum - bounds->minimum) + " m");
                if (IsBuiltinMesh(mesh->meshAsset))
                {
                    addDiag("diag.topology", "EditorUi.DiagMeshTopology",
                            mesh->meshAsset == BuiltinCubeAssetId ? "1 / 24 / 12" : "1 / 561 / 1056");
                }
                else if (!mesh->meshAsset.empty())
                {
                    std::vector<AssetDiagnostic> diags;
                    if (const auto mdl = const_cast<AssetStore&>(assets).LoadModel(mesh->meshAsset, diags))
                    {
                        std::size_t vCnt = 0, tCnt = 0;
                        for (const auto& sm : mdl->meshes)
                        {
                            vCnt += sm.vertices.size();
                            tCnt += sm.indices.size() / 3;
                        }
                        addDiag("diag.topology", "EditorUi.DiagMeshTopology",
                                std::to_string(mdl->meshes.size()) + " / " + std::to_string(vCnt) + " / " + std::to_string(tCnt));
                    }
                }
            }
            if (world.FindComponent(entity, CharacterSettingsComponent::TypeId))
                addDiag("diag.physics", "EditorUi.DiagPhysicsMode", text.GetText("EditorUi.DiagCharacter"));
            else if (world.FindComponent(entity, PhysicsBodyComponent::TypeId))
            {
                const float mass = std::get<float>(FeatureValue(world, entity, "body.mass"));
                addDiag("diag.physics", "EditorUi.DiagPhysicsMode",
                        text.GetText(mass > 0.0f ? "EditorUi.DiagDynamic" : "EditorUi.DiagStatic"));
            }
            else if (world.FindComponent(entity, ColliderSettingsComponent::TypeId))
                addDiag("diag.physics", "EditorUi.DiagPhysicsMode", text.GetText("EditorUi.DiagStatic"));
        }
        return;
    }

    // 未选中任何实体（id == 0）时，Inspector 展示场景级环境配置，绝不污染实体的 GO 组件栈。
    const auto settings = world.EnvironmentSettings();
    auto& todField = add("environment.todProfile", "EditorUi.TodProfile", EditorPropertyType::Asset, settings.todProfile,
                         "EditorUi.SceneEnvironment", false);
    todField.readOnly = playing;
    todField.acceptedAssetKind = "tod";
    todField.deferredCommit = true;
    if (settings.todProfile.empty()) todField.displayValue = text.GetText("EditorUi.None");
    else
    {
        const auto resolved = assets.Resolve(settings.todProfile);
        todField.displayValue = resolved.empty() ? settings.todProfile : asset_io::Utf8(resolved.filename());
    }
    add("environment.preserveTime", "EditorUi.TodPreserveTime", EditorPropertyType::Boolean,
        settings.preserveTime ? "true" : "false", "EditorUi.SceneEnvironment", false).readOnly = playing;
}

void AppendMultiSceneInspector(const EditorSession& session, const AssetStore& assets, const Localization& text,
                               const std::vector<std::uint64_t>& entities, std::vector<EditorProperty>& rows)
{
    if (entities.empty()) return;
    if (entities.size() == 1)
    {
        AppendSceneInspector(session, assets, text, entities.front(), rows);
        return;
    }
    AppendSceneInspector(session, assets, text, entities.front(), rows);
    const std::string diagSection = text.GetText("EditorUi.RuntimeDiagnostics");
    rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const EditorProperty& p) {
        return p.key == "stableId" || p.section == diagSection || p.diagnosticLevel != 0 ||
               p.key.starts_with("prefab.") || p.key.starts_with("mesh.slot.") ||
               p.key.starts_with("moveUp.") || p.key.starts_with("moveDown.") ||
               p.key.starts_with("feature.moveUp.") || p.key.starts_with("feature.moveDown.") ||
               p.key.starts_with("copy.") || p.key.starts_with("feature.copy.");
    }), rows.end());
    for (EditorProperty& row : rows)
    {
        row.overridden = false;
        if (row.key == "entity.staticFlags") row.unit.clear();
    }

    const std::string mixedText = text.GetText("EditorUi.MixedValue");
    for (std::size_t idx = 1; idx < entities.size(); ++idx)
    {
        std::vector<EditorProperty> other;
        AppendSceneInspector(session, assets, text, entities[idx], other);
        for (auto it = rows.begin(); it != rows.end();)
        {
            const auto found = std::find_if(other.begin(), other.end(), [&](const EditorProperty& o) {
                return o.key == it->key;
            });
            if (found == other.end())
            {
                it = rows.erase(it);
                continue;
            }
            if (found->value != it->value)
            {
                it->mixedValue = true;
                if (it->type == EditorPropertyType::Combo || it->type == EditorPropertyType::Asset ||
                    it->type == EditorPropertyType::EntityRef)
                {
                    it->displayValue = mixedText;
                }
            }
            if (found->readOnly) it->readOnly = true;
            ++it;
        }
    }
}

std::string FillSceneInspectorChoices(const EditorSession& session, AssetStore& assets, const Localization& text,
                                      std::uint64_t id, ui::editor::EditorProperty& field)
{
    if (field.key == "mesh.asset" || field.key == "actor.asset")
    {
        const bool mesh = field.key == "mesh.asset";
        field.options = {""};
        field.optionLabels = {text.GetText("EditorUi.None")};
        if (mesh)
        {
            field.options.insert(field.options.end(), {BuiltinCubeAssetId, BuiltinSphereAssetId});
            field.optionLabels.insert(field.optionLabels.end(), {text.GetText("EditorUi.BuiltinCube"), text.GetText("EditorUi.BuiltinSphere")});
        }
        std::vector<std::pair<std::string, AssetId>> entries;
        for (const auto& asset : assets.ListIds())
        {
            const auto path = assets.Resolve(asset);
            auto ext = asset_io::Utf8(path.extension());
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
            if (ext == ".glb" || ext == ".gltf" || (mesh && ext == ".fbx"))
                entries.emplace_back(asset_io::Utf8(path.lexically_relative(assets.SourceRoot())), asset);
        }
        std::sort(entries.begin(), entries.end());
        for (const auto& [label, asset] : entries) { field.options.push_back(asset); field.optionLabels.push_back(label); }
    }
    else if (field.key == "actor.clip")
    {
        const auto& world = session.EditWorld();
        const auto entity = world.FindByStableId(id);
        if (!entity) return text.GetText("EditorUi.AnimationChooseModel");
        const auto asset = std::get<std::string>(FeatureValue(world, entity, "actor.asset"));
        if (asset.empty()) return text.GetText("EditorUi.AnimationChooseModel");
        std::vector<AssetDiagnostic> diagnostics;
        auto model = assets.LoadModel(asset, diagnostics);
        if (!model) return text.GetText("EditorUi.AnimationModelUnavailable");
        for (const auto& clip : model->animations)
            if (!clip.name.empty() && std::find(field.options.begin(), field.options.end(), clip.name) == field.options.end())
                field.options.push_back(clip.name);
        if (field.options.empty()) return text.GetText("EditorUi.AnimationNoClips");
    }
    return {};
}

bool ApplySceneInspector(EditorSession& session, std::uint64_t id, const std::string& key,
                         const std::string& value, AssetStore* assets)
{
    if (key == "transform.space")
    {
        if (value != "local" && value != "world") return false;
        session.SetTransformWorldSpace(value == "world");
        return true;
    }
    if (key.starts_with("copy."))
        return session.CopyComponent(id, std::string_view(key).substr(5));
    if (key.starts_with("paste."))
        return session.PasteComponentValues(id, std::string_view(key).substr(6), assets);
    if (key == "pasteNew.component")
        return session.PasteComponentAsNew(id);
    if (key.starts_with("reset."))
        return session.ResetComponent(id, std::string_view(key).substr(6), assets);
    if (key.starts_with("moveUp."))
        return session.MoveComponentOrder(id, std::string_view(key).substr(7), -1);
    if (key.starts_with("moveDown."))
        return session.MoveComponentOrder(id, std::string_view(key).substr(9), 1);

    const bool playing = session.IsPlaying();
    if (playing)
    {
        if (key == "name" || key == "entity.staticFlags" ||
            key.starts_with("add.") || key.starts_with("remove.") || key.starts_with("prefab.") ||
            key.starts_with("mesh.slot.") || key.starts_with("environment."))
        {
            return false;
        }
    }
    if (key == "prefab.revert" && assets)
    {
        std::string error;
        return RevertAllOverrides(session, *assets, id, error);
    }
    if (key.starts_with("prefab.revertProperty:") && assets)
    {
        std::string error;
        return RevertPropertyOverride(session, *assets, id, std::string_view(key).substr(22), error);
    }
    if (key.starts_with("prefab.revertComponent:") && assets)
    {
        std::string error;
        return RevertComponentOverrides(session, *assets, id, std::string_view(key).substr(23), error);
    }
    if (key.starts_with("prefab.")) return false;

    const bool hasPrefab = !playing && [&]() {
        const auto& w = session.EditWorld();
        return w.GetPrefabLink(w.FindByStableId(id)) != nullptr;
    }();
    bool startedTransaction = false;
    if (!playing && hasPrefab && key != "environment.todProfile" && key != "environment.preserveTime")
    {
        if (session.BeginEdit(key)) startedTransaction = true;
        else if (session.LastError() != "Editor.TransactionActive") return false;
    }

    auto finish = [&](bool applied, std::string overrideKeyOverride = {}) -> bool {
        if (applied && hasPrefab && key != "environment.todProfile" && key != "environment.preserveTime")
        {
            const std::string trackKey = overrideKeyOverride.empty() ? key : overrideKeyOverride;
            session.Apply("Prefab.TrackOverride", [&](World& edit) {
                if (const EntityHandle target = edit.FindByStableId(id))
                    RecordOverrideAfterPropertyEdit(edit, assets, target, trackKey);
                return true;
            });
        }
        if (startedTransaction)
        {
            if (applied) return session.CommitEdit();
            session.CancelEdit();
            return false;
        }
        return applied;
    };

    const auto& world = (playing && session.PlayWorld()) ? *session.PlayWorld() : session.EditWorld();
    const auto entity = world.FindByStableId(id);

    // 原地一键修复动作（Inline Validation Auto-Fix）。
    if (key == "fix.meshUseCube")
    {
        const auto* mesh = world.GetMesh(entity);
        if (!mesh) return finish(false);
        MeshComponent fixed = *mesh;
        fixed.meshAsset = BuiltinCubeAssetId;
        if (playing) { World* pw = session.PlayWorld(); return pw && pw->SetMesh(pw->FindByStableId(id), fixed); }
        return finish(session.SetMesh(id, fixed), "mesh.asset");
    }
    if (key == "fix.meshClearInvalidMaterial")
    {
        const auto* mesh = world.GetMesh(entity);
        if (!mesh || !assets) return finish(false);
        MeshComponent fixed = *mesh;
        for (auto& m : fixed.materials)
            if (!m.empty() && assets->Resolve(m).empty()) m.clear();
        if (playing) { World* pw = session.PlayWorld(); return pw && pw->SetMesh(pw->FindByStableId(id), fixed); }
        return finish(session.SetMesh(id, fixed), "mesh.materials");
    }
    if (key == "fix.uniformScale")
    {
        if (playing) return finish(false);
        const auto* t = world.GetTransform(entity);
        if (!t) return finish(false);
        Transform fixed = *t;
        const float avg = std::max(0.05f, (std::abs(t->scale.x) + std::abs(t->scale.y) + std::abs(t->scale.z)) / 3.0f);
        fixed.scale = {avg, avg, avg};
        return finish(session.SetTransform(id, fixed), "transform.scale");
    }
    if (key == "fix.solePrimaryCamera")
    {
        auto applySole = [&](World& w) {
            for (const EntityHandle cand : w.Entities())
            {
                const auto* cam = w.GetCamera(cand);
                if (!cam) continue;
                const bool shouldBePrimary = (w.Get(cand)->stableId == id);
                if (cam->primary != shouldBePrimary)
                {
                    CameraComponent updated = *cam;
                    updated.primary = shouldBePrimary;
                    if (!w.SetCamera(cand, updated)) return false;
                }
                if (shouldBePrimary)
                {
                    if (const auto cc = w.FindComponent(cand, ComponentTypes::Camera))
                        (void)w.SetComponentEnabled(cc, true);
                }
            }
            return true;
        };
        if (playing) { World* pw = session.PlayWorld(); return pw && applySole(*pw); }
        return finish(session.Apply("Editor.AutoFix", applySole), "camera.primary");
    }
    if (key == "fix.audioDistance")
    {
        const float minD = std::max(0.01f, std::get<float>(FeatureValue(world, entity, "audio.minDistanceMeters")));
        const float maxD = std::max(minD + 1.0f, std::get<float>(FeatureValue(world, entity, "audio.maxDistanceMeters")));
        if (playing)
        {
            World* pw = session.PlayWorld();
            std::string err;
            return pw && WriteProperty(*pw, pw->FindByStableId(id), "audio.maxDistanceMeters", maxD, err);
        }
        return finish(session.SetProperty(id, "audio.maxDistanceMeters", maxD), "audio.maxDistanceMeters");
    }
    if (key == "fix.actorClip" && assets)
    {
        const std::string actorAsset = std::get<std::string>(FeatureValue(world, entity, "actor.asset"));
        std::vector<AssetDiagnostic> diags;
        const auto mdl = assets->LoadModel(actorAsset, diags);
        if (!mdl || mdl->animations.empty()) return finish(false);
        const std::string firstClip = mdl->animations.front().name;
        if (playing)
        {
            World* pw = session.PlayWorld();
            std::string err;
            return pw && WriteProperty(*pw, pw->FindByStableId(id), "actor.clip", firstClip, err);
        }
        return finish(session.SetProperty(id, "actor.clip", firstClip), "actor.clip");
    }

    // 实体头字段：Tag / Layer / StaticFlags。
    if (key == "entity.tag")
    {
        if (playing)
        {
            World* pw = session.PlayWorld();
            return pw && pw->SetTag(pw->FindByStableId(id), value);
        }
        return finish(session.Apply("Editor.EntityMeta", [&](World& edit) {
            return edit.SetTag(edit.FindByStableId(id), value);
        }));
    }
    if (key == "entity.layer")
    {
        std::uint32_t parsedLayer = 0;
        const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsedLayer);
        if (ec != std::errc{} || ptr != value.data() + value.size() || parsedLayer > 31u) return finish(false);
        if (playing)
        {
            World* pw = session.PlayWorld();
            return pw && pw->SetLayer(pw->FindByStableId(id), parsedLayer);
        }
        return finish(session.Apply("Editor.EntityMeta", [&](World& edit) {
            return edit.SetLayer(edit.FindByStableId(id), parsedLayer);
        }));
    }
    if (key == "entity.staticFlags")
    {
        const auto* rec = world.Get(entity);
        if (!rec) return finish(false);
        std::uint32_t nextFlags = rec->staticFlags;
        if (value == "auto") nextFlags = EntityStaticFlags::None;
        else if (value == "nothing") nextFlags = EntityStaticFlags::Explicit;
        else if (value == "everything") nextFlags = EntityStaticFlags::Explicit | EntityStaticFlags::All;
        else if (value.starts_with("toggle:"))
        {
            std::uint32_t bit = 0;
            const auto sv = std::string_view(value).substr(7);
            const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), bit);
            if (ec != std::errc{} || ptr != sv.data() + sv.size() || (bit & EntityStaticFlags::All) == 0)
                return finish(false);
            const std::uint32_t baseBits = EffectiveStaticBits(world, entity, *rec);
            nextFlags = EntityStaticFlags::Explicit | (baseBits ^ bit);
        }
        else
        {
            const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), nextFlags);
            if (ec != std::errc{} || ptr != value.data() + value.size() || nextFlags > 255u) return finish(false);
        }
        if (playing)
        {
            World* pw = session.PlayWorld();
            return pw && pw->SetStaticFlags(pw->FindByStableId(id), nextFlags);
        }
        return finish(session.Apply("Editor.EntityMeta", [&](World& edit) {
            return edit.SetStaticFlags(edit.FindByStableId(id), nextFlags);
        }));
    }

    // 内建组件启用开关（Camera / Light / Mesh）。
    if (key == "camera.enabled" || key == "light.enabled" || key == "mesh.enabled")
    {
        if (value != "true" && value != "false") return finish(false);
        const bool en = (value == "true");
        const ComponentTypeId cType = key == "camera.enabled" ? ComponentTypes::Camera
                                    : (key == "light.enabled" ? ComponentTypes::Light : ComponentTypes::Mesh);
        if (playing)
        {
            World* pw = session.PlayWorld();
            const EntityHandle pe = pw ? pw->FindByStableId(id) : EntityHandle{};
            return pw && pw->SetComponentEnabled(pw->FindComponent(pe, cType), en);
        }
        return finish(session.Apply("Feature.Edit", [&](World& edit) {
            const EntityHandle ee = edit.FindByStableId(id);
            return edit.SetComponentEnabled(edit.FindComponent(ee, cType), en);
        }));
    }

    const auto setProp = [&](std::string_view propKey, const PropertyValue& propVal) -> bool {
        if (playing)
        {
            World* pw = session.PlayWorld();
            std::string err;
            return pw && WriteProperty(*pw, pw->FindByStableId(id), propKey, propVal, err);
        }
        return session.SetProperty(id, propKey, propVal);
    };
    const auto setMesh = [&](const MeshComponent& nextMesh) -> bool {
        if (playing)
        {
            World* pw = session.PlayWorld();
            return pw && pw->SetMesh(pw->FindByStableId(id), nextMesh);
        }
        return session.SetMesh(id, nextMesh);
    };
    const auto setTransform = [&](const Transform& nextTransform) -> bool {
        if (playing)
        {
            World* pw = session.PlayWorld();
            return pw && pw->SetTransform(pw->FindByStableId(id), nextTransform);
        }
        return session.SetTransform(id, nextTransform);
    };
    if (key.starts_with("remove."))
    {
        if (key == "remove.mesh" && world.FindComponent(entity, ActorSettingsComponent::TypeId))
        {
            session.ReportError("Editor.DependencyRequired");
            return finish(false);
        }
        return finish(session.Apply("EditorUi.ComponentRemove", [&](World& edit) {
            const auto target = edit.FindByStableId(id);
            std::string err;
            if (key == "remove.camera") return RemoveComponent(edit, target, "camera", err);
            if (key == "remove.light") return RemoveComponent(edit, target, "light", err);
            if (key == "remove.mesh") return RemoveComponent(edit, target, "mesh", err);
            return false;
        }));
    }
    const auto validateAssetKind = [&](const std::string& assetId, std::string_view expectedKind) -> bool {
        if (!assets || assetId.empty()) return true;
        if (expectedKind == "model" && (assetId == BuiltinCubeAssetId || assetId == BuiltinSphereAssetId)) return true;
        const auto resolved = assets->Resolve(assetId);
        return !resolved.empty() && ui::editor::IsAssetPathCompatible(asset_io::Utf8(resolved), expectedKind);
    };
    if (const auto result = ApplyFeatureInspector(session, id, key, value, assets)) return finish(*result);
    if (key == "light.castsShadow" && (!world.GetLight(entity) || world.GetLight(entity)->type == LightType::Directional)) return finish(false);
    if (key == "camera.primary" || key == "mesh.castsShadow" || key == "mesh.receivesShadow" || key == "light.castsShadow")
        return finish((value == "true" || value == "false") && setProp(key, value == "true"));
    if (key.starts_with("mesh.material.") || key.starts_with("mesh.slot."))
    {
        const auto* mesh = world.GetMesh(entity);
        if (!mesh) return finish(false);
        auto next = *mesh;
        if (key == "mesh.slot.add") { if (next.materials.size() >= 256) return finish(false); next.materials.push_back({}); }
        else if (key == "mesh.slot.remove") { if (next.materials.size() < 2) return finish(false); next.materials.pop_back(); }
        else
        {
            if (!validateAssetKind(value, "material")) return finish(false);
            std::size_t slot = 0;
            const auto raw = std::string_view(key).substr(14);
            const auto [end, error] = std::from_chars(raw.data(), raw.data() + raw.size(), slot);
            if (error != std::errc{} || end != raw.data() + raw.size() || slot >= next.materials.size()) return finish(false);
            next.materials[slot] = value;
        }
        return finish(setMesh(next));
    }
    if (key == "entity.enabled")
    {
        if (value != "true" && value != "false") return finish(false);
        const bool en = (value == "true");
        if (playing)
        {
            World* pw = session.PlayWorld();
            return pw && pw->SetEnabled(pw->FindByStableId(id), en);
        }
        return finish(session.Apply("EditorUi.EntityEnabled", [&](World& edit) {
            return edit.SetEnabled(edit.FindByStableId(id), en);
        }));
    }
    if (key == "name")
    {
        const bool saved = session.Rename(id, value);
        if (saved) Log(LogLevel::Info, "EditorUi.ToolkitProperty: name");
        return finish(saved);
    }
    if (key.starts_with("add."))
        return finish(session.Apply("EditorUi.AddComponent", [&](World& edit) {
            const auto target = edit.FindByStableId(id);
            if (key == "add.camera") return !edit.GetCamera(target) && edit.SetCamera(target, {});
            if (key == "add.light") return !edit.GetLight(target) && edit.SetLight(target, {});
            return key == "add.mesh" && !edit.GetMesh(target) && edit.SetMesh(target, {});
        }));
    if (key == "mesh.asset" || key == "mesh.material")
    {
        if (!validateAssetKind(value, key == "mesh.asset" ? "model" : "material")) return finish(false);
        const auto* mesh = world.GetMesh(entity);
        if (!mesh) return finish(false);
        auto next = *mesh;
        if (key == "mesh.asset") next.meshAsset = value;
        else if (next.materials.empty()) next.materials.push_back(value);
        else next.materials.front() = value;
        return finish(setMesh(next));
    }
    if (key == "environment.todProfile" || key == "environment.preserveTime")
    {
        if (key == "environment.todProfile" && !validateAssetKind(value, "tod")) return finish(false);
        auto settings = world.EnvironmentSettings();
        if (key == "environment.todProfile") settings.todProfile = value;
        else settings.preserveTime = value == "true";
        return session.Apply("TOD.Editor", [&](World& edit) { return edit.SetEnvironmentSettings(settings); });
    }
    if (key == "light.type")
    {
        return finish(setProp("light.kind", value));
    }
    if (key == "light.color")
    {
        Vec3 color;
        std::istringstream input(value);
        input.imbue(std::locale::classic());
        if (!(input >> color.x >> color.y >> color.z) || (input >> std::ws, !input.eof()) || !IsFinite(color) ||
            color.x < 0 || color.y < 0 || color.z < 0 || color.x > 1 || color.y > 1 || color.z > 1) return finish(false);
        return finish(setProp(key, color));
    }
    const bool isRel = value.starts_with("@rel:");
    if (key == "camera.fov" || key == "camera.near" || key == "camera.far")
    {
        const auto* cam = world.GetCamera(entity);
        if (!cam) return finish(false);
        const float current = key == "camera.fov" ? (cam->verticalFovRadians * (180.0f / Pi))
                            : (key == "camera.near" ? cam->nearPlane : cam->farPlane);
        float number{};
        if (!ParseSceneNumberOrRelative(value, current, number)) return finish(false);
        if (key == "camera.fov")
        {
            if (isRel) number = std::clamp(number, 0.1f, 179.9f);
            number *= Pi / 180.0f;
        }
        return finish(setProp(key, number));
    }
    if (key == "light.intensity" || key == "light.range")
    {
        const auto* light = world.GetLight(entity);
        if (!light) return finish(false);
        const float current = key == "light.intensity" ? light->intensity : light->range;
        float number{};
        if (!ParseSceneNumberOrRelative(value, current, number)) return finish(false);
        if (key == "light.intensity" && isRel && number < 0.0f) number = 0.0f;
        if (key == "light.range")
        {
            if (light->type == LightType::Directional) return finish(false);
            if (isRel) number = std::clamp(number, 0.05f, 10000.0f);
            if (number < .05f || number > 10000.0f) return finish(false);
        }
        return finish(setProp(key, number));
    }
    if (key == "light.innerAngle" || key == "light.outerAngle")
    {
        const auto* light = world.GetLight(entity);
        if (!light || light->type != LightType::Spot) return finish(false);
        const float current = (key == "light.innerAngle" ? light->innerAngle : light->outerAngle) * (180.f / Pi);
        float number{};
        if (!ParseSceneNumberOrRelative(value, current, number)) return finish(false);
        return finish(setProp(key, number * Pi / 180));
    }
    if (key == "light.color.r" || key == "light.color.g" || key == "light.color.b")
    {
        const auto* light = world.GetLight(entity);
        if (!light) return finish(false);
        const float current = key.back() == 'r' ? light->color.x : (key.back() == 'g' ? light->color.y : light->color.z);
        float number{};
        if (!ParseSceneNumberOrRelative(value, current, number)) return finish(false);
        if (isRel) number = std::clamp(number, 0.0f, 1.0f);
        if (number < 0 || number > 1) return finish(false);
        auto color = light->color;
        if (key.back() == 'r') color.x = number;
        else if (key.back() == 'g') color.y = number;
        else color.z = number;
        return finish(setProp("light.color", color));
    }

    const auto* transform = world.GetTransform(entity);
    const auto* record = world.Get(entity);
    if (!transform || !record || key.size() < 3 || key[key.size() - 2] != '.') return finish(false);
    const auto family = std::string_view(key).substr(0, key.size() - 2);
    if (family != "position" && family != "rotation" && family != "scale") return finish(false);

    const bool useWorld = session.TransformWorldSpace();
    if (useWorld && record->parent)
    {
        const Mat4 wm = world.GetWorldMatrix(entity);
        const auto invParent = Inverse(world.GetWorldMatrix(record->parent));
        if (!invParent) return finish(false);

        if (family == "position")
        {
            Vec3 worldPos{wm(0, 3), wm(1, 3), wm(2, 3)};
            float* axisPtr = key.back() == 'x' ? &worldPos.x : (key.back() == 'y' ? &worldPos.y : (key.back() == 'z' ? &worldPos.z : nullptr));
            if (!axisPtr) return finish(false);
            float number{};
            if (!ParseSceneNumberOrRelative(value, *axisPtr, number)) return finish(false);
            *axisPtr = number;
            Transform nextLocal = *transform;
            nextLocal.position = TransformPoint(*invParent, worldPos);
            return finish(setTransform(nextLocal));
        }

        Transform worldTrs{};
        if (!TryDecomposeTrs(wm, worldTrs)) return finish(false);
        Vec3 worldAngles = session.CachedEulerDegrees(id, worldTrs.rotation, true);
        Vec3* targetVec = (family == "rotation") ? &worldAngles : &worldTrs.scale;
        float* axisPtr = key.back() == 'x' ? &targetVec->x : (key.back() == 'y' ? &targetVec->y : (key.back() == 'z' ? &targetVec->z : nullptr));
        if (!axisPtr) return finish(false);
        float number{};
        if (!ParseSceneNumberOrRelative(value, *axisPtr, number)) return finish(false);
        *axisPtr = number;
        if (family == "rotation") worldTrs.rotation = FromEulerDegrees(worldAngles);
        Transform nextLocal{};
        if (!TryDecomposeTrs(*invParent * worldTrs.Matrix(), nextLocal))
        {
            session.ReportError("Editor.TransformNotRepresentable");
            return finish(false);
        }
        const bool ok = setTransform(nextLocal);
        if (ok)
        {
            if (family == "rotation") session.UpdateCachedEulerDegrees(id, worldTrs.rotation, worldAngles, true);
            session.UpdateCachedEulerDegrees(id, nextLocal.rotation, session.CachedEulerDegrees(id, nextLocal.rotation, false), false);
        }
        return finish(ok);
    }

    auto next = *transform;
    auto angles = session.CachedEulerDegrees(id, next.rotation, useWorld);
    Vec3* vector = (family == "position") ? &next.position : ((family == "scale") ? &next.scale : &angles);
    float* axisPtr = nullptr;
    switch (key.back())
    {
    case 'x': axisPtr = &vector->x; break;
    case 'y': axisPtr = &vector->y; break;
    case 'z': axisPtr = &vector->z; break;
    default: return finish(false);
    }
    float number{};
    if (!ParseSceneNumberOrRelative(value, *axisPtr, number)) return finish(false);
    *axisPtr = number;
    if (family == "rotation") next.rotation = FromEulerDegrees(angles);
    const bool ok = setTransform(next);
    if (ok && family == "rotation")
    {
        session.UpdateCachedEulerDegrees(id, next.rotation, angles, false);
        session.UpdateCachedEulerDegrees(id, next.rotation, angles, true);
    }
    return finish(ok);
}

bool ApplyMultiSceneInspector(EditorSession& session, const std::vector<std::uint64_t>& entities,
                              const std::string& key, const std::string& value, AssetStore* assets)
{
    if (entities.empty()) return false;
    if (entities.size() == 1) return ApplySceneInspector(session, entities.front(), key, value, assets);
    if (key == "transform.space") return ApplySceneInspector(session, entities.front(), key, value, assets);
    const bool playing = session.IsPlaying();
    bool startedTx = false;
    if (!playing)
    {
        if (session.BeginEdit(key)) startedTx = true;
        else if (session.LastError() != "Editor.TransactionActive") return false;
    }
    bool anyApplied = false;
    for (const std::uint64_t id : entities)
    {
        if (ApplySceneInspector(session, id, key, value, assets)) anyApplied = true;
    }
    if (startedTx)
    {
        if (anyApplied) return session.CommitEdit();
        session.CancelEdit();
        return false;
    }
    return anyApplied;
}
} // namespace flare
