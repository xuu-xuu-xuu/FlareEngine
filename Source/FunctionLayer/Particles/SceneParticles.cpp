#include "SceneParticles.h"
#include "FunctionLayer/Runtime/FeatureComponents.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace flare
{
namespace
{
ParticleEmitterDesc ReadEmitterDescription(const World& world, EntityHandle entity)
{
    ParticleEmitterDesc desc;
    const auto shapeValue = static_cast<int>(std::lround(std::get<float>(FeatureValue(world, entity, "emitter.shape"))));
    desc.shape = shapeValue == 0 ? EmitterShape::Point : shapeValue == 1 ? EmitterShape::Sphere : EmitterShape::Cone;
    desc.playing = std::get<bool>(FeatureValue(world, entity, "emitter.playing"));
    desc.loop = std::get<bool>(FeatureValue(world, entity, "emitter.loop"));
    desc.prewarm = std::get<bool>(FeatureValue(world, entity, "emitter.prewarm"));
    desc.additive = std::get<bool>(FeatureValue(world, entity, "emitter.additive"));
    desc.emissionRatePerSecond = std::get<float>(FeatureValue(world, entity, "emitter.emissionRate"));
    desc.lifetimeSeconds = std::get<float>(FeatureValue(world, entity, "emitter.lifetimeSeconds"));
    desc.startSizeMeters = std::get<float>(FeatureValue(world, entity, "emitter.startSizeMeters"));
    desc.endSizeMeters = std::get<float>(FeatureValue(world, entity, "emitter.endSizeMeters"));
    desc.speedMinMetersPerSecond = std::get<float>(FeatureValue(world, entity, "emitter.speedMin"));
    desc.speedMaxMetersPerSecond = std::get<float>(FeatureValue(world, entity, "emitter.speedMax"));
    desc.coneAngleDegrees = std::get<float>(FeatureValue(world, entity, "emitter.coneAngleDegrees"));
    desc.radiusMeters = std::get<float>(FeatureValue(world, entity, "emitter.radiusMeters"));
    desc.gravity = std::get<Vec3>(FeatureValue(world, entity, "emitter.gravity"));
    const auto startRgb = std::get<Vec3>(FeatureValue(world, entity, "emitter.startColor"));
    const auto startAlpha = std::get<float>(FeatureValue(world, entity, "emitter.startAlpha"));
    const auto endRgb = std::get<Vec3>(FeatureValue(world, entity, "emitter.endColor"));
    const auto endAlpha = std::get<float>(FeatureValue(world, entity, "emitter.endAlpha"));
    desc.startColor = {startRgb.x, startRgb.y, startRgb.z, startAlpha};
    desc.endColor = {endRgb.x, endRgb.y, endRgb.z, endAlpha};
    const auto maxVal = std::max(1.0f, std::get<float>(FeatureValue(world, entity, "emitter.maxParticles")));
    desc.maxParticles = static_cast<std::uint32_t>(std::lround(maxVal));
    const auto seedVal = std::max(1.0f, std::get<float>(FeatureValue(world, entity, "emitter.seed")));
    desc.seed = static_cast<std::uint32_t>(std::lround(seedVal));
    desc.softFactor = std::get<float>(FeatureValue(world, entity, "emitter.softFactor"));
    return desc;
}

bool ActiveEmitterEntity(const World& world, EntityHandle entity) noexcept
{
    if (!world.IsEnabled(entity)) return false;
    const auto* component = world.GetComponent(world.FindComponent(entity, ParticleEmitterComponent::TypeId));
    return component && component->Active();
}
} // namespace

void SceneParticles::Clear() noexcept
{
    emitters_.clear();
}

void SceneParticles::RemoveEmitter(StableEntityId entityId) noexcept
{
    emitters_.erase(entityId);
}

std::map<StableEntityId, SceneParticles::Entry>::iterator SceneParticles::EnsureEntry(
    const World& world, EntityHandle entity, StableEntityId stableId)
{
    auto desc = ReadEmitterDescription(world, entity);
    const bool authoredPlaying = desc.playing;
    auto it = emitters_.find(stableId);
    if (it == emitters_.end())
    {
        auto [inserted, _] = emitters_.emplace(
            stableId, Entry{ParticleSimulator(desc), authoredPlaying, std::nullopt, false, syncTick_});
        it = inserted;
        if (desc.prewarm && desc.playing)
        {
            it->second.simulator.Prewarm(world.GetWorldMatrix(entity));
            it->second.prewarmed = true;
        }
    }
    else
    {
        if (it->second.authoredPlaying != authoredPlaying)
        {
            it->second.authoredPlaying = authoredPlaying;
            it->second.playingOverride.reset();
        }
        else if (it->second.playingOverride.has_value())
        {
            desc.playing = *it->second.playingOverride;
        }
        it->second.simulator.SetDescription(desc);
        if (desc.prewarm && desc.playing && !it->second.prewarmed)
        {
            it->second.simulator.Prewarm(world.GetWorldMatrix(entity));
            it->second.prewarmed = true;
        }
        else if (!desc.prewarm)
        {
            it->second.prewarmed = false;
        }
        it->second.seenTick = syncTick_;
    }
    return it;
}

void SceneParticles::Synchronize(const World& world, double deltaSeconds, bool paused)
{
    ++syncTick_;
    for (const auto entity : world.Entities())
    {
        if (!ActiveEmitterEntity(world, entity)) continue;
        const auto* record = world.Get(entity);
        if (!record) continue;

        auto it = EnsureEntry(world, entity, record->stableId);
        if (!paused && deltaSeconds > 0.0)
        {
            it->second.simulator.Step(deltaSeconds, world.GetWorldMatrix(entity));
        }
    }

    for (auto it = emitters_.begin(); it != emitters_.end();)
    {
        if (it->second.seenTick != syncTick_) it = emitters_.erase(it);
        else ++it;
    }
}

std::size_t SceneParticles::EmitBurst(const World& world, StableEntityId entityId, std::size_t count)
{
    if (count == 0) return 0;
    const auto entity = world.FindByStableId(entityId);
    if (!entity || !ActiveEmitterEntity(world, entity)) return 0;
    auto it = EnsureEntry(world, entity, entityId);
    return it->second.simulator.EmitBurst(count, world.GetWorldMatrix(entity));
}

bool SceneParticles::SetPlaying(const World& world, StableEntityId entityId, bool playing)
{
    const auto entity = world.FindByStableId(entityId);
    if (!entity || !ActiveEmitterEntity(world, entity)) return false;

    auto it = EnsureEntry(world, entity, entityId);
    it->second.playingOverride = playing;
    auto desc = it->second.simulator.Description();
    desc.playing = playing;
    it->second.simulator.SetDescription(desc);
    return true;
}

void SceneParticles::ClearParticles(std::optional<StableEntityId> entityId) noexcept
{
    if (entityId)
    {
        if (const auto it = emitters_.find(*entityId); it != emitters_.end())
            it->second.simulator.Clear();
        return;
    }
    for (auto& [_, entry] : emitters_) entry.simulator.Clear();
}

void SceneParticles::BuildBatches(Vec3 cameraPosition, std::vector<rhi::ParticleBatch>& output) const
{
    output.clear();
    if (emitters_.empty()) return;

    struct AlphaSortItem
    {
        float distanceSq = 0.0f;
        StableEntityId entityId = 0;
        std::size_t particleIndex = 0;
        rhi::ParticleInstanceGpuData instance;
    };

    std::vector<AlphaSortItem> alphaItems;
    std::vector<rhi::ParticleInstanceGpuData> additiveItems;

    for (const auto& [entityId, entry] : emitters_)
    {
        const auto& sim = entry.simulator;
        const std::size_t count = sim.ActiveCount();
        if (count == 0) continue;

        const bool additive = sim.Description().additive;
        if (additive) additiveItems.reserve(additiveItems.size() + count);
        else alphaItems.reserve(alphaItems.size() + count);

        for (std::size_t i = 0; i < count; ++i)
        {
            const auto sample = sim.SampleAt(i);
            rhi::ParticleInstanceGpuData gpu{};
            gpu.position[0] = sample.position.x;
            gpu.position[1] = sample.position.y;
            gpu.position[2] = sample.position.z;
            gpu.size = sample.size;
            gpu.color[0] = sample.color.x;
            gpu.color[1] = sample.color.y;
            gpu.color[2] = sample.color.z;
            gpu.color[3] = sample.color.w;
            gpu.params[0] = sample.rotationRadians;
            gpu.params[1] = additive ? 1.0f : 0.0f;
            gpu.params[2] = sample.softFactor;
            gpu.params[3] = 0.0f;

            if (additive)
            {
                additiveItems.push_back(gpu);
            }
            else
            {
                const Vec3 delta = sample.position - cameraPosition;
                alphaItems.push_back({LengthSquared(delta), entityId, i, gpu});
            }
        }
    }

    if (!alphaItems.empty())
    {
        std::stable_sort(alphaItems.begin(), alphaItems.end(), [](const AlphaSortItem& a, const AlphaSortItem& b) {
            if (a.distanceSq != b.distanceSq) return a.distanceSq > b.distanceSq;
            if (a.entityId != b.entityId) return a.entityId < b.entityId;
            return a.particleIndex < b.particleIndex;
        });
        rhi::ParticleBatch batch;
        batch.additive = false;
        batch.instances.reserve(alphaItems.size());
        for (const auto& item : alphaItems) batch.instances.push_back(item.instance);
        output.push_back(std::move(batch));
    }

    if (!additiveItems.empty())
    {
        rhi::ParticleBatch batch;
        batch.additive = true;
        batch.instances = std::move(additiveItems);
        output.push_back(std::move(batch));
    }
}

std::size_t SceneParticles::ActiveParticleCount(std::optional<StableEntityId> entityId) const noexcept
{
    if (entityId)
    {
        const auto it = emitters_.find(*entityId);
        return it != emitters_.end() ? it->second.simulator.ActiveCount() : 0u;
    }
    std::size_t total = 0;
    for (const auto& [_, entry] : emitters_) total += entry.simulator.ActiveCount();
    return total;
}

const ParticleSimulator* SceneParticles::FindSimulator(StableEntityId entityId) const noexcept
{
    const auto it = emitters_.find(entityId);
    return it != emitters_.end() ? &it->second.simulator : nullptr;
}
} // namespace flare
