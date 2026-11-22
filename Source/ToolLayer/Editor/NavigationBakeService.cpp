#include "NavigationBakeService.h"
#include "FunctionLayer/Navigation/SceneNavigation.h"
#include "FunctionLayer/Runtime/FeatureComponents.h"
#include "FunctionLayer/Runtime/World.h"
#include "ResourceLayer/Assets/AssetIO.h"

namespace flare
{
NavigationBakeService::NavigationBakeService()
    : queue_(1u, 16u)
{
}

NavigationBakeService::~NavigationBakeService()
{
    CancelAndWait();
    queue_.Shutdown();
}

bool NavigationBakeService::StartBake(
    const World& world,
    AssetStore& assets,
    const std::filesystem::path& outputPath,
    const NavBakeSettings& settings,
    std::function<void(const NavigationBakeResult&)> onComplete,
    std::string& errorKey)
{
    errorKey.clear();
    if (Busy())
    {
        errorKey = "Navigation.BakeBusy";
        return false;
    }

    std::vector<Vec3> vertices;
    std::vector<std::uint32_t> indices;
    CollectNavigationSceneGeometry(world, &assets, vertices, indices);
    if (vertices.empty() || indices.size() < 3u)
    {
        errorKey = "Navigation.EmptyGeometry";
        return false;
    }

    auto token = queue_.Submit(
        [vertices = std::move(vertices),
         indices = std::move(indices),
         settings,
         outputPath,
         &assets,
         onComplete = std::move(onComplete)](const CancellationToken& cancel) mutable -> TaskQueue::Completion {
            NavigationBakeResult result;
            result.outputPath = outputPath;
            if (cancel.IsCancelled())
            {
                result.errorKey = "Navigation.BakeCancelled";
                return [result = std::move(result), onComplete = std::move(onComplete)]() mutable {
                    if (onComplete) onComplete(result);
                };
            }

            auto nav = std::make_shared<NavMeshData>();
            std::string bakeError;
            if (!BakeNavMesh(vertices, indices, settings, *nav, bakeError))
            {
                result.errorKey = bakeError.empty() ? "Navigation.BakeFailed" : std::move(bakeError);
            }
            else if (cancel.IsCancelled())
            {
                result.errorKey = "Navigation.BakeCancelled";
            }
            else
            {
                std::vector<std::uint8_t> bytes;
                if (!nav->Serialize(bytes) || !asset_io::WriteAtomic(outputPath, bytes))
                {
                    result.errorKey = "Assets.WriteFailed";
                }
                else
                {
                    result.success = true;
                    result.navMesh = std::move(nav);
                }
            }

            return [result = std::move(result), &assets, onComplete = std::move(onComplete)]() mutable {
                if (result.success)
                {
                    std::vector<AssetDiagnostic> diagnostics;
                    AssetId id;
                    if (assets.RegisterSource(result.outputPath, {}, id, diagnostics))
                    {
                        result.assetId = std::move(id);
                    }
                }
                if (onComplete) onComplete(result);
            };
        });

    if (!token.Accepted())
    {
        errorKey = "Navigation.BakeBusy";
        return false;
    }
    active_ = std::move(token);
    return true;
}

std::size_t NavigationBakeService::Poll()
{
    return queue_.Pump(16u);
}

bool NavigationBakeService::Busy() const noexcept
{
    const auto status = active_.Status();
    return status == TaskStatus::Queued || status == TaskStatus::Running || status == TaskStatus::Ready
        || status == TaskStatus::Publishing;
}

void NavigationBakeService::CancelAndWait()
{
    active_.Cancel();
    while (Busy())
    {
        queue_.WaitForReady(std::chrono::milliseconds(5));
        queue_.Pump(16u);
    }
}

bool NavigationBakeService::HasUnassignedNavAgent(const World& world)
{
    for (const EntityHandle entity : world.Entities())
    {
        if (!world.FindComponent(entity, NavigationAgentComponent::TypeId)) continue;
        const auto current = FeatureValue(world, entity, "navagent.asset");
        const auto* currentId = std::get_if<std::string>(&current);
        if (!currentId || currentId->empty()) return true;
    }
    return false;
}

std::size_t NavigationBakeService::MountBakedNavMeshToWorld(
    World& world, const AssetId& assetId, bool overwriteExisting)
{
    if (assetId.empty()) return 0u;
    std::size_t updated = 0u;
    for (const EntityHandle entity : world.Entities())
    {
        const auto handle = world.FindComponent(entity, NavigationAgentComponent::TypeId);
        if (!handle) continue;
        const auto current = FeatureValue(world, entity, "navagent.asset");
        const auto* currentId = std::get_if<std::string>(&current);
        if (!overwriteExisting && currentId && !currentId->empty()) continue;
        std::string error;
        if (flare::WriteProperty(world, entity, "navagent.asset", PropertyValue{assetId}, error))
        {
            ++updated;
        }
    }
    return updated;
}
} // namespace flare
