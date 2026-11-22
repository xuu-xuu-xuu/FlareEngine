# 昼夜、天空与场景环境

这篇文档讲清楚 Flare 的昼夜时间系统（Time of Day, TOD）：环境关键帧资产 `TODProfile` 的格式与循环插值、`TODSystem` 的固定步推进与日出/日落事件广播、渲染与编辑器共享的 `EnvironmentSnapshot`，以及 C# 脚本控制接口。

## 一句话定位

`TODProfile` 保存一天（`[0, 1)`）的太阳/天空/雾效关键帧；`TODSystem` 在每个固定步推进时间、产出带边界事件的昼夜状态，并向渲染帧数据、编辑器预览和 C# 脚本提供同一份可插值的 `EnvironmentSnapshot`。

## 数据流：从 `.tod.json` 到画面与脚本

```text
  TODProfile (FlareEngine.TODProfile JSON, keys[0..255])
        │  SceneManager::ResolveActiveEnvironment()
        ▼
  TODSystem (归属 Environment)
    ├─ 固定步: Advance(dt) ──▶ 计算跨越的日出(20)/日落(21) ──▶ EventBus (AfterLogic)
    ├─ 脚本改写: SetTime() / SetRate() ──▶ 发布 TODTimeChanged(19)
    └─ 渲染取样: Snapshot(frame, alpha) ──▶ SampleTODProfile() + 正/负半波算 sunDirection
                                                    │
                    ┌───────────────────────────────┼──────────────────────────────┐
                    ▼                               ▼                              ▼
         BuildFrameData (渲染帧)            TODPreview (编辑器独立预览)     TimeOfDayService (C# SDK)
```

## 核心源码拆解

### 1. 关键帧资产与环形插值（`TODProfile`）

