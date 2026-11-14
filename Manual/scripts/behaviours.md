# C# 行为、绑定与事件

## 默认编辑器的脚本入口

在场景层级选中物体后，属性面板显示脚本类名输入框和“创建 C# 行为”。创建后会用配置的外部代码编辑器或系统关联程序打开源文件。脚本源文件也显示在资源列表，选中后可从属性面板打开编辑。

保存 C# 文件后使用工具栏“构建并重启”。编译成功，选中物体的属性面板会列出可添加的脚本类型。点击“添加脚本”后可以修改暴露字段、启停或移除脚本，再点击“保存脚本绑定”。脚本文档提供独立撤销和重做，运行期间禁止修改绑定结构和字段。

Toolkit 和 ImGui 的脚本数据来自同一宿主 query/apply 协议。Toolkit 的字段转换与文件操作集中在 `EditorScriptWorkflow`，字段合法性及绑定保存仍由托管 `ScriptBindings` 校验。

## Behaviour

[Behaviour](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/Behaviour.cs#L17) 是脚本基类。[ScriptRegistry](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/Behaviour.cs#L43) 从程序集发现可实例化的派生类型，按脚本 ID 登记。[ScriptSystem](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/ScriptSystem.cs#L3) 将实例绑定到实体，在启用、更新、固定步和销毁阶段分发回调。内置派生类有 [Mover](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/BuiltInScripts/Mover.cs#L6)、[Rotator](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/BuiltInScripts/Mover.cs#L19)、[Menu](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/BuiltInScripts/Menu.cs#L7)、[PlayerCharacter](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/BuiltInScripts/Menu.cs#L30)、[MenuFlow](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/BuiltInScripts/MenuFlow.cs#L14)、[MenuWorld](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/BuiltInScripts/MenuWorld.cs#L7) 和 [PhysicsPlayground](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/BuiltInScripts/PhysicsPlayground.cs#L7)。

## 从绑定文件到实体

[ScriptBindings](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/ScriptBindings.cs#L12) 保存实体、脚本 ID、启用状态及字段的绑定文档。[ScriptComponentBinding](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/ScriptComponentBinding.cs#L4) 将托管行为接到原生组件生命周期；场景变化时用稳定实体引用重新解析目标。字段怎样恢复见[脚本字段与程序集](lifetime.md)。

## 事件与 UI 动作

[ScriptEvents](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/ScriptEvents.cs#L58) 登记事件类型并在脚本和原生事件总线之间编解码载荷。[UiActionBindings](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/UiActionBindings.cs#L4) 以实体、脚本和动作匹配 UI 事件，[ScriptCommands](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/ScriptCommands.cs#L4) 按命令键找到处理器。订阅或注册返回的令牌用于解除对应关系。
