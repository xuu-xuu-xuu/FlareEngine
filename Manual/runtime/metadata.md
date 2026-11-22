# 原生组件反射与属性序列化

这篇文档讲清楚 Flare 引擎如何在不依赖 C++ RTTI 或复杂宏代码生成的前提下，靠一套轻量显式元数据表统一驱动 **Inspector 面板、场景 JSON 存档、预制体覆写、撤销重做以及组件克隆**。

## 一句话定位

`ComponentMetadata`（`FunctionLayer/Runtime`）与 `PropertyMetadata`（`CoreLayer`）提供显式注册的组件与属性描述符，任何工具或运行时系统都可以通过字符串属性键（如 `"light.intensity"`、`"body.mass"`）完成类型安全的读取、校验写入、跨世界克隆与旧版本迁移。

## 数据流：一份元数据服务五大系统

```text
               ComponentDescriptor + PropertyDescriptor[]
                 ├── 内置 4 组件 (Transform/Camera/Mesh/Light)
                 ├── 11 引擎特性组件 (AppendEngineFeatureMetadata)
                 └── 扩展自定义组件 (RegisterComponent, TypeId >= 1024)
                                     │
         ┌───────────────┬───────────┼───────────┬────────────────┐
         ▼               ▼           ▼           ▼                ▼
   SceneInspector   SceneSerializer Prefab     Undo/Redo     CloneComponent
  (表单/颜色/滑块)  (JSON读写+迁移) (覆写比对) (属性快照恢复) (重映射实体引用)
```

## 核心源码拆解

### 1. 属性值与描述符（`CoreLayer`）

