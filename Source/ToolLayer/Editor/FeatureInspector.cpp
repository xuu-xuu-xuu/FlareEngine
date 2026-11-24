#include "FeatureInspector.h"
#include "EditorSession.h"
#include "PrefabWorkflow.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Math/TransformDecomposition.h"
#include "FunctionLayer/Runtime/FeatureComponents.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Assets/AssetStore.h"
#include "ResourceLayer/Assets/Primitives.h"
#include <algorithm>
#include <charconv>
#include <cmath>

namespace flare
{
namespace
{
using namespace ui::editor;

std::string Number(float value)
{
    if (value == 0.0f) return "0";
    char buffer[48];
    auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), value);
    return error == std::errc{} ? std::string(buffer, end) : "0";
}

std::string AcceptedKindForFeatureProperty(std::string_view propKey)
{
    if (propKey == "actor.asset") return "model";
    if (propKey == "audio.clip") return "audio";
    if (propKey == "navagent.asset") return "navmesh";
    if (propKey == "environmentmap.asset") return "env";
    if (propKey == "render.diffuseProbe") return "probe";
    if (propKey == "render.skyShader") return "skyshader";
    return {};
}

std::string UnitForFeatureProperty(std::string_view propKey)
{
    if (propKey.ends_with("Meters") || propKey == "body.halfExtents" || propKey == "character.radius" ||
        propKey == "character.segmentHalfHeight" || propKey == "navagent.stoppingDistance" ||
        propKey == "rectangle.width" || propKey == "rectangle.height" || propKey == "render.aoRadius" ||
        propKey == "render.contactLength" || propKey == "render.fogDistance" || propKey == "render.fogBaseHeight" ||
        propKey == "render.rtMaxDistance" || propKey == "render.rtAoRadius" || propKey == "render.cascadeDistance" ||
        propKey == "render.shadowDistance" || propKey == "render.shadowExtent")
    {
        return "m";
    }
    if (propKey == "body.mass") return "kg";
    if (propKey == "emitter.lifetimeSeconds") return "s";
    if (propKey == "emitter.coneAngleDegrees" || propKey == "environmentmap.yaw") return "\xC2\xB0";
    if (propKey == "character.walkSpeed" || propKey == "character.jumpSpeed" || propKey == "navagent.speed" ||
        propKey == "emitter.speedMin" || propKey == "emitter.speedMax")
    {
        return "m/s";
    }
    if (propKey == "rectangle.radiance") return "cd/m\xC2\xB2";
    return {};
}

bool ApplyRelativeNumber(std::string_view raw, float current, float& outNumber)
{
    if (raw.starts_with("@rel:") && raw.size() >= 8 && raw[6] == ':')
    {
        const char op = raw[5];
        float rhs = 0.0f;
        const auto [end, ec] = std::from_chars(raw.data() + 7, raw.data() + raw.size(), rhs);
        if (ec != std::errc{} || end != raw.data() + raw.size() || !IsFinite(rhs)) return false;
        switch (op)
        {
        case '+': outNumber = current + rhs; break;
        case '-': outNumber = current - rhs; break;
        case '*': outNumber = current * rhs; break;
        case '/': if (std::abs(rhs) < 1e-12f) return false; outNumber = current / rhs; break;
        default: return false;
        }
        return IsFinite(outNumber);
    }
    const auto [end, ec] = std::from_chars(raw.data(), raw.data() + raw.size(), outNumber);
    return ec == std::errc{} && end == raw.data() + raw.size() && IsFinite(outNumber);
}

bool HasNonUniformOrZeroWorldScale(const World& world, EntityHandle entity)
{
    Transform worldTrs{};
    if (!TryDecomposeTrs(world.GetWorldMatrix(entity), worldTrs)) return true;
    const float sx = std::abs(worldTrs.scale.x);
    const float sy = std::abs(worldTrs.scale.y);
    const float sz = std::abs(worldTrs.scale.z);
    if (sx < 1e-3f || sy < 1e-3f || sz < 1e-3f) return true;
    return std::abs(sx - sy) > 1e-3f || std::abs(sy - sz) > 1e-3f;
}
} // namespace

