#pragma once

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace se
{
struct BodyHandle
{
    std::uint32_t index = UINT32_MAX;
    std::uint32_t generation = 0;

    explicit operator bool() const noexcept { return index != UINT32_MAX && generation != 0; }
    [[nodiscard]] std::uint64_t Pack() const noexcept { return (std::uint64_t{generation} << 32) | index; }
    bool operator==(const BodyHandle&) const = default;
};

struct TriggerPairEvent
{
    BodyHandle trigger{};
    BodyHandle other{};
    std::uint64_t triggerEntity = 0;
    std::uint64_t otherEntity = 0;
    bool entered = false;
};

struct TriggerOverlap
{
    BodyHandle trigger{};
    BodyHandle other{};
    std::uint64_t triggerEntity = 0;
    std::uint64_t otherEntity = 0;
};

/// 对比前后固定步的触发器重叠集合，输出确定性排序的进入/离开事件。
class TriggerTracker
{
public:
    void Update(std::span<const TriggerOverlap> currentOverlaps);
    void Clear() noexcept;

    [[nodiscard]] const std::vector<TriggerPairEvent>& Events() const noexcept { return events_; }
    [[nodiscard]] std::size_t ActiveCount() const noexcept { return active_.size(); }

private:
    struct PairKey
    {
        std::uint64_t trigger = 0;
        std::uint64_t other = 0;
        bool operator==(const PairKey&) const = default;
    };

    struct PairHash
    {
        std::size_t operator()(const PairKey& key) const noexcept
        {
            return std::hash<std::uint64_t>{}(key.trigger) ^ (std::hash<std::uint64_t>{}(key.other) << 1);
        }
    };

    std::unordered_map<PairKey, TriggerOverlap, PairHash> active_;
    std::vector<TriggerPairEvent> events_;
};
} // namespace se
