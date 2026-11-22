#include "ParticleSimulator.h"
#include <algorithm>
#include <cmath>

namespace flare
{
namespace
{
constexpr std::uint32_t MaximumCapacity = 65536u;
constexpr float Epsilon = 1e-6f;

bool IsFiniteVec4(Vec4 v) noexcept
{
    return IsFinite(v.x) && IsFinite(v.y) && IsFinite(v.z) && IsFinite(v.w);
}

ParticleEmitterDesc Sanitize(ParticleEmitterDesc desc) noexcept
{
    desc.maxParticles = std::clamp(desc.maxParticles, 1u, MaximumCapacity);
    if (!IsFinite(desc.emissionRatePerSecond) || desc.emissionRatePerSecond < 0.0f) desc.emissionRatePerSecond = 0.0f;
    if (!IsFinite(desc.lifetimeSeconds) || desc.lifetimeSeconds < 0.01f) desc.lifetimeSeconds = 0.01f;
    if (!IsFinite(desc.startSizeMeters) || desc.startSizeMeters < 0.0f) desc.startSizeMeters = 0.0f;
    if (!IsFinite(desc.endSizeMeters) || desc.endSizeMeters < 0.0f) desc.endSizeMeters = 0.0f;
    if (!IsFinite(desc.speedMinMetersPerSecond)) desc.speedMinMetersPerSecond = 0.0f;
    if (!IsFinite(desc.speedMaxMetersPerSecond)) desc.speedMaxMetersPerSecond = desc.speedMinMetersPerSecond;
    if (desc.speedMaxMetersPerSecond < desc.speedMinMetersPerSecond)
        desc.speedMaxMetersPerSecond = desc.speedMinMetersPerSecond;
    desc.coneAngleDegrees = IsFinite(desc.coneAngleDegrees) ? std::clamp(desc.coneAngleDegrees, 0.0f, 89.0f) : 25.0f;
    desc.radiusMeters = IsFinite(desc.radiusMeters) && desc.radiusMeters > 0.0f ? desc.radiusMeters : 0.0f;
    if (!IsFinite(desc.gravity)) desc.gravity = {};
    if (!IsFiniteVec4(desc.startColor)) desc.startColor = {1.0f, 1.0f, 1.0f, 1.0f};
    if (!IsFiniteVec4(desc.endColor)) desc.endColor = {1.0f, 1.0f, 1.0f, 0.0f};
    desc.softFactor = IsFinite(desc.softFactor) ? std::clamp(desc.softFactor, 0.5f, 8.0f) : 1.5f;
    return desc;
}

float LerpScalar(float a, float b, float t) noexcept
{
    return a + (b - a) * t;
}

Vec3 ExtractUnitDirection(const Mat4& world, Vec3 localDir) noexcept
{
    const Vec3 transformed = TransformVector(world, localDir);
    const float lenSq = LengthSquared(transformed);
    return lenSq > Epsilon ? transformed * (1.0f / std::sqrt(lenSq)) : Vec3{0.0f, 1.0f, 0.0f};
}

void SampleShape(const ParticleEmitterDesc& desc, XorShift32& rng, Vec3& localPosition, Vec3& localDirection) noexcept
{
    constexpr float TwoPi = 2.0f * Pi;
    if (desc.shape == EmitterShape::Point)
    {
        localPosition = {};
        const float z = rng.Range(-1.0f, 1.0f);
        const float phi = rng.Range(0.0f, TwoPi);
        const float sinTheta = std::sqrt((std::max)(0.0f, 1.0f - z * z));
        localDirection = {sinTheta * std::cos(phi), z, sinTheta * std::sin(phi)};
        return;
    }

    if (desc.shape == EmitterShape::Sphere)
    {
        const float z = rng.Range(-1.0f, 1.0f);
        const float phi = rng.Range(0.0f, TwoPi);
        const float sinTheta = std::sqrt((std::max)(0.0f, 1.0f - z * z));
        const Vec3 dir{sinTheta * std::cos(phi), z, sinTheta * std::sin(phi)};
        const float radius = desc.radiusMeters > 0.0f ? desc.radiusMeters * std::cbrt(rng.NextFloat01()) : 0.0f;
        localPosition = dir * radius;
        localDirection = dir;
        return;
    }

    // Cone：在 XZ 圆盘半径内生成并沿 +Y 张角发散。
    const float diskPhi = rng.Range(0.0f, TwoPi);
    const float diskRadius = desc.radiusMeters > 0.0f ? desc.radiusMeters * std::sqrt(rng.NextFloat01()) : 0.0f;
    localPosition = {diskRadius * std::cos(diskPhi), 0.0f, diskRadius * std::sin(diskPhi)};

    const float halfAngle = desc.coneAngleDegrees * (Pi / 180.0f);
    const float cosMax = std::cos(halfAngle);
    const float cosTheta = LerpScalar(cosMax, 1.0f, rng.NextFloat01());
    const float sinTheta = std::sqrt((std::max)(0.0f, 1.0f - cosTheta * cosTheta));
    const float phi = rng.Range(0.0f, TwoPi);
    localDirection = {sinTheta * std::cos(phi), cosTheta, sinTheta * std::sin(phi)};
}
} // namespace

ParticleSimulator::ParticleSimulator(const ParticleEmitterDesc& desc)
{
    desc_ = Sanitize(desc);
    rng_.Seed(desc_.seed);
    ResizeStorage(desc_.maxParticles);
}

void ParticleSimulator::ResizeStorage(std::size_t newCapacity)
{
    capacity_ = newCapacity;
    activeCount_ = (std::min)(activeCount_, capacity_);
    posX_.resize(capacity_, 0.0f);
    posY_.resize(capacity_, 0.0f);
    posZ_.resize(capacity_, 0.0f);
    velX_.resize(capacity_, 0.0f);
    velY_.resize(capacity_, 0.0f);
    velZ_.resize(capacity_, 0.0f);
    remainingLifetime_.resize(capacity_, 0.0f);
    initialLifetime_.resize(capacity_, 1.0f);
    size_.resize(capacity_, 0.0f);
    rotation_.resize(capacity_, 0.0f);
    colorR_.resize(capacity_, 1.0f);
    colorG_.resize(capacity_, 1.0f);
    colorB_.resize(capacity_, 1.0f);
    colorA_.resize(capacity_, 1.0f);
}

void ParticleSimulator::SetDescription(const ParticleEmitterDesc& desc)
{
    const auto sanitized = Sanitize(desc);
    const bool wasPlaying = desc_.playing;
    const bool capacityChanged = sanitized.maxParticles != capacity_;
    const bool seedChanged = sanitized.seed != desc_.seed;
    desc_ = sanitized;
    if (!wasPlaying && desc_.playing)
    {
        playElapsed_ = 0.0;
        emissionAccumulator_ = 0.0;
    }
    if (capacityChanged) ResizeStorage(desc_.maxParticles);
    if (seedChanged) rng_.Seed(desc_.seed);
}

void ParticleSimulator::Reset(bool resetRng) noexcept
{
    activeCount_ = 0;
    emissionAccumulator_ = 0.0;
    playElapsed_ = 0.0;
    if (resetRng) rng_.Seed(desc_.seed);
}

void ParticleSimulator::Clear() noexcept
{
    activeCount_ = 0;
    emissionAccumulator_ = 0.0;
}

void ParticleSimulator::Prewarm(const Mat4& emitterWorld) noexcept
{
    if (!desc_.prewarm || !desc_.playing) return;
    Reset(true);
    constexpr double fixedDt = 1.0 / 30.0;
    const double targetDuration = static_cast<double>(desc_.lifetimeSeconds);
    const std::size_t steps = static_cast<std::size_t>(std::clamp(std::ceil(targetDuration / fixedDt), 1.0, 300.0));
    const double stepDt = targetDuration / static_cast<double>(steps);
    for (std::size_t i = 0; i < steps; ++i)
    {
        Step(stepDt, emitterWorld);
    }
}

bool ParticleSimulator::SpawnOne(const Mat4& emitterWorld) noexcept
{
    if (activeCount_ >= capacity_) return false;

    Vec3 localPos{};
    Vec3 localDir{};
    SampleShape(desc_, rng_, localPos, localDir);

    const float speed = rng_.Range(desc_.speedMinMetersPerSecond, desc_.speedMaxMetersPerSecond);
    const Vec3 worldPos = TransformPoint(emitterWorld, localPos);
    const Vec3 worldDir = ExtractUnitDirection(emitterWorld, localDir);
    const Vec3 worldVel = worldDir * speed;

    const std::size_t slot = activeCount_++;
    posX_[slot] = worldPos.x;
    posY_[slot] = worldPos.y;
    posZ_[slot] = worldPos.z;
    velX_[slot] = worldVel.x;
    velY_[slot] = worldVel.y;
    velZ_[slot] = worldVel.z;
    initialLifetime_[slot] = desc_.lifetimeSeconds;
    remainingLifetime_[slot] = desc_.lifetimeSeconds;
    size_[slot] = desc_.startSizeMeters;
    rotation_[slot] = rng_.Range(0.0f, 2.0f * Pi);
    colorR_[slot] = desc_.startColor.x;
    colorG_[slot] = desc_.startColor.y;
    colorB_[slot] = desc_.startColor.z;
    colorA_[slot] = desc_.startColor.w;
    return true;
}

void ParticleSimulator::SwapRemove(std::size_t index) noexcept
{
    const std::size_t last = activeCount_ - 1;
    if (index != last)
    {
        posX_[index] = posX_[last];
        posY_[index] = posY_[last];
        posZ_[index] = posZ_[last];
        velX_[index] = velX_[last];
        velY_[index] = velY_[last];
        velZ_[index] = velZ_[last];
        remainingLifetime_[index] = remainingLifetime_[last];
        initialLifetime_[index] = initialLifetime_[last];
        size_[index] = size_[last];
        rotation_[index] = rotation_[last];
        colorR_[index] = colorR_[last];
        colorG_[index] = colorG_[last];
        colorB_[index] = colorB_[last];
        colorA_[index] = colorA_[last];
    }
    --activeCount_;
}

std::size_t ParticleSimulator::EmitBurst(std::size_t count, const Mat4& emitterWorld) noexcept
{
    std::size_t spawned = 0;
    for (std::size_t i = 0; i < count; ++i)
    {
        if (!SpawnOne(emitterWorld)) break;
        ++spawned;
    }
    return spawned;
}

void ParticleSimulator::Step(double deltaSeconds, const Mat4& emitterWorld) noexcept
{
    if (!IsFinite(static_cast<float>(deltaSeconds)) || deltaSeconds <= 0.0) return;

    const double clampedDt = (std::min)(deltaSeconds, 0.25);
    const float dt = static_cast<float>(clampedDt);

    for (std::size_t i = 0; i < activeCount_;)
    {
        const float remaining = remainingLifetime_[i] - dt;
        if (remaining <= 0.0f)
        {
            SwapRemove(i);
            continue;
        }
        remainingLifetime_[i] = remaining;

        const float vx = velX_[i] + desc_.gravity.x * dt;
        const float vy = velY_[i] + desc_.gravity.y * dt;
        const float vz = velZ_[i] + desc_.gravity.z * dt;
        velX_[i] = vx;
        velY_[i] = vy;
        velZ_[i] = vz;

        posX_[i] += vx * dt;
        posY_[i] += vy * dt;
        posZ_[i] += vz * dt;

        const float total = (std::max)(initialLifetime_[i], Epsilon);
        const float ageRatio = std::clamp(1.0f - (remaining / total), 0.0f, 1.0f);
        size_[i] = LerpScalar(desc_.startSizeMeters, desc_.endSizeMeters, ageRatio);
        colorR_[i] = LerpScalar(desc_.startColor.x, desc_.endColor.x, ageRatio);
        colorG_[i] = LerpScalar(desc_.startColor.y, desc_.endColor.y, ageRatio);
        colorB_[i] = LerpScalar(desc_.startColor.z, desc_.endColor.z, ageRatio);
        colorA_[i] = LerpScalar(desc_.startColor.w, desc_.endColor.w, ageRatio);
        ++i;
    }

    if (!desc_.playing || desc_.emissionRatePerSecond <= 0.0f) return;

    const double cycleDuration = static_cast<double>(desc_.lifetimeSeconds);
    if (!desc_.loop && playElapsed_ >= cycleDuration) return;

    double emitWindow = clampedDt;
    if (!desc_.loop)
    {
        const double remainingPlay = (std::max)(0.0, cycleDuration - playElapsed_);
        emitWindow = (std::min)(emitWindow, remainingPlay);
    }
    playElapsed_ += clampedDt;
    emissionAccumulator_ += emitWindow * static_cast<double>(desc_.emissionRatePerSecond);

    const std::size_t wantSpawn = static_cast<std::size_t>(std::floor(emissionAccumulator_));
    emissionAccumulator_ -= static_cast<double>(wantSpawn);
    EmitBurst(wantSpawn, emitterWorld);
}

ParticleSample ParticleSimulator::SampleAt(std::size_t index) const noexcept
{
    if (index >= activeCount_) return {};
    return {
        {posX_[index], posY_[index], posZ_[index]},
        size_[index],
        {colorR_[index], colorG_[index], colorB_[index], colorA_[index]},
        rotation_[index],
        desc_.softFactor
    };
}
} // namespace flare
