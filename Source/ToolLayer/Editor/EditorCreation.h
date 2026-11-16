#pragma once
#include "EditorSession.h"
#include <filesystem>
#include <string_view>
class Localization;

namespace se
{
class AssetStore;
bool CreateEditorObject(EditorSession& session, std::string_view kind, StableEntityId parent,const Localization* text=nullptr);
bool AddModelToScene(EditorSession& session, AssetStore& assets, const std::string& asset, std::string& error);
bool CreateEditorAsset(std::string_view kind, const std::filesystem::path& project, AssetStore& assets,
                       std::filesystem::path& created, std::string& error);
}
