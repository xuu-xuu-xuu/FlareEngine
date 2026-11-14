#pragma once

#include "EditorUi/EditorPanels.h"
class Localization;
namespace se
{
class AssetStore;
class EditorSession;

// Toolkit 场景属性的展示适配。World/EditorSession 继续拥有数据、检查和撤销。
void AppendSceneInspector(const EditorSession& session, const AssetStore& assets, const Localization& text,
                          std::uint64_t entity, std::vector<ui::editor::EditorProperty>& result);
bool ApplySceneInspector(EditorSession& session, std::uint64_t entity, const std::string& key, const std::string& value);
}
