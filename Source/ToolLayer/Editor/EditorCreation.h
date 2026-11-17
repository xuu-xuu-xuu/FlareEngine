#pragma once
#include "EditorSession.h"
#include <filesystem>
#include <string_view>
class Localization;

namespace se
{
class AssetStore;
class World;
bool CreateEditorObject(EditorSession& session, std::string_view kind, StableEntityId parent,const Localization* text=nullptr);
bool SpawnModelEntity(World& world, AssetStore& assets, const std::string& asset,
                      const std::string& name, const Transform& transform,
                      StableEntityId& createdId, std::string& error);
bool AddModelToScene(EditorSession& session, AssetStore& assets, const std::string& asset, std::string& error);
bool CreateEditorAsset(std::string_view kind, const std::filesystem::path& project, AssetStore& assets,
                       std::filesystem::path& created, std::string& error);
}
