#pragma once
#include "EditorUi/EditorPanels.h"
#include "FunctionLayer/GameUI/UiTree.h"
class Localization;
namespace flare
{
std::vector<ui::editor::EditorProperty> UiInspectorRows(const UiTree&,UiId,const Localization&,bool locked);
bool ApplyUiInspector(UiTree&,UiId,const std::string& key,const std::string& value,UiId& selected);
}
