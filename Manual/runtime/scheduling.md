# 主循环、任务顺序与事件

这篇文档讲清楚 Flare 引擎每一帧的精确执行顺序：`Environment` 如何把变长渲染帧与固定步长物理/动画/脚本交织起来、`TickScheduler` 如何按依赖拓扑排序执行任务，以及 `EventBus` 如何用有界邮箱安全完成跨线程和跨阶段消息分发。

## 一句话定位

`flare::Environment` 是运行时主循环总控，独占 `World`、`FixedStepClock`、`TickScheduler`、`EventBus`、`SceneManager`、`SceneSpatialIndex`、`TODSystem` 与 `SkeletalAnimationSystem`；每个子系统按固定阶段边界接力，绝不互相打断迭代。

## 单帧主循环时序图（`Environment::RunFrame`）

```text
Environment::RunFrame(deltaSeconds, hooks)
  │
  ├─ 1. World::BeginFrame() + SceneManager::FrameBegin() + SynchronizeTOD()
  ├─ 2. Phase(Prepare) ──▶ Phase(PrepareFinished) ──▶ TickScheduler::BeginFrame()
  ├─ 3. Stage(FrameBegin)
  ├─ 4. EventBus::Drain(EventPhase::NextFrame)
  ├─ 5. Stage(PreTick)
  │
  ├─ 6. FixedStepClock::Advance(deltaSeconds)   [每消耗一个固定步 step 执行 0..maxSteps 次]
  │      ├─ TODSystem::Advance(step)             (推进昼夜时钟并投递日出/日落事件)
  │      ├─ SkeletalAnimationSystem::Advance()   (先于脚本固定步，保证同帧查关节有效)
  │      ├─ Stage(FixedPreTick)                  (脚本 F_FixedUpdate 前置/逻辑)
  │      ├─ Phase(BeforePhysics)
  │      ├─ physics(step)                        (物理步进)
  │      ├─ hooks.synchronize() + Spatial::Synchronize()
  │      ├─ Phase(AfterPhysics) ──▶ EventBus::Drain(EventPhase::AfterPhysics)
  │      └─ Stage(FixedPostTick) ──▶ Phase(FixedEnd)
  │
  ├─ 7. Stage(Tick)                              (脚本 F_Update 等按帧更新)
  ├─ 8. Phase(AfterLogic) ──▶ EventBus::Drain(EventPhase::AfterLogic)
  ├─ 9. hooks.synchronize() + Spatial::Synchronize()
  ├─10. Stage(PostTick) ──▶ hooks.synchronize() + Spatial::Synchronize()
  └─11. Phase(FrameEnd) ──▶ World::FinishFrame() + EventBus::EndFrame()
```

## 核心源码拆解

### 1. 固定步时钟与暂停/单步机制（`FixedStepClock` & `Environment`）