void AppendSingleFeatureComponent(const EditorSession& session, const Localization& text, StableEntityId id,
                                  const ComponentDescriptor& descriptor, ComponentHandle handle,
                                  std::vector<EditorProperty>& rows, const AssetStore* assets)
{
    if (!IsEngineFeature(descriptor.type)) return;
    const bool playing = session.IsPlaying();
    const auto& world = (playing && session.PlayWorld()) ? *session.PlayWorld() : session.EditWorld();
    const auto entity = world.FindByStableId(id);
    const auto* component = world.GetComponent(handle);
    if (!entity || !component) return;

    const std::string colliderTip = text.GetText("EditorUi.TooltipColliderSize");
    const std::string scrubTip = text.GetText("EditorUi.TooltipDragScrub");
    const auto title = text.GetText(std::string(descriptor.labelKey));
    const std::string compKey(descriptor.key);

    EditorProperty enabledRow{"feature.enabled." + compKey, text.GetText("Feature.Enabled"),
                              EditorPropertyType::Boolean, component->Enabled() ? "true" : "false", false, {}, title};
    enabledRow.sectionEnabled = true;
    if (descriptor.type == PhysicsBodyComponent::TypeId || descriptor.type == ColliderSettingsComponent::TypeId)
        enabledRow.spatialGizmoKey = "gizmo.collider";
    else if (descriptor.type == CharacterSettingsComponent::TypeId)
        enabledRow.spatialGizmoKey = "gizmo.character";
    else if (descriptor.type == RectangleLightComponent::TypeId || descriptor.type == AudioSourceComponent::TypeId)
        enabledRow.spatialGizmoKey = "gizmo.light";
    rows.push_back(std::move(enabledRow));

    // 原地校验与一键修复（Inline Validation & Auto-Fix）。
    if (descriptor.type == CharacterSettingsComponent::TypeId && HasNonUniformOrZeroWorldScale(world, entity))
    {
        EditorProperty warn{"diag.character.scale", text.GetText("EditorUi.WarnNonUniformCollider"),
                            EditorPropertyType::String, {}, true, {}, title};
        warn.diagnosticLevel = 2;
        if (!playing)
        {
            warn.fixActionKey = "fix.uniformScale";
            warn.fixActionLabel = text.GetText("EditorUi.FixUniformScale");
        }
        rows.push_back(std::move(warn));
    }
    if (descriptor.type == ColliderSettingsComponent::TypeId)
    {
        const int shapeCode = static_cast<int>(std::lround(std::get<float>(FeatureValue(world, entity, "collider.shape"))));
        if (shapeCode == 1 && HasNonUniformOrZeroWorldScale(world, entity))
        {
            EditorProperty warn{"diag.collider.scale", text.GetText("EditorUi.WarnNonUniformCollider"),
                                EditorPropertyType::String, {}, true, {}, title};
            warn.diagnosticLevel = 2;
            if (!playing)
            {
                warn.fixActionKey = "fix.uniformScale";
                warn.fixActionLabel = text.GetText("EditorUi.FixUniformScale");
            }
            rows.push_back(std::move(warn));
        }
    }
    if (descriptor.type == AudioSourceComponent::TypeId)
    {
        const float blend = std::get<float>(FeatureValue(world, entity, "audio.spatialBlend"));
        const float minD = std::get<float>(FeatureValue(world, entity, "audio.minDistanceMeters"));
        const float maxD = std::get<float>(FeatureValue(world, entity, "audio.maxDistanceMeters"));
        if (blend > 0.0001f && minD > maxD)
        {
            EditorProperty warn{"diag.audio.dist", text.GetText("EditorUi.WarnAudioDistance"),
                                EditorPropertyType::String, {}, true, {}, title};
            warn.diagnosticLevel = 2;
            warn.fixActionKey = "fix.audioDistance";
            warn.fixActionLabel = text.GetText("EditorUi.FixAudioDistance");
            rows.push_back(std::move(warn));
        }
    }
    if (descriptor.type == ActorSettingsComponent::TypeId && assets)
    {
        const std::string actorAsset = std::get<std::string>(FeatureValue(world, entity, "actor.asset"));
        const std::string actorClip = std::get<std::string>(FeatureValue(world, entity, "actor.clip"));
        if (!actorAsset.empty() && !actorClip.empty())
        {
            if (const auto model = assets->CachedModel(actorAsset); model && !model->animations.empty())
            {
                const bool found = std::any_of(model->animations.begin(), model->animations.end(),
                                               [&](const auto& c) { return c.name == actorClip; });
                if (!found)
                {
                    EditorProperty warn{"diag.actor.clip", text.GetText("EditorUi.WarnInvalidActorClip"),
                                        EditorPropertyType::String, {}, true, {}, title};
                    warn.diagnosticLevel = 2;
                    warn.fixActionKey = "fix.actorClip";
                    warn.fixActionLabel = text.GetText("EditorUi.FixSelectFirstClip");
                    rows.push_back(std::move(warn));
                }
            }
        }
    }

    const bool rendering = descriptor.type == RenderSettingsComponent::TypeId;
    const bool instancing = rendering && std::get<bool>(FeatureValue(world, entity, "render.instancing"));
    if (rendering || descriptor.type == EnvironmentSettingsComponent::TypeId)
    {
        rows.push_back({"feature.scope." + compKey, text.GetText("Feature.Tools.Scope"), EditorPropertyType::String,
                        text.GetText(instancing ? "Feature.Render.InstancingLimits" : "Feature.Render.SceneScope"),
                        true, {}, title});
    }

    // 读取条件显隐控制状态。
    const float bodyMass = descriptor.type == PhysicsBodyComponent::TypeId
        ? std::get<float>(FeatureValue(world, entity, "body.mass")) : 1.0f;
    const int colliderShape = descriptor.type == ColliderSettingsComponent::TypeId
        ? static_cast<int>(std::lround(std::get<float>(FeatureValue(world, entity, "collider.shape")))) : 0;
    const float audioSpatial = descriptor.type == AudioSourceComponent::TypeId
        ? std::get<float>(FeatureValue(world, entity, "audio.spatialBlend")) : 1.0f;
    const bool navHasTarget = descriptor.type == NavigationAgentComponent::TypeId
        ? std::get<bool>(FeatureValue(world, entity, "navagent.hasTarget")) : true;
    const int emitterShape = descriptor.type == ParticleEmitterComponent::TypeId
        ? static_cast<int>(std::lround(std::get<float>(FeatureValue(world, entity, "emitter.shape")))) : 2;

    const auto rBool = [&](std::string_view k) -> bool {
        return rendering ? std::get<bool>(FeatureValue(world, entity, k)) : true;
    };
    const bool rPost = rBool("render.post");
    const bool rBloom = rBool("render.bloom");
    const bool rLut = rBool("render.lutEnabled");
    const bool rShadow = rBool("render.shadowEnabled");
    const bool rCascades = rBool("render.cascades");
    const bool rPcss = rBool("render.shadowPcss");
    const bool rGtao = rBool("render.gtao");
    const bool rContact = rBool("render.contactShadows");
    const bool rDiffuseSh = rBool("render.diffuseSh");
    const bool rHlod = rBool("render.hlod");
    const bool rOcclusion = rBool("render.occlusion");
    const bool rFog = rBool("render.volumetricFog");
    const bool rProcSky = rBool("render.proceduralSky");
    const bool rTod = rBool("render.todLighting");
    const bool rHasSkyShader = rendering && !std::get<std::string>(FeatureValue(world, entity, "render.skyShader")).empty();
    const bool rRt = rBool("render.rayTracing");

    auto pushSubheader = [&](const char* idSuffix, const char* labelKey) {
        rows.push_back({std::string("render.hdr.") + idSuffix, text.GetText(labelKey),
                        EditorPropertyType::Subheader, {}, true, {}, title});
    };

    for (std::size_t index = 0; index < descriptor.properties.size(); ++index)
    {
        const auto& property = descriptor.properties[index];
        const std::string_view pk = property.key;

        // 状态驱动的条件显隐（Conditional Visibility）：消除无物理意义的死参数。
        if (descriptor.type == ColliderSettingsComponent::TypeId)
        {
            if (pk == "collider.halfExtentsMeters" && colliderShape != 0) continue;
            if (pk == "collider.radiusMeters" && colliderShape != 1) continue;
        }
        else if (descriptor.type == PhysicsBodyComponent::TypeId)
        {
            if (bodyMass <= 0.0f && (pk == "body.linearDamping" || pk == "body.angularDamping")) continue;
        }
        else if (descriptor.type == AudioSourceComponent::TypeId)
        {
            if (audioSpatial <= 0.0001f && (pk == "audio.minDistanceMeters" || pk == "audio.maxDistanceMeters")) continue;
        }
        else if (descriptor.type == NavigationAgentComponent::TypeId)
        {
            if (!navHasTarget && pk == "navagent.target") continue;
        }
        else if (descriptor.type == ParticleEmitterComponent::TypeId)
        {
            if (emitterShape == 0 && (pk == "emitter.radiusMeters" || pk == "emitter.coneAngleDegrees")) continue;
            if (emitterShape == 1 && pk == "emitter.coneAngleDegrees") continue;
        }
        else if (rendering)
        {
            if (pk == "render.post") pushSubheader("post", "EditorUi.RenderGroupPost");
            else if (pk == "render.shadowEnabled") pushSubheader("shadows", "EditorUi.RenderGroupShadows");
            else if (pk == "render.lod") pushSubheader("geometry", "EditorUi.RenderGroupGeometry");
            else if (pk == "render.depthNormals") pushSubheader("lighting", "EditorUi.RenderGroupLighting");
            else if (pk == "render.volumetricFog") pushSubheader("fog", "EditorUi.RenderGroupFog");
            else if (pk == "render.rayTracing") pushSubheader("rt", "EditorUi.RenderGroupRayTracing");

            if (!rPost && (pk == "render.bloom" || pk == "render.bloomThreshold" || pk == "render.bloomStrength" ||
                           pk == "render.fxaa" || pk == "render.taa" || pk == "render.autoExposure" ||
                           pk == "render.aces" || pk == "render.lutEnabled" || pk == "render.lutLift" ||
                           pk == "render.lutGamma" || pk == "render.lutGain" ||
                           pk == "render.vignette" || pk == "render.chromaticAberration" || pk == "render.filmGrain"))
                continue;
            if (rPost && !rBloom && (pk == "render.bloomThreshold" || pk == "render.bloomStrength")) continue;
            if (rPost && !rLut && (pk == "render.lutLift" || pk == "render.lutGamma" || pk == "render.lutGain")) continue;

            if (!rShadow && (pk == "render.shadowPcf" || pk == "render.cascades" || pk == "render.cascadeCount" ||
                             pk == "render.cascadeResolution" || pk == "render.cascadeDistance" || pk == "render.cascadeSplitLambda" ||
                             pk == "render.cascadeDebug" ||
                             pk == "render.cascadeBias" || pk == "render.cascadeSlopeBias" || pk == "render.cascadeNormalBias" ||
                             pk == "render.shadowExtent" || pk == "render.shadowDistance" || pk == "render.shadowBias" ||
                             pk == "render.shadowNormalBias" || pk == "render.shadowPcss" || pk == "render.shadowAngularRadius" ||
                             pk == "render.shadowSearchDistance" || pk == "render.shadowFilterRadius" || pk == "render.shadowFilterDebug" ||
                             pk == "render.staticShadowCache"))
                continue;
            if (rShadow && !rCascades && (pk == "render.cascadeCount" || pk == "render.cascadeResolution" ||
                                          pk == "render.cascadeDistance" || pk == "render.cascadeSplitLambda" ||
                                          pk == "render.cascadeDebug" ||
                                          pk == "render.cascadeBias" || pk == "render.cascadeSlopeBias" ||
                                          pk == "render.cascadeNormalBias"))
                continue;
            if (rShadow && !rPcss && (pk == "render.shadowAngularRadius" || pk == "render.shadowSearchDistance")) continue;

            if (!rHlod && pk == "render.hlodRadius") continue;
            if (!rOcclusion && (pk == "render.occlusionCounters" || pk == "render.occlusionTiming")) continue;
            if (!rGtao && (pk == "render.aoRadius" || pk == "render.aoIntensity")) continue;
            if (!rContact && pk == "render.contactLength") continue;
            if (!rDiffuseSh && (pk == "render.localDiffuseSh" || pk == "render.diffuseProbe")) continue;
            if (!rFog && (pk == "render.fogDensity" || pk == "render.fogDistance" || pk == "render.fogAlbedo" ||
                          pk == "render.fogHeightFalloff" || pk == "render.fogBaseHeight" || pk == "render.fogSunScatter" ||
                          pk == "render.fogTurbulence" || pk == "render.fogWindSpeed"))
                continue;
            if (!rProcSky && !rHasSkyShader && (pk == "render.skyZenithColor" || pk == "render.skyHorizonColor" || pk == "render.skyGroundColor"))
                continue;
            if (!rProcSky && !rTod && !rHasSkyShader && (pk == "render.cloudCoverage" || pk == "render.cloudDensity" || pk == "render.cloudSpeed"))
                continue;
            if (!rRt && pk.starts_with("render.rt")) continue;
        }

        PropertyValue value;
        std::string error;
        if (!descriptor.read(world, handle, index, value, error)) continue;
        const auto label = text.GetText(std::string(property.labelKey));

        // 语义化枚举下拉框适配（替代晦涩的裸整数浮点输入框）。
        if (pk == "collider.shape" && std::holds_alternative<float>(value))
        {
            const int sc = std::clamp(static_cast<int>(std::lround(std::get<float>(value))), 0, 1);
            EditorProperty row{std::string(pk), label, EditorPropertyType::Combo, std::to_string(sc), !property.editable,
                               {"0", "1"}, title};
            row.optionLabels = {text.GetText("EditorUi.ColliderBox"), text.GetText("EditorUi.ColliderSphere")};
            row.displayValue = row.optionLabels[static_cast<std::size_t>(sc)];
            rows.push_back(std::move(row));
            continue;
        }
        if (pk == "emitter.shape" && std::holds_alternative<float>(value))
        {
            const int sc = std::clamp(static_cast<int>(std::lround(std::get<float>(value))), 0, 2);
            EditorProperty row{std::string(pk), label, EditorPropertyType::Combo, std::to_string(sc), !property.editable,
                               {"0", "1", "2"}, title};
            row.optionLabels = {text.GetText("EditorUi.EmitterPoint"), text.GetText("EditorUi.EmitterSphere"), text.GetText("EditorUi.EmitterCone")};
            row.displayValue = row.optionLabels[static_cast<std::size_t>(sc)];
            rows.push_back(std::move(row));
            continue;
        }
        if ((pk == "render.cascadeCount" || pk == "render.cascadeResolution" ||
             pk == "render.lookMode" || pk == "render.shadowFilterDebug") && std::holds_alternative<float>(value))
        {
            const int iv = static_cast<int>(std::lround(std::get<float>(value)));
            EditorProperty row{std::string(pk), label, EditorPropertyType::Combo, std::to_string(iv), !property.editable, {}, title};
            if (pk == "render.cascadeCount")
            {
                row.options = {"1", "2", "3", "4"};
                row.optionLabels = {"1", "2", "3", "4"};
            }
            else if (pk == "render.cascadeResolution")
            {
                row.options = {"256", "512", "1024", "1536", "2048", "4096"};
                row.optionLabels = row.options;
            }
            else if (pk == "render.lookMode")
            {
                row.options = {"0", "1", "2"};
                row.optionLabels = {"0: PBR", "1: Clay", "2: Toon"};
            }
            else
            {
                row.options = {"0", "1", "2"};
                row.optionLabels = {"0: Off", "1: Penumbra", "2: Blockers"};
            }
            for (std::size_t oi = 0; oi < row.options.size(); ++oi)
                if (row.options[oi] == row.value) row.displayValue = row.optionLabels[oi];
            rows.push_back(std::move(row));
            continue;
        }

        const auto append = [&](std::string key, std::string raw, EditorPropertyType type, char axis = 0) {
            EditorProperty row{std::move(key), label, type, std::move(raw), !property.editable, {}, title};
            if (rendering)
            {
                const bool limited = pk.starts_with("render.shadow") || pk == "render.post" || pk == "render.lod" ||
                    pk == "render.frameGraph" || pk == "render.aces" || pk.starts_with("render.bloom") ||
                    pk == "render.taa" || pk == "render.supersample4x" || pk == "render.fxaa" ||
                    pk == "render.autoExposure" || pk.starts_with("render.lut") ||
                    pk == "render.rayTracing" || pk.starts_with("render.rt");
                row.readOnly |= (instancing && (limited || pk.starts_with("render.hlod"))) ||
                                (!instancing && pk.starts_with("render.occlusion"));
            }
            row.axis = axis;
            if (type == EditorPropertyType::Number)
            {
                row.dragStep = .01f;
                row.dragMinimum = property.minimum;
                row.dragMaximum = property.maximum;
                if (axis == 0) row.unit = UnitForFeatureProperty(pk);
                if (pk == "body.halfExtents" || pk == "collider.halfExtentsMeters" || pk == "collider.radiusMeters" ||
                    pk == "character.radius" || pk == "character.segmentHalfHeight")
                {
                    row.tooltip = colliderTip;
                }
                else
                {
                    row.tooltip = scrubTip;
                }
            }
            if (type == EditorPropertyType::Asset)
            {
                row.acceptedAssetKind = AcceptedKindForFeatureProperty(pk);
                row.deferredCommit = true;
                if (row.value.empty()) row.displayValue = text.GetText("EditorUi.None");
                else if (assets)
                {
                    const auto resolved = assets->Resolve(row.value);
                    row.displayValue = resolved.empty() ? row.value : asset_io::Utf8(resolved.filename());
                }
            }
            rows.push_back(std::move(row));
        };
        if (const auto* vector = std::get_if<Vec3>(&value))
        {
            const float values[]{vector->x, vector->y, vector->z};
            for (int axis = 0; axis < 3; ++axis)
                append(std::string(pk) + '.' + "xyz"[axis], Number(values[axis]), EditorPropertyType::Number, "XYZ"[axis]);
        }
        else if (const auto* number = std::get_if<float>(&value))
            append(std::string(pk), Number(*number), EditorPropertyType::Number);
        else if (const auto* boolean = std::get_if<bool>(&value))
            append(std::string(pk), *boolean ? "true" : "false", EditorPropertyType::Boolean);
        else if (const auto* string = std::get_if<std::string>(&value))
            append(std::string(pk), *string, property.type == PropertyType::AssetId ? EditorPropertyType::Asset : EditorPropertyType::String);
    }

    auto addMenu = [&](std::string actionKey, const char* labelKey, bool ro) {
        EditorProperty item{std::move(actionKey), text.GetText(labelKey), EditorPropertyType::Action, {}, ro, {}, title};
        item.headerMenuAction = true;
        rows.push_back(std::move(item));
    };
    addMenu("feature.reset." + compKey, "EditorUi.ComponentReset", false);
    addMenu("feature.moveUp." + compKey, "EditorUi.ComponentMoveUp", playing);
    addMenu("feature.moveDown." + compKey, "EditorUi.ComponentMoveDown", playing);
    addMenu("feature.copy." + compKey, "EditorUi.ComponentCopy", false);
    if (session.ComponentClipboard().componentKey == compKey)
        addMenu("feature.paste." + compKey, "EditorUi.ComponentPasteValues", false);
    if (!playing && world.GetPrefabLink(entity))
    {
        addMenu("prefab.revertComponent:" + compKey, "EditorUi.PrefabRevertComponent", false);
        addMenu("prefab.applyComponent:" + compKey, "EditorUi.PrefabApplyComponent", false);
    }
    addMenu("feature.remove." + compKey, "Feature.Remove", playing);
}