[`PropertyValue`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/CoreLayer/PropertyMetadata.h#L16) 用 `std::variant` 统一表示 8 种底层存储类型（覆盖 9 种语义 `PropertyType`，其中 `AssetId` 与 `Text` 共用 `std::string`），并用 `PropertyEntityReference` 强制要求实体引用只存 64 位 `stableId`，杜绝持久化运行句柄。

来源：[PropertyMetadata.h#L12](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/CoreLayer/PropertyMetadata.h#L12)

```cpp
// Source/CoreLayer/PropertyMetadata.h
enum class PropertyType
{
    Boolean, Number, Vector3, Quaternion, AssetId, AssetList, Text, Vector4, EntityReference
};
enum class PropertyPresentation { Default, Color, Slider };

// 持久引用只保存稳定 ID；运行时句柄始终由所属 World 解析。
struct PropertyEntityReference
{
    std::uint64_t stableId = 0;
    bool operator==(const PropertyEntityReference&) const = default;
};

using PropertyValue = std::variant<
    bool, float, Vec3, Quat, std::string,
    std::vector<std::string>, Vec4, PropertyEntityReference>;

struct PropertyDescriptor
{
    std::string_view key, labelKey;       // 如 "light.color", "Property.Light.Color"
    PropertyType type;
    PropertyValue defaultValue;
    std::optional<float> minimum, maximum;
    bool editable = true;
    std::string_view serializationKey;    // 为空时取 key 最后一段（如 "color"）
    PropertyPresentation presentation = PropertyPresentation::Default;
    bool serializedOptional = false;      // 旧场景缺失该字段时是否允许回退 defaultValue
    bool serialized = true;               // false 表示仅兼容脚本/旧接口，不写入新场景
};
```

### 2. 组件描述符与注册校验（`FunctionLayer/Runtime`）

[`ComponentDescriptor`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/ComponentMetadata.h#L11) 用 4 个纯函数指针桥接组件实例，避免虚函数表膨胀，并内置版本迁移入口 `migrate`。

来源：[ComponentMetadata.h#L11](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/ComponentMetadata.h#L11) · [ComponentMetadata.cpp#L202](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/ComponentMetadata.cpp#L202)

```cpp
// Source/FunctionLayer/Runtime/ComponentMetadata.h
struct ComponentDescriptor
{
    std::string_view key, labelKey;
    std::span<const PropertyDescriptor> properties;
    bool optional = true;       // false 表示不可移除（如 Transform）
    ComponentTypeId type = 0;   // 扩展组件必须 >= 1024
    std::uint32_t version = 1;
    bool multiple = false;      // 是否允许单实体多挂
    bool (*read)(const World&, ComponentHandle, std::size_t, PropertyValue&, std::string&) = nullptr;
    bool (*write)(World&, ComponentHandle, std::size_t, const PropertyValue&, std::string&) = nullptr;
    ComponentHandle (*create)(World&, EntityHandle, std::span<const PropertyValue>, bool, std::string&) = nullptr;
    bool (*migrate)(std::uint32_t, std::vector<ComponentProperty>&, std::string&) = nullptr;
};
```

调用 [`RegisterComponent()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/ComponentMetadata.cpp#L202) 时会执行严格不变量检查：
- 组件 `type >= 1024`、`key` 非空且不含 `.`、无重复 `type` 或 `key`、最多注册 1024 种组件、每组件最多 256 个属性；
- 每个属性的 `key` 必须以 `"<component.key>."` 开头，且 **属性 `key` 和序列化键 `SerializedPropertyKey()` 在组件内各自唯一**（即便两个字段序列化键不同，也不能复用同一属性键）；
- 默认值类型必须与 `PropertyType` 严格对应并通过 `ValidatePropertyValue()`；
- `minimum` / `maximum` 必须有限且 `minimum <= maximum`。

### 3. 读写、克隆与旧版迁移

- **读取与写入**：[`ReadProperty()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/ComponentMetadata.cpp#L227) 直接按键定位属性索引并回调 `descriptor->read`；[`WriteProperty()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/ComponentMetadata.cpp#L235) 先检查世界安全点（`!IsIterating()` 且无未决命令）、值域范围和实体引用可达性（`CanResolveReference`），再委托给 `descriptor->write` 走 `World::Set*` 或 `UpdateRegisteredComponent` 完成原子提交。
- **跨世界 / 子树克隆**：[`CloneComponentProperties()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/ComponentMetadata.cpp#L304) 跳过脚本组件，将其余组件全部读出为 `ComponentProperty` 列表；若遇到 `PropertyEntityReference`，在 `references` 映射表命中的内部引用替换为新稳定 ID，外部引用则原样保留，最后在目标世界上重建。
- **版本迁移**：当反序列化遇到 `version != descriptor->version` 时，[`CreateComponentFromProperties()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/ComponentMetadata.cpp#L260) 会先调用 `descriptor->migrate`（例如内置 `Light` 组件 v1 → v2 的 [`MigrateLight()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/ComponentMetadata.cpp#L41)，把旧布尔字段 `type` 迁移为字符串枚举 `kind`），再做缺失字段补全与值域验证。

```cpp
// Source/FunctionLayer/Runtime/ComponentMetadata.cpp
bool CloneComponentProperties(
    const World& source, EntityHandle from, World& target, EntityHandle to,
    std::string& error,
    const std::unordered_map<StableEntityId, StableEntityId>& references)
{
    for (const auto handle : source.Components(from))
    {
        const auto* component = source.GetComponent(handle);
        if (component->Type() == ComponentTypes::Script) continue;
        std::vector<ComponentProperty> properties;
        if (!ReadComponentProperties(source, handle, properties, error)) return false;
        for (auto& property : properties)
            if (auto* ref = std::get_if<PropertyEntityReference>(&property.value))
                if (const auto it = references.find(ref->stableId); it != references.end())
                    ref->stableId = it->second; // 内部引用重映射，外部引用保持原稳定 ID
        const auto* descriptor = FindComponentDescriptor(component->Type());
        if (!CreateComponentFromProperties(target, to, descriptor->type,
                descriptor->version, std::move(properties), component->Enabled(), error))
            return false;
    }
    return true;
}
```

### 4. 材质属性反射

材质资产使用同一套 `PropertyDescriptor` / `PropertyValue`，通过 `serializationKey` 支持嵌套 JSON 路径（如 `"textures/baseColor"`、`"clearCoat/weight"`）。读写入口为 [`ReadMaterialProperty()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/MaterialMetadata.cpp#L42) 与 [`WriteMaterialProperty()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/MaterialMetadata.cpp#L75)，写入时会复制一份 `MaterialDocument` 并在末尾调用 `ValidateMaterialDocument()`，校验失败绝不修改原材质。

托管 C# 脚本的字段发现走独立的托管反射路径，详见[脚本反射与寿命](../scripts/lifetime.md)。

## 常用接口速查

| 函数 | 作用 | 源码位置 |
| --- | --- | --- |
| `RegisterComponent(desc, err)` | 注册扩展原生组件描述符（仅在启动/安全点调用） | [ComponentMetadata.cpp#L202](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/ComponentMetadata.cpp#L202) |
| `ComponentMetadata()` | 获取当前全部已注册组件描述符只读视图 | [ComponentMetadata.cpp#L197](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/ComponentMetadata.cpp#L197) |
| `FindComponentDescriptor(type \| key)` | 按 `ComponentTypeId` 或字符串键查找描述符 | [ComponentMetadata.cpp#L198](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/ComponentMetadata.cpp#L198) |
| `ReadProperty` / `WriteProperty` | 按 `"组件.字段"` 键读写实体上的单个属性 | [ComponentMetadata.cpp#L227](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/ComponentMetadata.cpp#L227) |
| `AddComponent` / `RemoveComponent` | 按组件键使用默认值添加组件，或移除可选组件 | [ComponentMetadata.cpp#L287](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/ComponentMetadata.cpp#L287) |
| `ReadComponentProperties` / `CreateComponentFromProperties` | 序列化层整组件导出/导入（含版本迁移） | [ComponentMetadata.cpp#L245](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/ComponentMetadata.cpp#L245) |

::: tip 引擎特性组件的向后兼容技巧
[`FeatureComponents.cpp`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/FeatureComponents.cpp#L243) 在构建 11 个引擎扩展组件描述符时，自动将所有属性标记为 `serializedOptional = true`。这样以后给 `RenderSettingsComponent` 等组件追加新配置项时，老场景 JSON 缺少该字段会自动回退注册默认值，不会破坏旧工程。
:::

::: warning 指针寿命与重新查询原则
`RegisterComponent()` 要求传入的 `properties` span 和所有 `string_view` 字面量拥有**进程级静态寿命**（推荐使用 `static const std::array` 与字符串字面量）。此外，再次注册新组件可能导致内部 `std::vector` 扩容重分配，使之前缓存的 `ComponentDescriptor*` 失效——调用方应在每次操作时重新调用 `FindComponentDescriptor()`。
:::