[`FixedStepClock`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/CoreLayer/FixedStepClock.h#L11) 默认步长 `1/60` 秒、单帧最大追赶步数 `maxSteps = 8`。遇到掉帧或调试断点恢复时，超出 `maxSteps` 的整步余量不会导致“死亡螺旋”，而是用 `std::fmod` 丢弃并计入 `DroppedSeconds()`。

来源：[FixedStepClock.h#L20](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/CoreLayer/FixedStepClock.h#L20)

```cpp
// Source/CoreLayer/FixedStepClock.h
std::size_t Advance(double seconds, const std::function<void(double)>& update)
{
    accumulator_ += seconds;
    std::size_t steps = 0;
    while (steps < maxSteps_ && accumulator_ + step_ * 1e-9 >= step_)
    {
        accumulator_ -= step_;
        if (accumulator_ < 0) accumulator_ = 0;
        ++steps; ++totalSteps_;
        update(step_);
    }
    if (accumulator_ + step_ * 1e-9 >= step_)
    {
        double remaining = std::fmod(accumulator_, step_);
        if (step_ - remaining <= step_ * 1e-9) remaining = 0;
        droppedSeconds_ += (accumulator_ - remaining);
        accumulator_ = remaining;
    }
    return steps;
}
```

[`Environment`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/Environment.h#L21) 区分两级暂停与单步调试：
- **`SetPaused(true)`**（模拟暂停）：跳过固定步并清空时钟余量，但仍执行帧阶段与标记 `runWhenPaused = true` 的任务（适合游戏内暂停菜单/UI）。
- **`SetFramePaused(true)`**（编辑器整帧暂停）：`RunFrame()` 直接早退，冻结全部帧回调与 `runWhenPaused` 任务。
- **`StepFrame()`**（[Environment.cpp#L96](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/Environment.cpp#L96)）：仅在任一暂停状态下可用，借助 `forceFixedStep_` 执行**恰好一个固定步**及完整帧阶段，结束后原样恢复之前的暂停标志。

## TickScheduler

[`flare::TickScheduler`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/TickScheduler.h#L31) 把绑定在组件上的回调按 **6 个执行阶段**（`FrameBegin`、`PreTick`、`FixedPreTick`、`FixedPostTick`、`Tick`、`PostTick`）和 **显式前置依赖** 有序调度。

来源：[TickScheduler.h#L20](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/TickScheduler.h#L20) · [TickScheduler.cpp#L105](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/TickScheduler.cpp#L105)

```cpp
// Source/FunctionLayer/Runtime/TickScheduler.h
struct TickDescriptor
{
    std::string key;                     // 全局唯一任务键（<= 256 字节）
    ComponentHandle component;           // 所属组件；组件失活或销毁时自动跳过/清理
    TickStage stage = TickStage::Tick;
    TickTimeDomain time = TickTimeDomain::Frame; // Frame 用帧 dt，Simulation 用本帧累计固定步 dt
    bool runWhenPaused = false;
    std::vector<TickPrerequisite> prerequisites;
    std::function<void(const TickContext&)> callback;
};
```

**关键调度语义**：
1. **依赖校验与拓扑排序**：[`SetPrerequisites()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/TickScheduler.cpp#L75) 只允许在帧外调用，拒绝依赖未来阶段（`Tick.FutureDependency`）或形成环路（`Tick.DependencyCycle`）。同阶段内按拓扑序 + `TickToken` 递增的确定性顺序执行。
2. **两种依赖强度**：
   - `TickDependencyMode::OrderOnly`：仅要求前置任务排在前面且其所属组件仍存在。
   - `TickDependencyMode::Fresh`：额外要求前置任务在**本帧（若前置在固定步阶段，还要求在本固定步 `fixedStep`）确实成功运行过**，否则跳过当前任务并记录 `"Tick.FreshDataUnavailable"` 诊断。
3. **帧内注册与异常隔离**：帧中 `Register()` 的任务被标记 `eligibleFrame = frame_ + 1`，从下一帧起生效；[`RunStage()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/TickScheduler.cpp#L105) 每次任务执行完立刻 `FlushPhaseCommands()` 与 `SweepDead()`；若任务抛异常，自动禁用该任务与对应组件并记录 `"Tick.CallbackFailed"`。

## EventBus 与 BoundedMailbox

[`flare::EventBus`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/EventBus.h#L37) 是主循环内的类型化消息总线，负责场景切换通知（ID `16..18`）、昼夜边界事件（ID `19..21`）以及玩法/子系统解耦通信：

- **显式注册载荷格式**：[`RegisterType()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/EventBus.cpp#L87) 登记 `EventTypeId`、字符串键、版本号与单条字节上限（上限 4096 种类型，单条最大 16MB）。
- **有界线程安全邮箱**：跨线程发布端通过 `EventBus::Publisher` 投递到 [`BoundedMailbox<EventMessage>`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/CoreLayer/BoundedMailbox.h#L20)（默认容量 4096 条 / 16MB 字节预算，含已取出正在处理的 `Lease`）。`Publisher` 内部仅持有 `std::weak_ptr<Shared>`，总线关闭或销毁后 `Post()` 安全返回 `EventPostResult::Closed`，绝不会延长业务对象寿命。
- **三阶段精准分发与防自激栅栏**：发布时选择 `EventPhase::NextFrame` / `AfterPhysics` / `AfterLogic`，`Post()` 会把消息的 `eligible` 置为 `当前阶段发生次数 + 1`，因此在某个 `Drain(phase)` 回调内部新发出的同阶段事件不会在同一轮无限递归触发，而是自然落到下一次对应阶段。
- **精细订阅过滤**：[`Subscribe()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/EventBus.cpp#L112) 返回 RAII [`SubscriptionToken`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/EventBus.h#L56)（析构自动退订），支持按发布通道 `Publisher`、目标实体 `target` 以及场景作用域 `scene` 过滤。

## 常用接口速查

| 类 | 核心接口 | 说明 |
| --- | --- | --- |
| `Environment` | `RunFrame` / `StepFrame` / `RunFixed` / `ConfigureClock` | 驱动自动帧循环、暂停单步、手动固定步（`automatic = false`） |
| `FixedStepClock` | `Advance` / `Alpha` / `DroppedSeconds` / `ClearRemainder` | 固定步累积、渲染插值系数 `Alpha()`、追赶丢弃统计 |
| `TickScheduler` | `Register` / `SetPrerequisites` / `Unregister` / `RunStage` | 按阶段和前置依赖调度组件回调，查询 `Diagnostics()` |
| `EventBus` | `RegisterType` / `GetPublisher` / `CreatePublisher` / `Subscribe` / `Drain` | 类型化跨线程发布与主线程阶段分发，支持 `Trace()` 追踪最近 1024 条事件 |

::: tip 引擎保留的内置事件类型 ID
- `16` (`engine.scene_activated`)、`17` (`engine.scene_unloaded`)、`18` (`engine.scene_load_failed`)：由 `SceneManager` 注册与发布。
- `19` (`engine.tod_time_changed`)、`20` (`engine.tod_sunrise`)、`21` (`engine.tod_sunset`)：由 `TODSystem` 注册与发布。

业务自定义事件请避开内置低位 ID 段。
:::

::: warning 为什么 `SkeletalAnimationSystem::Advance()` 排在 `FixedPreTick` 之前？
查看 [`Environment.cpp#L81`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/Environment.cpp#L81)：动画姿态必须先于 `FixedPreTick` 更新，这样脚本在同一个固定步内通过 `JointPosition()` 读到的就是本步最新骨骼世界坐标，可直接用于触地/攻击判定，没有一帧延迟。
:::