void AppendMissingFeatureAddActions(const EditorSession& session, const Localization& text, StableEntityId id,
                                    std::vector<EditorProperty>& rows)
{
    const bool playing = session.IsPlaying();
    const auto& world = (playing && session.PlayWorld()) ? *session.PlayWorld() : session.EditWorld();
    const auto entity = world.FindByStableId(id);
    if (!entity) return;
    const std::string addSection = text.GetText("EditorUi.AddComponent");
    for (const auto& descriptor : ComponentMetadata())
    {
        if (!IsEngineFeature(descriptor.type)) continue;
        if (!world.GetComponent(world.FindComponent(entity, descriptor.type)))
        {
            rows.push_back({"add." + std::string(descriptor.key), text.GetText(std::string(descriptor.labelKey)),
                            EditorPropertyType::Action, {}, playing, {}, addSection, true});
        }
    }
}

void AppendFeatureInspector(const EditorSession& session, const Localization& text, StableEntityId id,
                            std::vector<EditorProperty>& rows, const AssetStore* assets)
{
    const bool playing = session.IsPlaying();
    const auto& world = (playing && session.PlayWorld()) ? *session.PlayWorld() : session.EditWorld();
    const auto entity = world.FindByStableId(id);
    if (!entity) return;
    for (const auto& descriptor : ComponentMetadata())
    {
        if (!IsEngineFeature(descriptor.type)) continue;
        if (const auto handle = world.FindComponent(entity, descriptor.type); world.GetComponent(handle))
            AppendSingleFeatureComponent(session, text, id, descriptor, handle, rows, assets);
    }
    AppendMissingFeatureAddActions(session, text, id, rows);
}

