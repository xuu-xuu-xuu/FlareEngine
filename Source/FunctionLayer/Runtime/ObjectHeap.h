#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace se
{
class ObjectHeap;
class ObjectTracer;

struct ObjectId
{
    std::uint64_t heap = 0, generation = 0;
    std::uint32_t slot = 0;
    bool operator==(const ObjectId&) const = default;
    explicit operator bool() const noexcept { return heap != 0 && generation != 0; }
};

// 句柄本身为弱引用；Trace 中枚举的字段和 ObjectRoot 才使对象存活。
template<class T>
class ObjectHandle
{
public:
    ObjectHandle() = default;
    template<class U> requires std::is_base_of_v<T, U>
    ObjectHandle(ObjectHandle<U> other) : id_(other.Id()) {}
    ObjectId Id() const noexcept { return id_; }
    explicit operator bool() const noexcept { return static_cast<bool>(id_); }
    bool operator==(const ObjectHandle&) const = default;
private:
    friend class ObjectHeap;
    explicit ObjectHandle(ObjectId id) : id_(id) {}
    ObjectId id_{};
};

class HeapObject
{
public:
    virtual ~HeapObject() = default;
    virtual void Trace(ObjectTracer&) const {}
};

struct ObjectRootEntry { ObjectId id; };

template<class T>
class ObjectRoot
{
public:
    ObjectRoot() = default;
    ObjectHandle<T> Handle() const noexcept { return handle_; }
    void Reset() noexcept { entry_.reset(); handle_ = {}; }
private:
    friend class ObjectHeap;
    ObjectRoot(ObjectHandle<T> handle, std::shared_ptr<ObjectRootEntry> entry)
        : handle_(handle), entry_(std::move(entry)) {}
    ObjectHandle<T> handle_;
    std::shared_ptr<ObjectRootEntry> entry_;
};

struct ObjectHeapStats
{
    std::size_t live = 0, peak = 0, collected = 0, roots = 0;
    double pauseMilliseconds = 0;
};

class ObjectTracer
{
public:
    template<class T> void Strong(ObjectHandle<T> handle) { Visit(handle.Id()); }
private:
    friend class ObjectHeap;
    explicit ObjectTracer(ObjectHeap& heap) : heap_(heap) {}
    void Visit(ObjectId id);
    ObjectHeap& heap_;
};

// 会话拥有的非移动追踪堆。所有操作及对象析构都在创建堆的线程执行。
// 调用方只在脚本/世界更新结束的安全点 Collect，借用指针不跨安全点保存。
class ObjectHeap
{
public:
    ObjectHeap();
    ~ObjectHeap();
    ObjectHeap(const ObjectHeap&) = delete;
    ObjectHeap& operator=(const ObjectHeap&) = delete;
    template<class T, class... Args> requires std::is_base_of_v<HeapObject, T>
    ObjectHandle<T> Create(Args&&... args)
    {
        CheckMutable();
        return ObjectHandle<T>(Adopt(std::make_unique<T>(std::forward<Args>(args)...)));
    }
    template<class T> T* Resolve(ObjectHandle<T> handle) const
    {
        CheckThread();
        return dynamic_cast<T*>(Find(handle.Id()));
    }
    template<class T> ObjectRoot<T> Root(ObjectHandle<T> handle)
    {
        CheckMutable();
        if (!Find(handle.Id())) throw std::invalid_argument("ObjectHeap.InvalidRoot");
        auto entry = std::make_shared<ObjectRootEntry>(ObjectRootEntry{handle.Id()});
        roots_.push_back(entry);
        return ObjectRoot<T>(handle, std::move(entry));
    }
    ObjectHeapStats Collect();
    std::uint64_t Identity() const { CheckThread(); return id_; }
    const ObjectHeapStats& Stats() const { CheckThread(); return stats_; }
private:
    friend class ObjectTracer;
    struct Slot
    {
        std::unique_ptr<HeapObject> object;
        std::uint64_t generation = 1;
        bool marked = false;
    };
    void CheckThread() const;
    void CheckMutable() const;
    HeapObject* Find(ObjectId id) const noexcept;
    ObjectId Adopt(std::unique_ptr<HeapObject> object);
    std::uint64_t id_;
    std::thread::id thread_;
    bool collecting_ = false;
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_;
    std::vector<std::weak_ptr<ObjectRootEntry>> roots_;
    std::vector<std::uint32_t> work_;
    ObjectHeapStats stats_;
};
}
