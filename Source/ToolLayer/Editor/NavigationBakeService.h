#pragma once
#include "CoreLayer/TaskQueue.h"
#include "FunctionLayer/Navigation/NavMeshData.h"
#include "FunctionLayer/Navigation/NavMeshBaker.h"
#include "ResourceLayer/Assets/AssetStore.h"
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace se
{
class World;

struct NavigationBakeResult
{
    bool success = false;
    std::string errorKey;
    AssetId assetId;
    std::filesystem::path outputPath;
    std::shared_ptr<const NavMeshData> navMesh;
};

/// 编辑器异步导航网格烘焙服务：使用 CoreLayer/TaskQueue 在工作线程执行体素化与区域合并，
/// 并在主线程 Poll() 阶段原子写出 .senav、注册进 AssetStore 与挂载到当前场景。
class NavigationBakeService
{
public:
    NavigationBakeService();
    ~NavigationBakeService();

    NavigationBakeService(const NavigationBakeService&) = delete;
    NavigationBakeService& operator=(const NavigationBakeService&) = delete;

    bool StartBake(
        const World& world,
        AssetStore& assets,
        const std::filesystem::path& outputPath,
        const NavBakeSettings& settings,
        std::function<void(const NavigationBakeResult&)> onComplete,
        std::string& errorKey);

    std::size_t Poll();
    [[nodiscard]] bool Busy() const noexcept;
    void CancelAndWait();

    /// 在主线程检查当前世界是否存在未分配导航网格的活跃导航代理，并支持自动挂载。
    [[nodiscard]] static bool HasUnassignedNavAgent(const World& world);
    static std::size_t MountBakedNavMeshToWorld(
        World& world, const AssetId& assetId, bool overwriteExisting = false);

private:
    TaskQueue queue_;
    CancellationToken active_;
};
} // namespace se
