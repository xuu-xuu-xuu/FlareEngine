#pragma once
#include "FunctionLayer/Particles/ParticleSimulator.h"
#include "FunctionLayer/Runtime/World.h"
#include "PlatformLayer/Graphics/Rhi.h"
#include <cstddef>
#include <map>
#include <optional>
#include <vector>

namespace se
{
/// 场景粒子协调器：管理拥有 ParticleEmitterComponent 的实体模拟器，推进仿真并生成只读渲染批。
class SceneParticles
{
public:
    void Clear() noexcept;
    void RemoveEmitter(StableEntityId entityId) noexcept;

    void Synchronize(const World& world, double deltaSeconds, bool paused);

    /// 为指定实体立即爆发粒子；仅当实体启用并挂载活跃 ParticleEmitterComponent 时生效。
    std::size_t EmitBurst(const World& world, StableEntityId entityId, std::size_t count);

    /// 控制指定实体的发射器运行时播放状态（不改写只读 World 组件，允许在回调与有挂起命令帧内调用）。
    bool SetPlaying(const World& world, StableEntityId entityId, bool playing);

    /// 清空指定实体或全部实体的活跃粒子实例。
    void ClearParticles(std::optional<StableEntityId> entityId = std::nullopt) noexcept;

    /// 将当前全部活跃粒子按混合模式分批并执行 Alpha 批从后向前排序，输出纯粹只读 GPU 实例列表。
    void BuildBatches(Vec3 cameraPosition, std::vector<rhi::ParticleBatch>& output) const;

    [[nodiscard]] std::size_t ActiveParticleCount(std::optional<StableEntityId> entityId = std::nullopt) const noexcept;
    [[nodiscard]] std::size_t TrackedEmitterCount() const noexcept { return emitters_.size(); }
    [[nodiscard]] const ParticleSimulator* FindSimulator(StableEntityId entityId) const noexcept;

private:
    struct Entry
    {
        ParticleSimulator simulator;
        bool authoredPlaying = true;
        std::optional<bool> playingOverride;
        bool prewarmed = false;
        std::uint64_t seenTick = 0;
    };

    std::map<StableEntityId, Entry>::iterator EnsureEntry(
        const World& world, EntityHandle entity, StableEntityId stableId);

    std::map<StableEntityId, Entry> emitters_;
    std::uint64_t syncTick_ = 0;
};
} // namespace se