std::optional<bool> ApplyFeatureInspector(EditorSession& session, StableEntityId id, const std::string& key,
                                          const std::string& raw, const AssetStore* assets)
{
    const bool playing = session.IsPlaying();
    if (key.starts_with("feature.copy."))
        return session.CopyComponent(id, std::string_view(key).substr(13));
    if (key.starts_with("feature.paste."))
        return session.PasteComponentValues(id, std::string_view(key).substr(14), const_cast<AssetStore*>(assets));
    if (key.starts_with("feature.reset."))
        return session.ResetComponent(id, std::string_view(key).substr(14), const_cast<AssetStore*>(assets));
    if (key.starts_with("feature.moveUp."))
        return session.MoveComponentOrder(id, std::string_view(key).substr(15), -1);
    if (key.starts_with("feature.moveDown."))
        return session.MoveComponentOrder(id, std::string_view(key).substr(17), 1);

    if (key.starts_with("feature.enabled.") || key.starts_with("feature.remove.") || key.starts_with("add."))
    {
        const auto name = key.substr(key.starts_with("feature.enabled.") ? 16 : key.starts_with("feature.remove.") ? 15 : 4);
        const auto* descriptor = FindComponentDescriptor(name);
        if (!descriptor || !IsEngineFeature(descriptor->type)) return {};
        if (playing)
        {
            if (!key.starts_with("feature.enabled.") || (raw != "true" && raw != "false")) return false;
            World* pw = session.PlayWorld();
            if (!pw) return false;
            const auto entity = pw->FindByStableId(id);
            return pw->SetComponentEnabled(pw->FindComponent(entity, descriptor->type), raw == "true");
        }
        std::string opError;
        const bool ok = session.Apply("Feature.Edit", [&](World& world) {
            const auto entity = world.FindByStableId(id);
            if (!entity) return false;
            if (key.starts_with("add."))
            {
                // 强依赖自动补齐：骨骼角色渲染必须依赖 Mesh 组件。
                if (name == "actor" && !world.GetMesh(entity))
                {
                    MeshComponent defMesh{};
                    defMesh.meshAsset = BuiltinCubeAssetId;
                    if (!world.SetMesh(entity, defMesh)) return false;
                }
                if (!AddComponent(world, entity, name, opError)) return false;
                return true;
            }
            if (key.starts_with("feature.remove."))
                return RemoveComponent(world, entity, name, opError);
            return (raw == "true" || raw == "false") &&
                   world.SetComponentEnabled(world.FindComponent(entity, descriptor->type), raw == "true");
        });
        if (!ok && !opError.empty()) session.ReportError(std::move(opError));
        return ok;
    }

    std::string propertyKey = key;
    const auto* property = FindProperty(propertyKey);
    char axis = 0;
    if (!property && key.size() > 2 && key[key.size() - 2] == '.')
    {
        propertyKey = key.substr(0, key.size() - 2);
        property = FindProperty(propertyKey);
        axis = key.back();
    }
    const auto* descriptor = FindComponentDescriptor(propertyKey.substr(0, propertyKey.find('.')));
    if (!property || !descriptor || !IsEngineFeature(descriptor->type)) return {};
    const World& activeWorld = (playing && session.PlayWorld()) ? *session.PlayWorld() : session.EditWorld();
    const EntityHandle entityHandle = activeWorld.FindByStableId(id);
    if (!entityHandle) return false;
    PropertyValue value;
    if (property->type == PropertyType::Boolean)
    {
        if (raw != "true" && raw != "false") return false;
        value = raw == "true";
    }
    else if (property->type == PropertyType::AssetId || property->type == PropertyType::Text)
    {
        if (property->type == PropertyType::AssetId && assets && !raw.empty())
        {
            const std::string expected = AcceptedKindForFeatureProperty(propertyKey);
            if (!expected.empty())
            {
                const auto resolved = assets->Resolve(raw);
                if (resolved.empty() || !IsAssetPathCompatible(asset_io::Utf8(resolved), expected)) return false;
            }
        }
        value = raw;
    }
    else
    {
        if (property->type == PropertyType::Vector3)
        {
            auto vector = std::get<Vec3>(FeatureValue(activeWorld, entityHandle, propertyKey));
            float currentAxis = axis == 'x' ? vector.x : (axis == 'y' ? vector.y : (axis == 'z' ? vector.z : 0.f));
            if (axis != 'x' && axis != 'y' && axis != 'z') return false;
            float number = 0.f;
            if (!ApplyRelativeNumber(raw, currentAxis, number)) return false;
            if (property->minimum && number < *property->minimum && raw.starts_with("@rel:")) number = *property->minimum;
            if (property->maximum && number > *property->maximum && raw.starts_with("@rel:")) number = *property->maximum;
            if (axis == 'x') vector.x = number;
            else if (axis == 'y') vector.y = number;
            else vector.z = number;
            value = vector;
        }
        else
        {
            const float currentVal = std::get<float>(FeatureValue(activeWorld, entityHandle, propertyKey));
            float number = 0.f;
            if (!ApplyRelativeNumber(raw, currentVal, number)) return false;
            if (property->minimum && number < *property->minimum && raw.starts_with("@rel:")) number = *property->minimum;
            if (property->maximum && number > *property->maximum && raw.starts_with("@rel:")) number = *property->maximum;
            value = number;
        }
    }
    if (playing)
    {
        World* pw = session.PlayWorld();
        std::string err;
        return pw && WriteProperty(*pw, pw->FindByStableId(id), propertyKey, value, err);
    }
    return session.SetProperty(id, propertyKey, value);
}
} // namespace flare
