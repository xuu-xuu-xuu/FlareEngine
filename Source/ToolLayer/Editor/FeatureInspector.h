#pragma once
#include "EditorUi/EditorPanels.h"
#include "FunctionLayer/Runtime/RuntimeHandles.h"
class Localization;
namespace flare
{
class AssetStore;
struct ComponentDescriptor;
class EditorSession;
void AppendSingleFeatureComponent(const EditorSession& session, const Localization& text, StableEntityId id,
                                  const ComponentDescriptor& descriptor, ComponentHandle handle,
                                  std::vector<ui::editor::EditorProperty>& rows, const AssetStore* assets = nullptr);
void AppendMissingFeatureAddActions(const EditorSession& session, const Localization& text, StableEntityId id,
                                    std::vector<ui::editor::EditorProperty>& rows);
void AppendFeatureInspector(const EditorSession&, const Localization&, StableEntityId,
                            std::vector<ui::editor::EditorProperty>&, const AssetStore* assets = nullptr);
std::optional<bool> ApplyFeatureInspector(EditorSession&, StableEntityId, const std::string&,
                                          const std::string&, const AssetStore* assets = nullptr);
}
