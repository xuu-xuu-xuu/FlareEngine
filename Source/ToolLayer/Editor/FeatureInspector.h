#pragma once
#include "EditorUi/EditorPanels.h"
#include "FunctionLayer/Runtime/RuntimeHandles.h"
class Localization;
namespace se
{
class EditorSession;
void AppendFeatureInspector(const EditorSession&,const Localization&,StableEntityId,std::vector<ui::editor::EditorProperty>&);
std::optional<bool> ApplyFeatureInspector(EditorSession&,StableEntityId,const std::string&,const std::string&);
}
