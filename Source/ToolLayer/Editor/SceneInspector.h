#pragma once

#include "EditorUi/EditorPanels.h"
class Localization;
namespace flare
{
class AssetStore;
class EditorSession;

// Toolkit 场景属性的展示适配。World/EditorSession 继续拥有数据、检查和撤销。
void AppendSceneInspector(const EditorSession& session, const AssetStore& assets, const Localization& text,
                          std::uint64_t entity, std::vector<ui::editor::EditorProperty>& result);
void AppendMultiSceneInspector(const EditorSession& session, const AssetStore& assets, const Localization& text,
                               const std::vector<std::uint64_t>& entities, std::vector<ui::editor::EditorProperty>& result);
// 只在点击选择器时查询目录或加载动画模型；普通的属性刷新不解析模型。
std::string FillSceneInspectorChoices(const EditorSession& session, AssetStore& assets, const Localization& text,
                                      std::uint64_t entity, ui::editor::EditorProperty& field);
bool ApplySceneInspector(EditorSession& session, std::uint64_t entity, const std::string& key,
                         const std::string& value, AssetStore* assets = nullptr);
bool ApplyMultiSceneInspector(EditorSession& session, const std::vector<std::uint64_t>& entities,
                              const std::string& key, const std::string& value, AssetStore* assets = nullptr);
}
