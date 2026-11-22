#pragma once
#include "SurfaceShader.h"
#include "StaticModel.h"
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace flare
{
enum class ShaderPortValueType : std::uint8_t
{
    Float = 0,
    Vec2 = 1,
    Vec3 = 2,
    Vec4 = 3,
};

struct ShaderNodePortSpec
{
    std::string id;
    std::string label;
    ShaderPortValueType type = ShaderPortValueType::Float;
    Vec4 defaultValue{0.0f, 0.0f, 0.0f, 1.0f};
};

struct ShaderNodeTypeSpec
{
    std::string type;
    std::string category;
    std::string title;
    std::vector<ShaderNodePortSpec> inputs;
    std::vector<ShaderNodePortSpec> outputs;
    bool isMaster = false;
    bool canExposeParam = false;
};

struct ShaderGraphNode
{
    std::uint32_t id = 0;
    std::string type;
    float x = 0.0f;
    float y = 0.0f;
    // Property / constant settings
    bool exposeAsParam = false;
    std::string paramName;
    std::string paramLabel;
    Vec4 constantValue{0.0f, 0.0f, 0.0f, 1.0f};
    float rangeMin = 0.0f;
    float rangeMax = 1.0f;
    // Per-unconnected-input fallback constants keyed by input port id
    std::map<std::string, Vec4> inputDefaults;
};

struct ShaderGraphLink
{
    std::uint32_t fromNode = 0;
    std::string fromPort;
    std::uint32_t toNode = 0;
    std::string toPort;

    bool operator==(const ShaderGraphLink&) const = default;
};

struct ShaderGraphDocument
{
    std::string name = "NewShaderGraph";
    SurfaceShadingModel shadingModel = SurfaceShadingModel::Lit;
    MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque;
    bool doubleSided = false;
    std::uint32_t nextNodeId = 1;
    float panX = 0.0f;
    float panY = 0.0f;
    float zoom = 1.0f;
    std::vector<ShaderGraphNode> nodes;
    std::vector<ShaderGraphLink> links;
};

struct ShaderPreviewImage
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba8;
};

[[nodiscard]] const std::vector<ShaderNodeTypeSpec>& BuiltinShaderNodeSpecs();
[[nodiscard]] const ShaderNodeTypeSpec* FindShaderNodeSpec(std::string_view type);

[[nodiscard]] ShaderGraphDocument MakeDefaultShaderGraph(std::string_view name = "NewShaderGraph");

[[nodiscard]] bool SerializeShaderGraph(const ShaderGraphDocument& graph, std::string& outJson,
                                        std::string& errorMessage);
[[nodiscard]] bool DeserializeShaderGraph(std::string_view jsonText, ShaderGraphDocument& outGraph,
                                          std::string& errorMessage);
[[nodiscard]] bool SaveShaderGraph(const std::filesystem::path& path, const ShaderGraphDocument& graph,
                                   std::string& errorMessage);
[[nodiscard]] bool LoadShaderGraph(const std::filesystem::path& path, ShaderGraphDocument& outGraph,
                                   std::string& errorMessage);

// Compiles the visual node graph into clean, human-readable .fshader.hlsl Surface Shader code.
[[nodiscard]] bool CompileShaderGraphToHlsl(const ShaderGraphDocument& graph, std::string& outHlsl,
                                            std::string& errorMessage);

// Evaluates the visual node graph on an anti-aliased 3D preview sphere (supports live time, light orbit & alpha).
[[nodiscard]] ShaderPreviewImage RenderShaderGraphPreviewSphere(
    const ShaderGraphDocument& graph,
    float timeSeconds = 0.0f,
    float lightYawRadians = -0.55f,
    float lightPitchRadians = 0.52f,
    const std::map<std::string, Vec4>& materialOverrides = {},
    std::uint32_t size = 112);
} // namespace flare
