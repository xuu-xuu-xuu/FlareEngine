#include "CoreLayer/Localization.h"
#include "FunctionLayer/Navigation/NavMeshBaker.h"
#include "FunctionLayer/Navigation/SceneNavigation.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Assets/AssetStore.h"
#include "ResourceLayer/Scenes/SceneSerializer.h"

#include <charconv>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
namespace
{
bool ParseFloat(std::wstring_view arg, float& outValue) noexcept
{
    std::string narrow;
    narrow.reserve(arg.size());
    for (const wchar_t wc : arg)
    {
        if (wc > 0x7Fu) return false;
        narrow.push_back(static_cast<char>(wc));
    }
    auto [ptr, ec] = std::from_chars(narrow.data(), narrow.data() + narrow.size(), outValue);
    return ec == std::errc{} && ptr == narrow.data() + narrow.size() && flare::IsFinite(outValue);
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    Localization text;
    std::string locError;
    if (!text.Load(GetExecutableDirectory() / "Resources/Localization/en-US.txt", locError))
    {
        text.Load(fs::current_path() / "Resources/Localization/en-US.txt", locError);
    }

    const auto fail = [&](const char* key, const std::string& detail = {}) -> int {
        std::cerr << text.GetText(key);
        if (!detail.empty()) std::cerr << ": " << detail;
        std::cerr << '\n';
        return 1;
    };

    if (argc < 5 || std::wstring_view(argv[1]) != L"bake")
    {
        std::cerr << text.GetText("NavigationCooker.Usage") << '\n';
        return 2;
    }

    const fs::path scenePath = fs::absolute(argv[2]);
    const fs::path contentRoot = fs::absolute(argv[3]);
    const fs::path outputPath = fs::absolute(argv[4]);

    flare::NavBakeSettings settings{};
    for (int i = 5; i < argc; ++i)
    {
        const std::wstring_view opt = argv[i];
        if (i + 1 >= argc) return fail("NavigationCooker.Usage");
        const std::wstring_view val = argv[++i];
        float parsed = 0.0f;
        if (!ParseFloat(val, parsed)) return fail("Navigation.InvalidSettings");
        if (opt == L"--cell") settings.cellSizeMeters = parsed;
        else if (opt == L"--radius") settings.agentRadiusMeters = parsed;
        else if (opt == L"--height") settings.agentHeightMeters = parsed;
        else if (opt == L"--slope") settings.maxSlopeDegrees = parsed;
        else if (opt == L"--step") settings.maxStepMeters = parsed;
        else return fail("NavigationCooker.Usage");
    }

    flare::World world;
    std::string sceneError;
    if (!flare::LoadScene(scenePath, world, sceneError))
    {
        return fail(sceneError.empty() ? "NavigationCooker.SceneLoadFailed" : sceneError.c_str(),
            flare::asset_io::Utf8(scenePath));
    }

    flare::AssetStore assets(contentRoot, outputPath.parent_path() / ".nav-cook-cache");
    std::vector<flare::AssetDiagnostic> scanDiags;
    assets.Scan(scanDiags);

    std::vector<flare::Vec3> vertices;
    std::vector<std::uint32_t> indices;
    flare::CollectNavigationSceneGeometry(world, &assets, vertices, indices);

    flare::NavMeshData navMesh;
    std::string bakeError;
    if (!flare::BakeNavMesh(vertices, indices, settings, navMesh, bakeError))
    {
        return fail(bakeError.empty() ? "Navigation.BakeFailed" : bakeError.c_str());
    }

    std::vector<std::uint8_t> bytes;
    if (!navMesh.Serialize(bytes) || !flare::asset_io::WriteAtomic(outputPath, bytes))
    {
        return fail("Assets.WriteFailed", flare::asset_io::Utf8(outputPath));
    }

    std::cout << text.GetText("NavigationCooker.Complete") << ": " << flare::asset_io::Utf8(outputPath)
              << " (" << navMesh.Polygons().size() << " polygons)\n";
    return 0;
}