[`TODProfile`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Environment/TODProfile.h#L17) 位于 `ResourceLayer/Environment`，定义一整天的时长 `dayLengthSeconds`（默认 600 秒）、初始时刻 `initialTime`、默认倍率 `rate`、日出/日落分界点（`0 <= sunrise < sunset < 1`）、太阳方位角 `azimuthRadians`、最大仰角 `maximumElevationRadians`，以及 1~256 个严格递增的 `TODKeyframe`。

来源：[TODProfile.h#L10](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Environment/TODProfile.h#L10) · [TODProfile.cpp#L68](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Environment/TODProfile.cpp#L68)

```cpp
// Source/ResourceLayer/Environment/TODProfile.h
struct TODParameters
{
    Vec3 sunColor{1, 1, 1}, skyColor{0.02f, 0.04f, 0.08f}, fogColor{0.5f, 0.6f, 0.7f};
    float sunIntensity = 1, fogDensity = 0;
};
struct TODKeyframe { double time = 0; TODParameters value; };

// Source/ResourceLayer/Environment/TODProfile.cpp
TODParameters SampleTODProfile(const TODProfile& p, double time)
{
    time -= std::floor(time); // 归一化到 [0, 1)
    const auto next = std::upper_bound(p.keys.begin(), p.keys.end(), time,
        [](double t, const auto& key) { return t < key.time; });
    // 首尾关键帧跨午夜 (0/1) 自动环绕插值
    const auto& b = next == p.keys.end()   ? p.keys.front() : *next;
    const auto& a = next == p.keys.begin() ? p.keys.back()  : *(next - 1);
    double begin = a.time, end = b.time;
    if (next == p.keys.begin()) begin -= 1;
    if (next == p.keys.end())   end += 1;
    const double t = p.keys.size() == 1 ? 0 : (time - begin) / (end - begin);
    // 对 sunColor / skyColor / fogColor / sunIntensity / fogDensity 线性插值...
}
```

- **序列化与向后兼容**：[`SerializeTODProfile()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Environment/TODProfile.cpp#L48) 写出 `"format": "FlareEngine.TODProfile"`、`"version": 1`；[`ParseTODProfile()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Environment/TODProfile.cpp#L34) 同时只读兼容旧名 `"SelfEngine.TODProfile"`；[`SaveTODProfile()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Environment/TODProfile.cpp#L59) 采用临时文件 + `MoveFileExW` 原子落盘。
- **场景绑定配置**：`World` 保存轻量结构 `SceneEnvironmentSettings{ todProfile, preserveTime }`（不保存运行中时刻）。切换场景时若 `preserveTime == true`，`TODSystem::SelectProfile()` 会沿用当前天数与时刻而不重置到 `initialTime`。

### 2. 模拟推进、事件预算与太阳方向计算（`TODSystem`）

[`TODSystem`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/TODSystem.h#L20) 在每个固定步由 `Environment::FixedStep()` 调用 [`Advance(seconds)`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/TODSystem.cpp#L75) 推进：

- **事件预算防雪崩**：单次步进或 `SetTime(..., replayBoundaries = true)` 允许产生的日出/日落边界事件总数上限为 `EventBudget = 64`（相应要求 `step * rate / dayLengthSeconds <= 32`），超出返回 `"TOD.EventBudget"`。跨越的所有边界按时间排序，打包成 24 字节小端 `TODEvent{ day, time, simulationSeconds }`，经 `EventBus` 在 `EventPhase::AfterLogic` 发布（类型 ID：`TODTimeChanged = 19`、`TODSunrise = 20`、`TODSunset = 21`）。
- **连续太阳轨迹与插值快照**：[`Snapshot(frame, interpolation)`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/TODSystem.cpp#L86) 在上一固定步状态 `previous_` 与当前状态 `state_` 间插值，并把白天映射到正半波 `[0, π]`、夜间映射到负半波 `[π, 2π]`，使太阳方向在日出/日落边界完全连续：

```cpp
// Source/FunctionLayer/Runtime/TODSystem.cpp
const auto daylightLength = profile_->sunset - profile_->sunrise;
double angle;
if (result.daylight)
    angle = (result.time - profile_->sunrise) / daylightLength * std::numbers::pi;
else
{
    double night = result.time - profile_->sunset;
    if (night < 0) night += 1;
    angle = std::numbers::pi + night / (1 - daylightLength) * std::numbers::pi;
}
const double elevation = std::sin(angle) * profile_->maximumElevationRadians;
const double azimuth   = profile_->azimuthRadians;
// 右手、Y 向上；从太阳射向世界的单位方向
result.sunDirection = {
    static_cast<float>(-std::cos(elevation) * std::sin(azimuth)),
    static_cast<float>(-std::sin(elevation)),
    static_cast<float>(-std::cos(elevation) * std::cos(azimuth))
};
```

### 3. 编辑器独立预览（`TODPreview`）与 C# 脚本接口（`TimeOfDayService`）

- **编辑器隔离预览**：[`TODPreview`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/TODPreview.h#L9) 内部独占一个独立的 `Environment` 实例与撤销/重做栈，允许美术在预览窗口随意拖动时间条、调整关键帧并保存 `.tod.json`，而完全不干扰正在运行的 Play 上下文。
- **C# 托管访问**：[`FlareEngine.TimeOfDayService`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/TimeOfDay.cs#L14) 通过三个 C ABI 入口桥接至运行时 `TODSystem`：
  - `Snapshot`（`flare_tod_snapshot`）：获取当前 `EnvironmentSnapshot`（含 `Active`、`Day`、`Time`、`Daylight`、`SunDirection`、太阳/天空/雾颜色及强度）。
  - `SetTime(long day, double time, bool replayBoundaries = false)`（`flare_tod_set_time`）：跳转至指定天数与归一化时刻 `time ∈ [0, 1)`，可选重放跳过的日出/日落事件。
  - `SetRate(double rate)`（`flare_tod_set_rate`）：动态修改时间流速倍率（传 `0` 可冻结昼夜流逝）。

## 常用接口速查

| 类 / 模块 | 接口 | 源码位置 |
| --- | --- | --- |
| `TODProfile` | `ValidateTODProfile` / `ParseTODProfile` / `SaveTODProfile` / `SampleTODProfile` | [TODProfile.h#L31](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Environment/TODProfile.h#L31) |
| `TODSystem` | `SelectProfile` / `SetTime` / `SetRate` / `Advance` / `Snapshot` / `DecodeTODEvent` | [TODSystem.h#L20](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/TODSystem.h#L20) |
| `TODPreview` | `Open` / `Apply` / `SetTime` / `Advance` / `SetPaused` / `Undo` / `Redo` / `Save` | [TODPreview.h#L9](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/TODPreview.h#L9) |
| `TimeOfDayService` (C#) | `Snapshot` / `SetTime(day, time, replayBoundaries)` / `SetRate(rate)` | [TimeOfDay.cs#L14](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/TimeOfDay.cs#L14) |

::: tip `BuildFrameData` 中的昼夜接入条件
只有当 `environment.TimeOfDay().Profile()` 非空（即主场景配置了有效的 `todProfile`）时，[`BuildFrameData(const Environment&, ...)`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Application/FrameData.cpp#L9) 才会写入 `output.timeOfDay` 并调用 `ApplyTimeOfDayFrameData()` 驱动主太阳光与天空雾效。如果场景未绑定 Profile，引擎仍保留手动放置的 `Directional` 灯光与静态环境设置。
:::

::: warning 为什么 `SetTime(..., replayBoundaries: true)` 不允许往回跳？
在 `TODSystem::Boundaries()` 中，回放日出/日落边界只支持时间向前推进（`end >= state_.time`），若开启 `replayBoundaries` 且目标时刻早于当前时刻，会直接返回错误键 `"TOD.ReplayBackwards"`。需要倒流时间时请保持 `replayBoundaries = false`。
:::
