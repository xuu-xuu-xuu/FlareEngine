#pragma once
#include "CoreLayer/Math/Math.h"
#include "CoreLayer/MaterialAlpha.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace flare
{
inline constexpr std::size_t MaxSurfaceShaderVec4Slots = 8;
inline constexpr std::size_t MaxSurfaceShaderPasses = 8;

enum class SurfaceShadingModel : std::uint8_t
{
    Lit = 0,
    Unlit = 1,
    Toon = 2,
};

enum class SurfaceCullMode : std::uint8_t
{
    Back = 0,
    Off = 1,
    Front = 2,
};

enum class SurfaceBlendPreset : std::uint8_t
{
    Opaque = 0,
    Cutout = 1,
    Transparent = 2,
    Additive = 3,
};

enum class SurfaceDepthTest : std::uint8_t
{
    LessEqual = 0,
    Greater = 1,
    Always = 2,
};

enum class SurfaceParamKind : std::uint8_t
{
    Float = 0,
    Range,
    Color,
    Vector2,
    Vector3,
    Vector4,
};

struct SurfaceShaderParam
{
    std::string name;
    std::string label;
    SurfaceParamKind kind = SurfaceParamKind::Float;
    Vec4 defaultValue{0.0f, 0.0f, 0.0f, 1.0f};
    float minValue = 0.0f;
    float maxValue = 1.0f;
    std::uint32_t slotIndex = 0;       // 0..7 in flareUserParams[8]
    std::uint32_t componentOffset = 0; // 0..3 (.x .y .z .w)
    std::uint32_t componentCount = 1;  // 1..4
};

struct SurfaceShaderPass
{
    std::string name;
    SurfaceShadingModel shadingModel = SurfaceShadingModel::Lit;
    std::optional<SurfaceBlendPreset> blendPreset;
    std::optional<SurfaceCullMode> cullMode;
    std::optional<bool> zWrite;
    SurfaceDepthTest zTest = SurfaceDepthTest::LessEqual;
    bool hasVertexHook = false;
    bool usesUnitySurfaceEntry = false;
    bool usesUnityVertexEntry = false;
    bool vertHasOutInput = false;
    std::string includeHlsl;
    std::uint32_t includeStartLine = 1;
    std::string bodyHlsl;
    std::uint32_t bodyStartLine = 1;
};

struct SurfaceShaderMetadata
{
    std::string displayName;
    SurfaceShadingModel shadingModel = SurfaceShadingModel::Lit;
    std::optional<MaterialAlphaMode> blendOverride;
    std::optional<bool> doubleSidedOverride;
    bool hasVertexHook = false;
    bool usesUnitySurfaceEntry = false;
    bool usesUnityVertexEntry = false;
    bool usesAlphaCoverage = false;
    std::optional<std::size_t> shadowPassIndex = 0;
    std::vector<SurfaceShaderParam> parameters;
    std::vector<SurfaceShaderPass> passes;
    std::string bodyHlsl;
    std::uint32_t bodyStartLine = 1;

    [[nodiscard]] std::size_t PassCount() const noexcept { return passes.empty() ? 1u : passes.size(); }
    [[nodiscard]] SurfaceShaderPass EffectivePass(std::size_t passIndex) const;
};

[[nodiscard]] bool IsSurfaceShaderHlslPath(const std::filesystem::path& path);
[[nodiscard]] bool IsShaderGraphPath(const std::filesystem::path& path);
[[nodiscard]] bool IsUserSurfaceAssetPath(const std::filesystem::path& path);

[[nodiscard]] bool ParseSurfaceShaderSource(std::string_view hlslText, SurfaceShaderMetadata& output,
                                            std::string& errorMessage);

[[nodiscard]] std::array<Vec4, MaxSurfaceShaderVec4Slots> PackSurfaceShaderParameters(
    const SurfaceShaderMetadata& metadata,
    const std::map<std::string, Vec4>& overrides);

[[nodiscard]] std::string MakeDefaultSurfaceShaderTemplate(std::string_view displayName);

// Generates the HLSL header included by Pbr.hlsl when SE_USER_SURFACE=1.
[[nodiscard]] std::string BuildSurfaceShaderUserInclude(const SurfaceShaderMetadata& metadata,
                                                        std::string_view sourceDebugName,
                                                        std::size_t passIndex = 0);

// Stages runtime shader includes + generated wrapper variant into cacheDir and returns the variant .hlsl path.
[[nodiscard]] bool StageSurfaceShaderVariant(const std::filesystem::path& builtinShadersDir,
                                             const std::filesystem::path& cacheRootDir,
                                             const std::filesystem::path& userSourcePath,
                                             const SurfaceShaderMetadata& metadata,
                                             std::string_view baseBuiltinVariantFile,
                                             std::filesystem::path& outVariantHlslPath,
                                             std::string& outContentKey,
                                             std::string& errorMessage,
                                             std::size_t passIndex = 0);
} // namespace flare
