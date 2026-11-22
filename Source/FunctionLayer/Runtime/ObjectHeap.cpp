#include "ObjectHeap.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <stdexcept>

namespace flare
{
namespace
{
std::uint64_t NextHeapId()
{
    static std::atomic<std::uint64_t> next{1};
    auto value = next.load();
    for (;;)
    {
        if (value == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("ObjectHeap.IdentityExhausted");
        if (next.compare_exchange_weak(value, value + 1)) return value;
    }
}
}
ObjectHeap::ObjectHeap() : id_(NextHeapId()), thread_(std::this_thread::get_id()) {}
ObjectHeap::~ObjectHeap()
{
    // 在任何析构回调之前失效整堆，避免回调读取同时销毁的对象。
    id_ = 0;
    collecting_ = true;
}
void ObjectHeap::CheckThread() const
{
    if (thread_ != std::this_thread::get_id()) throw std::logic_error("ObjectHeap.WrongThread");
}
void ObjectHeap::CheckMutable() const
{
    CheckThread();
    if (collecting_) throw std::logic_error("ObjectHeap.Collecting");
}
HeapObject* ObjectHeap::Find(ObjectId id) const noexcept
{
    if (id.heap != id_ || !id || id.slot >= slots_.size()) return nullptr;
    const auto& slot = slots_[id.slot];
    return slot.generation == id.generation ? slot.object.get() : nullptr;
}
ObjectId ObjectHeap::Adopt(std::unique_ptr<HeapObject> object)
{
    std::uint32_t index;
    if (free_.empty())
    {
        if (slots_.size() >= std::numeric_limits<std::uint32_t>::max())
            throw std::overflow_error("ObjectHeap.SlotsExhausted");
        index = static_cast<std::uint32_t>(slots_.size());
        slots_.emplace_back();
    }
    else { index = free_.back(); free_.pop_back(); }
    auto& slot = slots_[index];
    slot.object = std::move(object);
    ++stats_.live;
    stats_.peak = std::max(stats_.peak, stats_.live);
    return {id_, slot.generation, index};
}
void ObjectTracer::Visit(ObjectId id)
{
    if (!heap_.Find(id)) return;
    auto& slot = heap_.slots_[id.slot];
    if (slot.marked) return;
    heap_.work_.push_back(id.slot);
    slot.marked = true;
}
ObjectHeapStats ObjectHeap::Collect()
{
    CheckMutable();
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::unique_ptr<HeapObject>> garbage;
    garbage.reserve(stats_.live);
    free_.reserve(slots_.size());
    work_.clear();
    work_.reserve(stats_.live);
    for (auto& slot : slots_) slot.marked = false;
    collecting_ = true;
    struct Reset { bool& flag; ~Reset() { flag = false; } } reset{collecting_};
    ObjectTracer tracer(*this);
    std::erase_if(roots_, [](const auto& root) { return root.expired(); });
    stats_.roots = roots_.size();
    for (const auto& weak : roots_) if (const auto root = weak.lock()) tracer.Visit(root->id);
    while (!work_.empty())
    {
        const auto index = work_.back(); work_.pop_back();
        slots_[index].object->Trace(tracer);
    }
    for (std::uint32_t index = 0; index < slots_.size(); ++index)
    {
        auto& slot = slots_[index];
        if (!slot.object || slot.marked) continue;
        garbage.push_back(std::move(slot.object));
        if (slot.generation < std::numeric_limits<std::uint64_t>::max())
        {
            ++slot.generation;
            free_.push_back(index);
        }
    }
    stats_.collected = garbage.size();
    stats_.live -= garbage.size();
    // 所有回收对象均已失效，随后才触发用户析构。
    garbage.clear();
    stats_.pauseMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    return stats_;
}
}
