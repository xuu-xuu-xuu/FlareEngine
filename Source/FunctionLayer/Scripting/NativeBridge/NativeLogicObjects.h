#pragma once
#include "FunctionLayer/Runtime/ObjectHeap.h"
#include <atomic>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>

// C ABI 的数字令牌由会话拥有，句柄只在当前世界的堆中解析。
class NativeLogicObjects
{
    struct State final : flare::HeapObject
    {
        explicit State(double initial) : value(initial) {}
        double value;
        flare::ObjectHandle<State> next;
        void Trace(flare::ObjectTracer& tracer) const override { tracer.Strong(next); }
    };
    using Handle = flare::ObjectHandle<State>;
    std::unordered_map<std::uint64_t, Handle> handles_;
    std::unordered_map<std::uint64_t, flare::ObjectRoot<State>> roots_;
    std::uint64_t heap_ = 0;
    static std::uint64_t Token()
    {
        static std::atomic<std::uint64_t> next{1};
        auto token = next.load();
        for (;;)
        {
            if (token == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("Logic.TokenExhausted");
            if (next.compare_exchange_weak(token, token + 1)) return token;
        }
    }
public:
    // 0 create, 1 read, 2 write, 3 strong link, 4 retain, 5 release,
    // 6 collect. outToken 为对象/根令牌，outValue 为数值/收集后的存活数。
    bool Execute(flare::ObjectHeap& heap, std::uint32_t operation, std::uint64_t object,
                 std::uint64_t target, double value, std::uint64_t& outToken, double& outValue)
    {
        outToken = 0;outValue = 0;
        if (heap_ != heap.Identity())
        {
            roots_.clear();handles_.clear();heap_ = heap.Identity();
        }
        if (operation == 0)
        {
            if (!std::isfinite(value)) return false;
            const auto handle = heap.Create<State>(value);
            outToken = Token();handles_.emplace(outToken, handle);return true;
        }
        if (operation == 5) { roots_.erase(object);return true; }
        if (operation == 6)
        {
            outValue = static_cast<double>(heap.Collect().live);
            std::erase_if(handles_, [&heap](const auto& entry) { return !heap.Resolve(entry.second); });
            return true;
        }
        const auto found = handles_.find(object);
        if (found == handles_.end()) return false;
        auto* state = heap.Resolve(found->second);
        if (!state) return false;
        switch (operation)
        {
        case 1: outValue = state->value;return true;
        case 2:
            if (!std::isfinite(value)) return false;
            state->value = value;return true;
        case 3:
            if (!target) { state->next = {};return true; }
            {
                const auto next = handles_.find(target);
                if (next == handles_.end() || !heap.Resolve(next->second)) return false;
                state->next = next->second;return true;
            }
        case 4:
            outToken = Token();roots_.emplace(outToken, heap.Root(found->second));return true;
        default: return false;
        }
    }
};
