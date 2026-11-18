#include "FunctionLayer/Physics/TriggerTracker.h"

#include <algorithm>
#include <tuple>

namespace se
{
namespace
{
bool EventLess(const TriggerPairEvent& a, const TriggerPairEvent& b) noexcept
{
    // Exits fire deterministically before Enters; ties broken by handle & entity order.
    return std::tie(a.entered, a.triggerEntity, a.otherEntity) <
               std::tie(b.entered, b.triggerEntity, b.otherEntity) ||
           (std::tie(a.entered, a.triggerEntity, a.otherEntity) ==
                std::tie(b.entered, b.triggerEntity, b.otherEntity) &&
            std::make_pair(a.trigger.Pack(), a.other.Pack()) <
                std::make_pair(b.trigger.Pack(), b.other.Pack()));
}
} // namespace

void TriggerTracker::Update(std::span<const TriggerOverlap> currentOverlaps)
{
    events_.clear();
    std::unordered_map<PairKey, TriggerOverlap, PairHash> next;
    next.reserve(currentOverlaps.size());
    for (const auto& entry : currentOverlaps)
    {
        next.emplace(PairKey{entry.trigger.Pack(), entry.other.Pack()}, entry);
    }

    for (const auto& [key, previous] : active_)
    {
        if (!next.contains(key))
        {
            events_.push_back({
                previous.trigger,
                previous.other,
                previous.triggerEntity,
                previous.otherEntity,
                false});
        }
    }

    for (const auto& [key, current] : next)
    {
        if (!active_.contains(key))
        {
            events_.push_back({
                current.trigger,
                current.other,
                current.triggerEntity,
                current.otherEntity,
                true});
        }
    }

    std::sort(events_.begin(), events_.end(), EventLess);
    active_ = std::move(next);
}

void TriggerTracker::Clear() noexcept
{
    active_.clear();
    events_.clear();
}
} // namespace se
