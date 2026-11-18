#pragma once
#include "CoreLayer/Math/Math.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace se
{
/// 确定性 32 位 XorShift 伪随机数生成器（禁止使用非确定性随机源）。
class XorShift32
{
public:
    explicit XorShift32(std::uint32_t seed = 1u) noexcept { Seed(seed); }

    void Seed(std::uint32_t seed) noexcept { state_ = seed != 0u ? seed : 0x6D2B79F5u; }
    [[nodiscard]] std::uint32_t State() const noexcept { return state_; }

    std::uint32_t NextU32() noexcept
    {
        std::uint32_t x = state_;
        x ^= x << 13u;
        x ^= x >> 17u;
        x ^= x << 5u;
        state_ = x;
        return x;
    }

    float NextFloat01() noexcept
    {
        return static_cast<float>(NextU32() >> 8u) * (1.0f / 16777216.0f);
    }

    float Range(float minimum, float maximum) noexcept
    {
        return minimum + (maximum - minimum) * NextFloat01();
    }

private:
    std::uint32_t state_ = 0x6D2B79F5u;
};

enum class EmitterShape : std::uint8_t
{
    Point = 0,
    Sphere = 1,
    Cone = 2
};

struct ParticleEmitterDesc
{
    EmitterShape shape = EmitterShape::Cone;
    std::uint32_t maxParticles = 256u;
    std::uint32_t seed = 1u;
    float emissionRatePerSecond = 20.0f;
    float lifetimeSeconds = 1.5f;
    float startSizeMeters = 0.25f;
    float endSizeMeters = 0.05f;
    float speedMinMetersPerSecond = 1.5f;
    float speedMaxMetersPerSecond = 3.0f;
    float coneAngleDegrees = 25.0f;
    float radiusMeters = 0.5f;
    Vec3 gravity{0.0f, -2.0f, 0.0f};
    Vec4 startColor{1.0f, 0.85f, 0.4f, 1.0f};
    Vec4 endColor{1.0f, 0.35f, 0.1f, 0.0f};
    float softFactor = 1.5f;
    bool loop = true;
    bool playing = true;
    bool additive = false;
    bool prewarm = false;

    bool operator==(const ParticleEmitterDesc&) const = default;
};

struct ParticleSample
{
    Vec3 position{};
    float size = 0.0f;
    Vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
    float rotationRadians = 0.0f;
    float softFactor = 1.5f;
};

/// 纯 CPU SoA 粒子模拟内核：预分配固定容量缓冲区，步进与 O(1) 消亡回收期零堆内存分配。
class ParticleSimulator
{
public:
    explicit ParticleSimulator(const ParticleEmitterDesc& desc = {});

    void SetDescription(const ParticleEmitterDesc& desc);
    [[nodiscard]] const ParticleEmitterDesc& Description() const noexcept { return desc_; }

    void Reset(bool resetRng = true) noexcept;
    void Clear() noexcept;
    void Prewarm(const Mat4& emitterWorld = Mat4::Identity()) noexcept;

    /// 推进一个仿真步长（dt <= 0 仅同步属性，不生成或老化粒子）；发射与重力步进零堆分配。
    void Step(double deltaSeconds, const Mat4& emitterWorld = Mat4::Identity()) noexcept;

    /// 在发射器当前世界变换处立即爆发 count 个粒子，返回实际生成数量。
    std::size_t EmitBurst(std::size_t count, const Mat4& emitterWorld = Mat4::Identity()) noexcept;

    [[nodiscard]] std::size_t ActiveCount() const noexcept { return activeCount_; }
    [[nodiscard]] std::size_t Capacity() const noexcept { return capacity_; }
    [[nodiscard]] ParticleSample SampleAt(std::size_t index) const noexcept;

    [[nodiscard]] const float* PositionsX() const noexcept { return posX_.data(); }
    [[nodiscard]] const float* PositionsY() const noexcept { return posY_.data(); }
    [[nodiscard]] const float* PositionsZ() const noexcept { return posZ_.data(); }
    [[nodiscard]] const float* RemainingLifetimes() const noexcept { return remainingLifetime_.data(); }
    [[nodiscard]] const float* InitialLifetimes() const noexcept { return initialLifetime_.data(); }
    [[nodiscard]] const float* Sizes() const noexcept { return size_.data(); }

private:
    void ResizeStorage(std::size_t newCapacity);
    bool SpawnOne(const Mat4& emitterWorld) noexcept;
    void SwapRemove(std::size_t index) noexcept;

    ParticleEmitterDesc desc_{};
    XorShift32 rng_{1u};
    std::size_t capacity_ = 0;
    std::size_t activeCount_ = 0;
    double emissionAccumulator_ = 0.0;
    double playElapsed_ = 0.0;

    std::vector<float> posX_, posY_, posZ_;
    std::vector<float> velX_, velY_, velZ_;
    std::vector<float> remainingLifetime_, initialLifetime_;
    std::vector<float> size_, rotation_;
    std::vector<float> colorR_, colorG_, colorB_, colorA_;
};
} // namespace se
