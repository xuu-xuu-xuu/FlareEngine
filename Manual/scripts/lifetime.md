# 脚本反射、程序集卸载与 GC

## 字段怎样进入属性面板

[ExposeAttribute](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/Behaviour.cs#L9) 标记需要暴露的实例字段。[ScriptFields](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/ScriptFields.cs#L10) 用 .NET 反射枚举公开和非公开字段，筛选带标记、可写的实例字段。[Describe()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/ScriptFields.cs#L44) 生成字段描述，[Save()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/ScriptFields.cs#L48) 与 [Load()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/ScriptFields.cs#L69) 在可支持的字段值和 JSON 之间转换。C++ 组件对应[原生元数据](../runtime/metadata.md)。

## ScriptProject

[ScriptProject](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/ScriptProject.cs#L7) 为项目建立可卸载的 [ProjectContext](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/ScriptProject.cs#L9)。启动时加载候选程序集并解析允许的行为类型，停止时解除脚本引用并请求卸载上下文。托管 GC 在引用释放后回收对象，卸载请求与完成回收之间可以相隔多个 GC 周期。

## 原生对象由谁释放

脚本字段会沿继承链枚举，包含基类的私有 `[Expose]` 字段。字段名排序稳定，同名字段产生诊断。字段描述保留为普通数据，检查元数据时加载的上下文通过弱引用观察卸载。

## 原生逻辑对象的追踪回收

`World::Objects()` 返回当前世界拥有的 `ObjectHeap`。对象派生自 `HeapObject`，在 `Trace(ObjectTracer&)` 中枚举需要保活的强边。普通 `ObjectHandle<T>` 为弱引用，`Root(handle)` 返回显式根令牌。释放最后一个根后，下一次收集可回收整个不可达环。

`World::CollectObjects()` 在世界更新完成的安全点执行非移动标记清扫。句柄包含堆身份、槽位和代数。编辑世界与播放世界各自拥有堆，清空世界时释放堆，即使外部仍保存旧根令牌也无法恢复旧对象。GPU 资源仍由对应后端等待 Fence。

C# 可使用 `EngineSession.CreateLogicState()` 创建可相互 `Link()` 的逻辑状态，通过 `Retain()` 持有根令牌并在使用结束后 `Dispose()`。普通托管代理只携带弱令牌。`CollectLogicObjects()` 在安全点返回收集后的存活数，在更新回调内调用会被拒绝。当前回收采用显式安全点调用，单个场景卸载时先释放该场景拥有的根。

[EngineSession](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/EngineSession.cs#L25) 为回调分配弱 GC 句柄，通过 [NativeSessionHandle](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/NativeMethods.cs#L102) 持有原生会话。会话的 [Dispose()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/EngineSession.cs#L161) 走显式关闭路径，句柄的 [ReleaseHandle()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/NativeMethods.cs#L107) 收尾。实体和组件由[原生世界](../runtime/entities.md)管理，GPU 资源另受[提交 Fence](../render/device.md)约束。
