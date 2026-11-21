# 脚本反射、特性元数据、程序集卸载与 GC

## 字段与特性怎样进入属性面板

`[F_Expose]`（`F_ExposeAttribute`）标记需要序列化并暴露到编辑器属性面板的实例字段。`ScriptFields` 使用 .NET 反射沿继承链枚举公开与非公开的可写实例字段（包含基类私有 `[F_Expose]` 字段，同名字段产生诊断），支持 `bool`、`int`、`float`、`double`、`string`、`Vector2`、`Vector3`、`Vector4`、枚举、`EntityReference` 与 `AssetReference`。

在此基础上，字段可叠加以下修饰特性驱动卡片式属性面板渲染：

| 特性 | 作用 | 元数据传递方式 |
| :--- | :--- | :--- |
| `[F_Header(string title)]` | 在该字段上方插入金色分组小标题与分隔线 | 编码进 `ScriptFieldInfo` 描述前缀，由 `EditorInspector` 解析为 `SectionHeader` |
| `[F_Range(float min, float max)]` | 将数值字段渲染为带实时填充条的拖拽滑块 + 精确输入框 | 写入 `PropertyMetadata::minValue / maxValue` 并在两侧钳制 |
| `[F_Tooltip(string text)]` | 为字段提供编辑器提示文本 | 随字段描述表传递 |

`ScriptFields.Describe()` 生成稳定排序的字段元数据，`Save()` 与 `Load()` 在字段值与 `.bindings.json` 之间无损转换，`CopyExposedFields()` 则在 `F_Scene.Instantiate()` 克隆实体时复制全部暴露字段值。C++ 原生组件对应[原生组件反射](../runtime/metadata.md)。

## ScriptProject

`ScriptProject` 为项目建立可回收的 `ProjectContext`（继承 `AssemblyLoadContext(isCollectible: true)`）。`RunEditor` 允许初始无脚本工程以空程序集启动编辑器外壳；每次编译或热重载时通过内存流 `LoadFromStream` 加载候选 DLL 与 PDB，绝不占用磁盘文件句柄。停止运行或完成原位 `ReloadAssembly` 热替换后，宿主解除旧委托、事件与实例引用并调用 `Unload()`，由 .NET GC 回收旧程序集上下文。检查元数据时仅通过 `WeakReference` 观察卸载状态，不阻断回收。

## 原生逻辑对象的追踪回收

`World::Objects()` 返回当前世界拥有的 `ObjectHeap`。对象派生自 `HeapObject`，在 `Trace(ObjectTracer&)` 中枚举需要保活的强边。普通 `ObjectHandle<T>` 为弱引用，`Root(handle)` 返回显式根令牌。释放最后一个根后，下一次收集可回收整个不可达环。

`World::CollectObjects()` 在世界更新完成的安全点执行非移动标记清扫。句柄包含堆身份、槽位和代数。编辑世界与播放世界各自拥有堆，清空世界时释放堆，即使外部仍保存旧根令牌也无法恢复旧对象。GPU 资源仍由对应后端等待 Fence。

C# 可使用 `EngineSession.CreateLogicState()` 创建可相互 `Link()` 的逻辑状态，通过 `Retain()` 持有根令牌并在使用结束后 `Dispose()`。普通托管代理只携带弱令牌。`CollectLogicObjects()` 在安全点返回收集后的存活数，在更新回调内调用会被拒绝。单个场景卸载时先释放该场景拥有的根。

`EngineSession` 为回调分配弱 GC 句柄，通过 `NativeSessionHandle` 持有原生会话。会话的 `Dispose()` 走显式关闭路径，句柄的 `ReleaseHandle()` 收尾。实体和组件由[原生世界](../runtime/entities.md)管理，GPU 资源另受[提交 Fence](../render/device.md)约束。
