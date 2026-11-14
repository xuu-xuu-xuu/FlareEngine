#pragma once
#include "EditorUi/EditorPanels.h"
class Localization;
namespace se
{
class TODPreview;class AssetStore;class EditorSession;
void AppendTodInspector(TODPreview&,const Localization&,bool locked,std::vector<ui::editor::EditorProperty>&);
bool ApplyTodInspector(TODPreview&,AssetStore&,EditorSession&,const std::string&,const std::string&);
}
