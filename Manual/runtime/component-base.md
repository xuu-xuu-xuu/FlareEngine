# 组件基类与生命周期

这篇文档说明 Flare 引擎原生组件的共同基类 `ComponentBase`、它的 4 阶段生命周期状态机、异常熔断保护，以及内置组件和引擎扩展组件（`FeatureComponent`）的继承结构。

## 一句话定位

`flare::ComponentBase` 是所有原生运行时组件的统一底座，它把**组件的运行身份与生命周期状态**交由 [`flare::World`](world.md#world) 独占管理，而派生类只需专注保存业务属性和覆写生命周期钩子。

## 生命周期状态机

```text
                    World::AttachComponent()
  [Detached] ───────────────────────────────────────▶ [Attached] (触发 OnAttach)
      ▲                                                 │   ▲
      │ (仅 Staging 暂存世界停留于此，                  │   │
      │  待 ActivateStagedComponents 统一激活)          │   │ Enabled() && IsEnabled(owner)
      │                                         变 true │   │ 变 false（组件/实体/祖先禁用）
      │                                                 ▼   │
      │                                               [Active] (触发 OnEnable / OnDisable)
      │                                                 │
      │             World::ReleaseComponent()           ▼
      └──────────────────── (释放池槽位) ◀────────── [Removing] (先 OnDisable 再 OnDetach)
```

## ComponentBase

[`flare::ComponentBase`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/ComponentBase.h#L10) 将运行身份字段（所属实体、组件句柄、类型 ID、启用/活跃标志、生命周期阶段、故障熔断标志）全部设为 `private`，只对 `friend class World` 开放写入。

来源：[ComponentBase.h#L7](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/ComponentBase.h#L7)

```cpp
// Source/FunctionLayer/Runtime/ComponentBase.h
enum class ComponentLifecycle { Detached, Attached, Active, Removing };
enum class ComponentEventKind { Attach, Enable, Disable, Detach };
struct ComponentEvent
{
    ComponentEventKind kind;
    EntityHandle owner;
    ComponentHandle component;
    ComponentTypeId type;
};

class ComponentBase
{
public:
    ComponentBase() = default;
    virtual ~ComponentBase() = default;

    // 关键设计：拷贝/移动只更新派生类属性，刻意不复制运行身份！
    ComponentBase(const ComponentBase&) noexcept {}
    ComponentBase(ComponentBase&&) noexcept {}
    ComponentBase& operator=(const ComponentBase&) noexcept { return *this; }
    ComponentBase& operator=(ComponentBase&&) noexcept { return *this; }

    EntityHandle Owner() const noexcept { return owner_; }
    ComponentHandle Handle() const noexcept { return handle_; }
    ComponentTypeId Type() const noexcept { return type_; }
    bool Enabled() const noexcept { return enabled_; }
    bool Active() const noexcept { return active_; }
    ComponentLifecycle Lifecycle() const noexcept { return lifecycle_; }

protected:
    virtual void OnAttach() {}
    virtual void OnEnable() {}
    virtual void OnDisable() {}
    virtual void OnDetach() {}

private:
    friend class World;
    EntityHandle owner_{};
    ComponentHandle handle_{};
    ComponentTypeId type_ = 0;
    bool enabled_ = true, active_ = false, faulted_ = false;
    ComponentLifecycle lifecycle_ = ComponentLifecycle::Detached;
};
```

### 为什么拷贝赋值函数体是空的？

这是为了支持“候选值校验 + 安全原地覆写”模式：外部调用 `UpdateRegisteredComponent(handle, candidate)` 时，只需拷出一份组件副本修改属性，验证通过后执行 `*current = candidate;`。因为基类的 `operator=` 什么也不做，池中真实组件的 `owner_`、`handle_`、`lifecycle_` 不会被临时副本里空白的默认值覆盖。

### 异常隔离与故障熔断

[`World::InvokeLifecycle()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/WorldComponents.cpp#L53) 负责调用四个受保护钩子。一旦某个组件在回调中抛出未捕获异常，引擎不会让整个主循环崩溃，而是将其标记为 `faulted_ = true` 并强制禁用（`enabled_ = false`），记录错误键 `"World.ComponentCallbackFailed"`。之后若外部再试图重新启用该故障组件，`SetComponentEnabled(..., true)` 会直接返回 `false`（`"World.ComponentFaulted"`）。

```cpp
// Source/FunctionLayer/Runtime/WorldComponents.cpp
bool World::InvokeLifecycle(ComponentBase& component, ComponentEventKind kind)
{
    const auto previous = callbackOwner_;
    callbackOwner_ = component.Owner();
    ++lifecycleDepth_;
    bool result = true;
    try
    {
        switch (kind)
        {
        case ComponentEventKind::Attach:  component.OnAttach();  break;
        case ComponentEventKind::Enable:  component.OnEnable();  break;
        case ComponentEventKind::Disable: component.OnDisable(); break;
        case ComponentEventKind::Detach:  component.OnDetach();  break;
        }
    }
    catch (...)
    {
        component.faulted_ = true;
        component.enabled_ = false;
        result = Fail("World.ComponentCallbackFailed");
    }
    --lifecycleDepth_;
    callbackOwner_ = previous;
    return result;
}
```

### 公开只读接口速查

| 想知道什么 | 接口 | 源码位置 |
| --- | --- | --- |
| 属于哪个实体 / 组件句柄是什么？ | `Owner()` / `Handle()` | [ComponentBase.h#L20](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/ComponentBase.h#L20) |
| 组件类型 ID 是多少？ | `Type()` | [ComponentBase.h#L22](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/ComponentBase.h#L22) |
| 组件自身是否勾选启用？ | `Enabled()` | [ComponentBase.h#L23](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/ComponentBase.h#L23) |
| 综合自身 + 实体 + 祖先是否处于活跃态？ | `Active()` | [ComponentBase.h#L24](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/ComponentBase.h#L24) |
| 正处于哪个生命周期阶段？ | `Lifecycle()` | [ComponentBase.h#L25](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/ComponentBase.h#L25) |

### 直接派生类

Flare 把直接继承自 `ComponentBase` 的类分为两大阵营，且全部声明为 `final`：

1. **5 个核心内置组件**（`TypeId = 1..5`，保存在 `World` 专用池中）：
   - [`TransformComponent`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/RuntimeComponents.h#L9)（`TypeId = 1`）：保存局部变换 `Transform`，每个存活实体必有且不可移除。
   - [`CameraRuntimeComponent`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/RuntimeComponents.h#L17)（`TypeId = 2`）：包装 `CameraComponent` 镜头参数。
   - [`MeshRuntimeComponent`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/RuntimeComponents.h#L24)（`TypeId = 3`）：包装 `MeshComponent` 网格与材质引用。
   - [`LightRuntimeComponent`](light.md#lightruntimecomponent)（`TypeId = 4`）：包装 `LightComponent` 光源参数。
   - [`ScriptComponent`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/RuntimeComponents.h#L38)（`TypeId = 5`）：覆写全部 4 个生命周期钩子，将 `ComponentEvent` 转发给宿主回调（C# 托管脚本桥接即建立于此）。

2. **11 个引擎特性组件**（[`FeatureComponent<Id>`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/FeatureComponents.h#L7)，固定占用 `0x464C0001..0x464C000B`）：
   - 以统一属性数组 `std::vector<PropertyValue> values` 存储数据，直接由[原生组件反射系统](metadata.md)驱动编辑、克隆与序列化。涵盖刚体（`PhysicsBodyComponent`）、角色控制器（`CharacterSettingsComponent`）、渲染设置（`RenderSettingsComponent`）、环境贴图（`EnvironmentSettingsComponent`）、骨骼动画角色（`ActorSettingsComponent`）、矩形面光源（`RectangleLightComponent`）、碰撞体（`ColliderSettingsComponent`）、音频听者/音源（`AudioListenerComponent` / `AudioSourceComponent`）、粒子发射器（`ParticleEmitterComponent`）以及导航代理（`NavigationAgentComponent`）。

### 生命周期回调

以 [`ScriptComponent`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/RuntimeComponents.h#L46) 为例，派生类只需覆写关心的受保护钩子：

```cpp
// Source/FunctionLayer/Runtime/RuntimeComponents.h
class ScriptComponent final : public ComponentBase
{
public:
    static constexpr ComponentTypeId TypeId = ComponentTypes::Script;
    using Callback = std::function<void(const ComponentEvent&)>;
    // ...
protected:
    void OnAttach()  override { Notify(ComponentEventKind::Attach);  }
    void OnEnable()  override { Notify(ComponentEventKind::Enable);  }
    void OnDisable() override { Notify(ComponentEventKind::Disable); }
    void OnDetach()  override { Notify(ComponentEventKind::Detach);  }
private:
    void Notify(ComponentEventKind kind)
    {
        if (callback_) callback_({ kind, Owner(), Handle(), Type() });
    }
};
```

::: tip 扩展自定义原生组件
1. 继承 `flare::ComponentBase` 并标记 `final`，定义 `static constexpr ComponentTypeId TypeId = ...;`（**必须 `>= 1024`**，避开内置区 `1..5` 和引擎保留区 `0x464C0001..0x464C000B`）。
2. 通过 `World::AttachRegisteredComponent<MyComponent>(entity, value)` 挂载。
3. 如需让新组件自动出现在编辑器 Inspector、支持场景 `.json` 保存和预制体覆盖，再调用 [`RegisterComponent()`](metadata.md) 登记描述符即可。
:::

::: warning 为什么暂存世界（Staging World）里不触发 `OnAttach` / `OnEnable`？
在加载场景或克隆快照期间，`World(World::StagingTag{})` 创建的组件会停留在 `Detached` 状态。如果中途遇到 JSON 校验失败或资产缺失，直接丢弃暂存世界即可，**不会触发任何半成品的生命周期回调**。只有主线程确认原子提交后，才由 `ActivateStagedComponents()` 统一执行 `OnAttach` 与 `OnEnable`。
:::
