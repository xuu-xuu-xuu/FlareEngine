#include "ShaderGraph.h"
#include "AssetIO.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <set>
#include <sstream>
#include <unordered_map>

namespace flare
{
namespace
{
using Json = nlohmann::json;

bool IsReservedSgParamIdent(std::string_view s) noexcept
{
    static constexpr std::string_view kReserved[] = {
        "_Time", "_SinTime", "_CosTime", "unity_DeltaTime",
        "_MainTex", "_BumpMap", "_MetallicGlossMap",
        "Input", "SurfaceOutput", "SurfaceOutputStandard", "VertexInput", "appdata_full",
        "FlareSurface", "FlareVertex", "FlareSurfaceInput", "FlareSurfaceOutput", "FlareVertexInput",
        "tex2D", "tex2Dlod", "UnpackNormal", "UnpackScaleNormal", "sampler2D",
        "float", "float2", "float3", "float4", "half", "half2", "half3", "half4",
        "fixed", "fixed2", "fixed3", "fixed4", "int", "uint", "bool", "void",
        "in", "out", "inout", "return", "true", "false", "struct", "cbuffer",
    };
    for (std::string_view r : kReserved)
    {
        if (s == r) return true;
    }
    return false;
}

std::string SanitizeParamIdent(std::string_view raw, std::uint32_t fallbackId)
{
    std::string out;
    for (char c : raw)
    {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (c >= '0' && c <= '9'))
            out.push_back(c);
        else if (c == ' ' || c == '-')
            out.push_back('_');
    }
    if (out.empty()) out = "_Param" + std::to_string(fallbackId);
    if (out.front() >= '0' && out.front() <= '9') out.insert(out.begin(), '_');
    if (IsReservedSgParamIdent(out)) out += "_Param";
    return out;
}

std::string SanitizeShaderLabQuoted(std::string_view raw)
{
    std::string out;
    out.reserve(raw.size());
    for (char ch : raw)
    {
        if (ch == '\r' || ch == '\n') continue;
        if (ch == '"') out.push_back('\'');
        else if (ch == '\\') out.push_back('/');
        else out.push_back(ch);
    }
    return out;
}

bool HasUniformComponents(const Vec4& v, ShaderPortValueType type) noexcept
{
    switch (type)
    {
    case ShaderPortValueType::Float: return true;
    case ShaderPortValueType::Vec2: return v.x == v.y;
    case ShaderPortValueType::Vec3: return v.x == v.y && v.x == v.z;
    case ShaderPortValueType::Vec4: return v.x == v.y && v.x == v.z && v.x == v.w;
    }
    return true;
}

std::string FormatFloat(float v)
{
    if (!std::isfinite(v)) v = 0.0f;
    std::ostringstream s;
    s << std::fixed << std::setprecision(4) << v;
    std::string str = s.str();
    while (str.size() > 3 && str.back() == '0' && str[str.size() - 2] != '.') str.pop_back();
    return str;
}

std::string LiteralExpr(const Vec4& v, ShaderPortValueType type)
{
    switch (type)
    {
    case ShaderPortValueType::Float: return FormatFloat(v.x);
    case ShaderPortValueType::Vec2:
        return "float2(" + FormatFloat(v.x) + ", " + FormatFloat(v.y) + ")";
    case ShaderPortValueType::Vec3:
        return "float3(" + FormatFloat(v.x) + ", " + FormatFloat(v.y) + ", " + FormatFloat(v.z) + ")";
    case ShaderPortValueType::Vec4:
        return "float4(" + FormatFloat(v.x) + ", " + FormatFloat(v.y) + ", " + FormatFloat(v.z) + ", " + FormatFloat(v.w) + ")";
    }
    return "0.0";
}

std::string HlslTypeName(ShaderPortValueType type)
{
    switch (type)
    {
    case ShaderPortValueType::Float: return "float";
    case ShaderPortValueType::Vec2: return "float2";
    case ShaderPortValueType::Vec3: return "float3";
    case ShaderPortValueType::Vec4: return "float4";
    }
    return "float";
}

std::string CastHlslExpr(const std::string& expr, ShaderPortValueType from, ShaderPortValueType to)
{
    if (from == to) return expr;
    const std::string wrapped = "(" + expr + ")";
    if (from == ShaderPortValueType::Float)
    {
        if (to == ShaderPortValueType::Vec2) return "float2(" + wrapped + ", " + wrapped + ")";
        if (to == ShaderPortValueType::Vec3) return "float3(" + wrapped + ", " + wrapped + ", " + wrapped + ")";
        return "float4(" + wrapped + ", " + wrapped + ", " + wrapped + ", " + wrapped + ")";
    }
    if (to == ShaderPortValueType::Float) return wrapped + ".x";
    if (to == ShaderPortValueType::Vec2) return wrapped + ".xy";
    if (to == ShaderPortValueType::Vec3)
    {
        if (from == ShaderPortValueType::Vec2) return "float3(" + wrapped + ", 0.0)";
        return wrapped + ".xyz";
    }
    // to == Vec4
    if (from == ShaderPortValueType::Vec2) return "float4(" + wrapped + ", 0.0, 1.0)";
    return "float4(" + wrapped + ", 1.0)";
}

ShaderPortValueType WiderType(ShaderPortValueType a, ShaderPortValueType b)
{
    return static_cast<std::uint8_t>(a) >= static_cast<std::uint8_t>(b) ? a : b;
}

Vec4 CastCpuValue(const Vec4& v, ShaderPortValueType from, ShaderPortValueType to)
{
    if (from == to) return v;
    if (from == ShaderPortValueType::Float) return {v.x, v.x, v.x, v.x};
    if (to == ShaderPortValueType::Float) return {v.x, 0.0f, 0.0f, 0.0f};
    if (to == ShaderPortValueType::Vec2) return {v.x, v.y, 0.0f, 0.0f};
    if (to == ShaderPortValueType::Vec3)
    {
        return from == ShaderPortValueType::Vec2 ? Vec4{v.x, v.y, 0.0f, 0.0f} : Vec4{v.x, v.y, v.z, 0.0f};
    }
    if (from == ShaderPortValueType::Vec2) return {v.x, v.y, 0.0f, 1.0f};
    return {v.x, v.y, v.z, 1.0f};
}

float Hash21(float x, float y)
{
    const float h = std::sin(x * 127.1f + y * 311.7f) * 43758.5453f;
    return h - std::floor(h);
}

void GradHash22(float x, float y, float& gx, float& gy)
{
    const float a = Hash21(x, y) * 6.2831853f;
    gx = std::cos(a);
    gy = std::sin(a);
}

float ValueNoise2D(float x, float y)
{
    const float ix = std::floor(x), iy = std::floor(y);
    const float fx = x - ix, fy = y - iy;
    const float ux = fx * fx * (3.0f - 2.0f * fx);
    const float uy = fy * fy * (3.0f - 2.0f * fy);
    const float a = Hash21(ix, iy);
    const float b = Hash21(ix + 1.0f, iy);
    const float c = Hash21(ix, iy + 1.0f);
    const float d = Hash21(ix + 1.0f, iy + 1.0f);
    return (a + (b - a) * ux) + ((c - a) + (a - b - c + d) * ux) * uy;
}

float GradientNoise2D(float x, float y)
{
    const float ix = std::floor(x), iy = std::floor(y);
    const float fx = x - ix, fy = y - iy;
    const float ux = fx * fx * fx * (fx * (fx * 6.0f - 15.0f) + 10.0f);
    const float uy = fy * fy * fy * (fy * (fy * 6.0f - 15.0f) + 10.0f);
    float g00x, g00y, g10x, g10y, g01x, g01y, g11x, g11y;
    GradHash22(ix, iy, g00x, g00y);
    GradHash22(ix + 1.0f, iy, g10x, g10y);
    GradHash22(ix, iy + 1.0f, g01x, g01y);
    GradHash22(ix + 1.0f, iy + 1.0f, g11x, g11y);
    const float n00 = g00x * fx + g00y * fy;
    const float n10 = g10x * (fx - 1.0f) + g10y * fy;
    const float n01 = g01x * fx + g01y * (fy - 1.0f);
    const float n11 = g11x * (fx - 1.0f) + g11y * (fy - 1.0f);
    const float nx0 = n00 + (n10 - n00) * ux;
    const float nx1 = n01 + (n11 - n01) * ux;
    return std::clamp((nx0 + (nx1 - nx0) * uy) * 0.7071f + 0.5f, 0.0f, 1.0f);
}

float FbmNoise2D(float x, float y, float roughness)
{
    const float gain = std::clamp(roughness, 0.1f, 0.9f);
    float sum = 0.0f, amp = 0.5f, norm = 0.0f, freq = 1.0f;
    for (int oct = 0; oct < 4; ++oct)
    {
        sum += amp * ValueNoise2D(x * freq + float(oct) * 13.7f, y * freq + float(oct) * 29.3f);
        norm += amp;
        amp *= gain;
        freq *= 2.02f;
    }
    return norm > 1e-5f ? std::clamp(sum / norm, 0.0f, 1.0f) : 0.0f;
}

void Voronoi2DEx(float x, float y, float& outDist, float& outId)
{
    const float nx = std::floor(x), ny = std::floor(y);
    const float fx = x - nx, fy = y - ny;
    float minDist = 8.0f;
    float bestId = 0.0f;
    for (int j = -1; j <= 1; ++j)
    {
        for (int i = -1; i <= 1; ++i)
        {
            const float gx = static_cast<float>(i), gy = static_cast<float>(j);
            const float ox = Hash21(nx + gx, ny + gy);
            const float oy = Hash21(nx + gx + 17.0f, ny + gy + 31.0f);
            const float rx = gx + ox - fx;
            const float ry = gy + oy - fy;
            const float d = std::sqrt(rx * rx + ry * ry);
            if (d < minDist)
            {
                minDist = d;
                bestId = Hash21(nx + gx + 53.0f, ny + gy + 97.0f);
            }
        }
    }
    outDist = std::clamp(minDist, 0.0f, 1.0f);
    outId = bestId;
}

Vec3 RgbToHsvCpu(const Vec3& c)
{
    const float r = std::max(0.0f, c.x), g = std::max(0.0f, c.y), b = std::max(0.0f, c.z);
    const float cmax = std::max({r, g, b});
    const float cmin = std::min({r, g, b});
    const float d = cmax - cmin;
    float h = 0.0f;
    if (d > 1e-6f)
    {
        if (cmax == r) h = (g - b) / d + (g < b ? 6.0f : 0.0f);
        else if (cmax == g) h = (b - r) / d + 2.0f;
        else h = (r - g) / d + 4.0f;
        h /= 6.0f;
    }
    const float s = cmax > 1e-6f ? (d / cmax) : 0.0f;
    return {h - std::floor(h), s, cmax};
}

Vec3 HsvToRgbCpu(float h, float s, float v)
{
    h = h - std::floor(h);
    s = std::clamp(s, 0.0f, 1.0f);
    const float rx = std::clamp(std::abs(h * 6.0f - 3.0f) - 1.0f, 0.0f, 1.0f);
    const float gx = std::clamp(2.0f - std::abs(h * 6.0f - 2.0f), 0.0f, 1.0f);
    const float bx = std::clamp(2.0f - std::abs(h * 6.0f - 4.0f), 0.0f, 1.0f);
    return {v * (1.0f + (rx - 1.0f) * s), v * (1.0f + (gx - 1.0f) * s), v * (1.0f + (bx - 1.0f) * s)};
}

Vec3 RotateAboutAxisCpu(const Vec3& v, const Vec3& axisRaw, float angle)
{
    const Vec3 k = Normalize(axisRaw);
    const float c = std::cos(angle), s = std::sin(angle);
    return v * c + Cross(k, v) * s + k * (Dot(k, v) * (1.0f - c));
}

float PolygonMaskCpu(float ux, float uy, float sides, float w, float h)
{
    const float px = (ux * 2.0f - 1.0f) / std::max(w, 1e-4f);
    const float py = (uy * 2.0f - 1.0f) / std::max(h, 1e-4f);
    const float n = std::max(3.0f, std::floor(sides));
    const float a = std::atan2(px, std::abs(px) < 1e-6f && std::abs(py) < 1e-6f ? 1e-6f : py) + 3.14159265f;
    const float r = 6.2831853f / n;
    const float d = std::cos(std::floor(0.5f + a / r) * r - a) * std::hypot(px, py);
    return 1.0f - std::clamp((d - 0.98f) / 0.04f, 0.0f, 1.0f);
}

std::uint8_t ToSrgbByte(float linear)
{
    const float clamped = std::clamp(linear, 0.0f, 1.0f);
    const float srgb = clamped <= 0.0031308f ? clamped * 12.92f : 1.055f * std::pow(clamped, 1.0f / 2.4f) - 0.055f;
    return static_cast<std::uint8_t>(std::clamp(srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
}
} // namespace

const std::vector<ShaderNodeTypeSpec>& BuiltinShaderNodeSpecs()
{
    using T = ShaderPortValueType;
    static const std::vector<ShaderNodeTypeSpec> specs = {
        // ==================== 0. Master Output ====================
        {"MasterSurface", "Master", "主表面输出 (Master)",
         {{"BaseColor", "基础色 (RGB)", T::Vec3, {0.82f, 0.84f, 0.88f, 1.0f}},
          {"Metallic", "金属度", T::Float, {0.08f, 0, 0, 0}},
          {"Roughness", "粗糙度", T::Float, {0.38f, 0, 0, 0}},
          {"NormalTS", "切线法线", T::Vec3, {0.0f, 0.0f, 1.0f, 0.0f}},
          {"Emissive", "自发光 (HDR)", T::Vec3, {0.0f, 0.0f, 0.0f, 0.0f}},
          {"Opacity", "不透明度", T::Float, {1.0f, 0, 0, 0}},
          {"AlphaCutoff", "裁切阈值", T::Float, {0.5f, 0, 0, 0}},
          {"WorldOffset", "顶点局部偏移", T::Vec3, {0.0f, 0.0f, 0.0f, 0.0f}}},
         {}, true, false},

        // ==================== 1. Input (16) ====================
        {"UV", "Input", "纹理坐标 (UV)", {}, {{"Out", "UV", T::Vec2}, {"U", "U", T::Float}, {"V", "V", T::Float}}},
        {"Time", "Input", "时间 (Time)", {},
         {{"Time", "Time (t)", T::Float}, {"SinTime", "Sin(t)", T::Float}, {"CosTime", "Cos(t)", T::Float}, {"Delta", "dt", T::Float}}},
        {"WorldPos", "Input", "世界坐标 (World Pos)", {},
         {{"Out", "XYZ", T::Vec3}, {"X", "X", T::Float}, {"Y", "Y (高度)", T::Float}, {"Z", "Z", T::Float}}},
        {"LocalPos", "Input", "局部坐标 (Local Pos)", {},
         {{"Out", "XYZ", T::Vec3}, {"X", "X", T::Float}, {"Y", "Y", T::Float}, {"Z", "Z", T::Float}}},
        {"ObjectPos", "Input", "物体枢轴坐标 (Object Pos)", {}, {{"Out", "XYZ", T::Vec3}}},
        {"CameraPos", "Input", "相机坐标 (Camera Pos)", {}, {{"Out", "XYZ", T::Vec3}}},
        {"CameraDistance", "Input", "相机距离 (Camera Dist)", {}, {{"Out", "Dist", T::Float}}},
        {"WorldNormal", "Input", "世界法线 (World Normal)", {}, {{"Out", "Normal", T::Vec3}}},
        {"WorldTangent", "Input", "世界切线 (World Tangent)", {}, {{"Out", "Tangent", T::Vec3}}},
        {"WorldBitangent", "Input", "世界副切线 (Bitangent)", {}, {{"Out", "Bitangent", T::Vec3}}},
        {"ViewDir", "Input", "视线方向 (View Dir)", {}, {{"Out", "View", T::Vec3}}},
        {"LightDir", "Input", "主光方向 (Light Dir)", {}, {{"Out", "Light", T::Vec3}}},
        {"LightColor", "Input", "主光颜色 (Light Color)", {}, {{"RGB", "RGB", T::Vec3}, {"Intensity", "Intensity", T::Float}}},
        {"ScreenUV", "Input", "屏幕坐标 (Screen UV)", {}, {{"Out", "XY", T::Vec2}}},
        {"Facing", "Input", "正背面符号 (Face Sign)", {}, {{"Out", "+1/-1", T::Float}, {"Front", "IsFront", T::Float}}},
        {"VertexColor", "Input", "顶点色 (Vertex Color)", {},
         {{"RGBA", "RGBA", T::Vec4}, {"RGB", "RGB", T::Vec3}, {"A", "Alpha", T::Float}}},

        // ==================== 2. Property & Constants (9) ====================
        {"Float", "Property", "数值 (Float)", {}, {{"Out", "Value", T::Float}}, false, true},
        {"Range", "Property", "滑条范围 (Range)", {}, {{"Out", "Value", T::Float}}, false, true},
        {"Boolean", "Property", "布尔开关 (Boolean)", {}, {{"Out", "0 / 1", T::Float}}, false, true},
        {"Color", "Property", "颜色 (Color)", {},
         {{"RGBA", "RGBA", T::Vec4}, {"RGB", "RGB", T::Vec3}, {"A", "Alpha", T::Float}}, false, true},
        {"Vector2", "Property", "二维向量 (Vector2)", {}, {{"Out", "XY", T::Vec2}}, false, true},
        {"Vector3", "Property", "三维向量 (Vector3)", {}, {{"Out", "XYZ", T::Vec3}}, false, true},
        {"Vector4", "Property", "四维向量 (Vector4)", {}, {{"Out", "XYZW", T::Vec4}}, false, true},
        {"PI", "Property", "圆周率常量 (PI / TAU)", {},
         {{"PI", "PI (3.14)", T::Float}, {"TAU", "TAU (6.28)", T::Float}, {"HalfPI", "PI/2", T::Float}, {"InvPI", "1/PI", T::Float}}},
        {"GoldenRatio", "Property", "自然与黄金常量 (E / Phi)", {},
         {{"Phi", "Phi (1.618)", T::Float}, {"E", "E (2.718)", T::Float}, {"Sqrt2", "Sqrt(2)", T::Float}}},

        // ==================== 3. Texture & Normal (9) ====================
        {"BaseColorMap", "Texture", "采样基础色贴图", {{"UV", "UV", T::Vec2, {0, 0, 0, 0}}},
         {{"RGB", "RGB", T::Vec3}, {"A", "Alpha", T::Float}, {"RGBA", "RGBA", T::Vec4}}},
        {"NormalMap", "Texture", "采样法线贴图", {{"UV", "UV", T::Vec2, {0, 0, 0, 0}}}, {{"NormalTS", "NormalTS", T::Vec3}}},
        {"MetallicRoughnessMap", "Texture", "采样金属粗糙贴图", {{"UV", "UV", T::Vec2, {0, 0, 0, 0}}},
         {{"Metallic", "Metallic", T::Float}, {"Roughness", "Roughness", T::Float}}},
        {"NormalStrength", "Texture", "法线强度 (Normal Strength)",
         {{"In", "NormalTS", T::Vec3, {0, 0, 1, 0}}, {"Strength", "Strength", T::Float, {1.0f, 0, 0, 0}}},
         {{"Out", "NormalTS", T::Vec3}}},
        {"NormalBlend", "Texture", "细节法线混合 (Normal Blend)",
         {{"A", "BaseTS", T::Vec3, {0, 0, 1, 0}}, {"B", "DetailTS", T::Vec3, {0, 0, 1, 0}}},
         {{"Out", "NormalTS", T::Vec3}}},
        {"NormalFromHeight", "Texture", "高度转法线 (Height To Normal)",
         {{"In", "Height", T::Float, {0.5f, 0, 0, 0}}, {"Strength", "Strength", T::Float, {1.5f, 0, 0, 0}}},
         {{"Out", "NormalTS", T::Vec3}}},
        {"TriplanarBaseColor", "Texture", "三平面无缝采样 (Triplanar)",
         {{"Scale", "Scale", T::Float, {1.0f, 0, 0, 0}}, {"Sharpness", "Sharpness", T::Float, {4.0f, 0, 0, 0}}},
         {{"RGB", "RGB", T::Vec3}, {"A", "Alpha", T::Float}}},
        {"ParallaxOffset", "Texture", "视差偏移 UV (Bump Offset)",
         {{"UV", "UV", T::Vec2, {0, 0, 0, 0}}, {"Height", "Height", T::Float, {0.5f, 0, 0, 0}}, {"Amplitude", "Depth", T::Float, {0.04f, 0, 0, 0}}},
         {{"Out", "UV", T::Vec2}}},
        {"Flipbook", "Texture", "序列帧动画 UV (Flipbook)",
         {{"UV", "UV", T::Vec2, {0, 0, 0, 0}}, {"Columns", "Cols", T::Float, {4.0f, 0, 0, 0}},
          {"Rows", "Rows", T::Float, {4.0f, 0, 0, 0}}, {"Frame", "Frame", T::Float, {0.0f, 0, 0, 0}}},
         {{"Out", "UV", T::Vec2}}},

        // ==================== 4. UV Coordinates (10) ====================
        {"TilingOffset", "UV", "UV 缩放偏移 (Tiling & Offset)",
         {{"UV", "UV", T::Vec2, {0, 0, 0, 0}}, {"Tiling", "Tiling", T::Vec2, {1, 1, 0, 0}}, {"Offset", "Offset", T::Vec2, {0, 0, 0, 0}}},
         {{"Out", "UV", T::Vec2}}},
        {"Panner", "UV", "UV 流动 (Panner)",
         {{"UV", "UV", T::Vec2, {0, 0, 0, 0}}, {"Speed", "Speed", T::Vec2, {0.2f, 0.0f, 0, 0}}, {"Time", "Time", T::Float, {-1.0f, 0, 0, 0}}},
         {{"Out", "UV", T::Vec2}}},
        {"Rotator", "UV", "UV 旋转 (Rotate UV)",
         {{"UV", "UV", T::Vec2, {0, 0, 0, 0}}, {"Center", "Center", T::Vec2, {0.5f, 0.5f, 0, 0}}, {"Angle", "Rad", T::Float, {0.0f, 0, 0, 0}}},
         {{"Out", "UV", T::Vec2}}},
        {"PolarCoordinates", "UV", "极坐标变换 (Polar Coords)",
         {{"UV", "UV", T::Vec2, {0, 0, 0, 0}}, {"Center", "Center", T::Vec2, {0.5f, 0.5f, 0, 0}},
          {"RadialScale", "Radial", T::Float, {1.0f, 0, 0, 0}}, {"LengthScale", "Angle", T::Float, {1.0f, 0, 0, 0}}},
         {{"Out", "UV", T::Vec2}, {"Radius", "R", T::Float}, {"Angle", "Phi", T::Float}}},
        {"Twirl", "UV", "漩涡扭曲 (Twirl)",
         {{"UV", "UV", T::Vec2, {0, 0, 0, 0}}, {"Center", "Center", T::Vec2, {0.5f, 0.5f, 0, 0}},
          {"Strength", "Strength", T::Float, {5.0f, 0, 0, 0}}, {"Offset", "Offset", T::Vec2, {0, 0, 0, 0}}},
         {{"Out", "UV", T::Vec2}}},
        {"RadialShear", "UV", "径向切变 (Radial Shear)",
         {{"UV", "UV", T::Vec2, {0, 0, 0, 0}}, {"Center", "Center", T::Vec2, {0.5f, 0.5f, 0, 0}},
          {"Strength", "Strength", T::Vec2, {1.0f, 1.0f, 0, 0}}, {"Offset", "Offset", T::Vec2, {0, 0, 0, 0}}},
         {{"Out", "UV", T::Vec2}}},
        {"Spherize", "UV", "球面凸镜变形 (Spherize)",
         {{"UV", "UV", T::Vec2, {0, 0, 0, 0}}, {"Center", "Center", T::Vec2, {0.5f, 0.5f, 0, 0}},
          {"Strength", "Strength", T::Float, {2.0f, 0, 0, 0}}, {"Offset", "Offset", T::Vec2, {0, 0, 0, 0}}},
         {{"Out", "UV", T::Vec2}}},
        {"PixelateUV", "UV", "像素马赛克 UV (Pixelate)",
         {{"UV", "UV", T::Vec2, {0, 0, 0, 0}}, {"Cells", "Cells", T::Vec2, {32.0f, 32.0f, 0, 0}}},
         {{"Out", "UV", T::Vec2}}},
        {"MirrorUV", "UV", "镜像折返 UV (Mirror UV)",
         {{"UV", "UV", T::Vec2, {0, 0, 0, 0}}},
         {{"Out", "UV", T::Vec2}}},
        {"WorldAlignedUV", "UV", "世界平面投射 UV (World UV)",
         {{"Scale", "Scale", T::Float, {1.0f, 0, 0, 0}}},
         {{"XZ", "Top XZ", T::Vec2}, {"XY", "Front XY", T::Vec2}, {"YZ", "Side YZ", T::Vec2}}},

        // ==================== 5. Math (41) ====================
        {"Add", "Math", "相加 (Add)", {{"A", "A", T::Vec3, {0, 0, 0, 0}}, {"B", "B", T::Vec3, {0, 0, 0, 0}}}, {{"Out", "A + B", T::Vec3}}},
        {"Subtract", "Math", "相减 (Subtract)", {{"A", "A", T::Vec3, {1, 1, 1, 0}}, {"B", "B", T::Vec3, {0, 0, 0, 0}}}, {{"Out", "A - B", T::Vec3}}},
        {"Multiply", "Math", "相乘 (Multiply)", {{"A", "A", T::Vec3, {1, 1, 1, 1}}, {"B", "B", T::Vec3, {1, 1, 1, 1}}}, {{"Out", "A × B", T::Vec3}}},
        {"Divide", "Math", "相除 (Divide)", {{"A", "A", T::Vec3, {1, 1, 1, 1}}, {"B", "B", T::Vec3, {1, 1, 1, 1}}}, {{"Out", "A ÷ B", T::Vec3}}},
        {"MultiplyAdd", "Math", "乘加融合 (Multiply Add)",
         {{"A", "A", T::Vec3, {1, 1, 1, 0}}, {"B", "B", T::Vec3, {1, 1, 1, 0}}, {"C", "C", T::Vec3, {0, 0, 0, 0}}},
         {{"Out", "A×B+C", T::Vec3}}},
        {"OneMinus", "Math", "反向 (One Minus)", {{"In", "In", T::Vec3, {0, 0, 0, 0}}}, {{"Out", "1 - In", T::Vec3}}},
        {"Negate", "Math", "取负 (Negate)", {{"In", "In", T::Vec3, {0, 0, 0, 0}}}, {{"Out", "-In", T::Vec3}}},
        {"Reciprocal", "Math", "倒数 (Reciprocal)", {{"In", "In", T::Vec3, {1, 1, 1, 1}}}, {{"Out", "1 / In", T::Vec3}}},
        {"Abs", "Math", "绝对值 (Abs)", {{"In", "In", T::Vec3, {0, 0, 0, 0}}}, {{"Out", "|In|", T::Vec3}}},
        {"Sign", "Math", "正负符号 (Sign)", {{"In", "In", T::Vec3, {1, 1, 1, 0}}}, {{"Out", "Sign", T::Vec3}}},
        {"Saturate", "Math", "夹取 0..1 (Saturate)", {{"In", "In", T::Vec3, {0.5f, 0.5f, 0.5f, 0}}}, {{"Out", "Out", T::Vec3}}},
        {"Clamp", "Math", "区间夹取 (Clamp)",
         {{"In", "In", T::Vec3, {0.5f, 0.5f, 0.5f, 0}}, {"Min", "Min", T::Vec3, {0, 0, 0, 0}}, {"Max", "Max", T::Vec3, {1, 1, 1, 1}}},
         {{"Out", "Out", T::Vec3}}},
        {"Min", "Math", "最小值 (Min)", {{"A", "A", T::Vec3, {0, 0, 0, 0}}, {"B", "B", T::Vec3, {1, 1, 1, 1}}}, {{"Out", "Min", T::Vec3}}},
        {"Max", "Math", "最大值 (Max)", {{"A", "A", T::Vec3, {0, 0, 0, 0}}, {"B", "B", T::Vec3, {1, 1, 1, 1}}}, {{"Out", "Max", T::Vec3}}},
        {"Lerp", "Math", "线性混合 (Lerp)",
         {{"A", "A", T::Vec3, {0, 0, 0, 0}}, {"B", "B", T::Vec3, {1, 1, 1, 1}}, {"T", "Alpha", T::Float, {0.5f, 0, 0, 0}}},
         {{"Out", "Out", T::Vec3}}},
        {"InverseLerp", "Math", "反向插值 (Inverse Lerp)",
         {{"A", "A", T::Float, {0.0f, 0, 0, 0}}, {"B", "B", T::Float, {1.0f, 0, 0, 0}}, {"T", "In", T::Float, {0.5f, 0, 0, 0}}},
         {{"Out", "0..1", T::Float}}},
        {"Remap", "Math", "区间重映射 (Remap)",
         {{"In", "In", T::Float, {0.0f, 0, 0, 0}}, {"InMin", "InMin", T::Float, {-1.0f, 0, 0, 0}},
          {"InMax", "InMax", T::Float, {1.0f, 0, 0, 0}}, {"OutMin", "OutMin", T::Float, {0.0f, 0, 0, 0}}, {"OutMax", "OutMax", T::Float, {1.0f, 0, 0, 0}}},
         {{"Out", "Out", T::Float}}},
        {"Smoothstep", "Math", "平滑阶跃 (Smoothstep)",
         {{"Edge0", "Min", T::Float, {0.2f, 0, 0, 0}}, {"Edge1", "Max", T::Float, {0.8f, 0, 0, 0}}, {"In", "In", T::Float, {0.5f, 0, 0, 0}}},
         {{"Out", "Out", T::Float}}},
        {"Step", "Math", "硬阶跃 (Step)",
         {{"Edge", "Edge", T::Float, {0.5f, 0, 0, 0}}, {"In", "In", T::Float, {0.5f, 0, 0, 0}}},
         {{"Out", "Out", T::Float}}},
        {"Floor", "Math", "向下取整 (Floor)", {{"In", "In", T::Vec3, {0, 0, 0, 0}}}, {{"Out", "Floor", T::Vec3}}},
        {"Ceil", "Math", "向上取整 (Ceil)", {{"In", "In", T::Vec3, {0, 0, 0, 0}}}, {{"Out", "Ceil", T::Vec3}}},
        {"Round", "Math", "四舍五入 (Round)", {{"In", "In", T::Vec3, {0, 0, 0, 0}}}, {{"Out", "Round", T::Vec3}}},
        {"Frac", "Math", "小数部分 (Frac)", {{"In", "In", T::Vec3, {0, 0, 0, 0}}}, {{"Out", "Frac", T::Vec3}}},
        {"Fmod", "Math", "取模余数 (Modulo)",
         {{"A", "A", T::Vec3, {0, 0, 0, 0}}, {"B", "Mod", T::Vec3, {1, 1, 1, 1}}},
         {{"Out", "Out", T::Vec3}}},
        {"PingPong", "Math", "往返折返 (PingPong)",
         {{"In", "In", T::Float, {0, 0, 0, 0}}, {"Length", "Length", T::Float, {1.0f, 0, 0, 0}}},
         {{"Out", "Out", T::Float}}},
        {"Power", "Math", "幂运算 (Power)",
         {{"Base", "Base", T::Float, {0.5f, 0, 0, 0}}, {"Exp", "Exp", T::Float, {3.0f, 0, 0, 0}}},
         {{"Out", "Out", T::Float}}},
        {"Sqrt", "Math", "平方根 (Sqrt)", {{"In", "In", T::Vec3, {1, 1, 1, 0}}}, {{"Out", "Sqrt", T::Vec3}}},
        {"Rsqrt", "Math", "平方根倒数 (Inv Sqrt)", {{"In", "In", T::Vec3, {1, 1, 1, 0}}}, {{"Out", "Rsqrt", T::Vec3}}},
        {"Exp", "Math", "指数运算 (Exp / Exp2)", {{"In", "In", T::Float, {0, 0, 0, 0}}},
         {{"Out", "e^x", T::Float}, {"Exp2", "2^x", T::Float}}},
        {"Log", "Math", "对数运算 (Ln / Log2)", {{"In", "In", T::Float, {1.0f, 0, 0, 0}}},
         {{"Out", "ln(x)", T::Float}, {"Log2", "log2(x)", T::Float}}},
        {"Sin", "Math", "正弦 (Sin)", {{"In", "Rad", T::Float, {0, 0, 0, 0}}}, {{"Out", "Sin", T::Float}}},
        {"Cos", "Math", "余弦 (Cos)", {{"In", "Rad", T::Float, {0, 0, 0, 0}}}, {{"Out", "Cos", T::Float}}},
        {"Tan", "Math", "正切 (Tan)", {{"In", "Rad", T::Float, {0, 0, 0, 0}}}, {{"Out", "Tan", T::Float}}},
        {"Asin", "Math", "反正弦 (Asin)", {{"In", "-1..1", T::Float, {0, 0, 0, 0}}}, {{"Out", "Rad", T::Float}}},
        {"Acos", "Math", "反余弦 (Acos)", {{"In", "-1..1", T::Float, {0, 0, 0, 0}}}, {{"Out", "Rad", T::Float}}},
        {"Atan2", "Math", "二元反正切 (Atan2)",
         {{"Y", "Y", T::Float, {0, 0, 0, 0}}, {"X", "X", T::Float, {1, 0, 0, 0}}},
         {{"Out", "Rad", T::Float}}},
        {"Radians", "Math", "角度转弧度 (Degrees->Rad)", {{"In", "Deg", T::Float, {180.0f, 0, 0, 0}}}, {{"Out", "Rad", T::Float}}},
        {"Degrees", "Math", "弧度转角度 (Rad->Degrees)", {{"In", "Rad", T::Float, {3.14159f, 0, 0, 0}}}, {{"Out", "Deg", T::Float}}},
        {"TriangleWave", "Math", "三角波 (Triangle Wave)", {{"In", "Phase", T::Float, {0, 0, 0, 0}}}, {{"Out", "0..1", T::Float}}},
        {"SquareWave", "Math", "方波 (Square Wave)",
         {{"In", "Phase", T::Float, {0, 0, 0, 0}}, {"Duty", "Duty", T::Float, {0.5f, 0, 0, 0}}},
         {{"Out", "0 / 1", T::Float}}},
        {"SawtoothWave", "Math", "锯齿波 (Sawtooth Wave)", {{"In", "Phase", T::Float, {0, 0, 0, 0}}}, {{"Out", "-1..1", T::Float}}},

        // ==================== 6. Vector & Channel (14) ====================
        {"Dot", "Vector", "点积 (Dot Product)",
         {{"A", "A", T::Vec3, {0, 1, 0, 0}}, {"B", "B", T::Vec3, {0, 1, 0, 0}}},
         {{"Out", "A·B", T::Float}}},
        {"Cross", "Vector", "叉积 (Cross Product)",
         {{"A", "A", T::Vec3, {1, 0, 0, 0}}, {"B", "B", T::Vec3, {0, 1, 0, 0}}},
         {{"Out", "A×B", T::Vec3}}},
        {"Normalize", "Vector", "归一化 (Normalize)", {{"In", "Vec", T::Vec3, {0, 0, 1, 0}}}, {{"Out", "Unit", T::Vec3}}},
        {"Length", "Vector", "向量模长 (Length)", {{"In", "Vec", T::Vec3, {1, 0, 0, 0}}}, {{"Out", "Len", T::Float}}},
        {"Distance", "Vector", "两点距离 (Distance)",
         {{"A", "A", T::Vec3, {0, 0, 0, 0}}, {"B", "B", T::Vec3, {0, 0, 0, 0}}},
         {{"Out", "Dist", T::Float}}},
        {"Reflect", "Vector", "反射向量 (Reflect)",
         {{"In", "Incident", T::Vec3, {0, -1, 0, 0}}, {"NormalWS", "Normal", T::Vec3, {0, 1, 0, 0}}},
         {{"Out", "Reflect", T::Vec3}}},
        {"Project", "Vector", "向量投影 (Project)",
         {{"A", "Vec", T::Vec3, {1, 1, 0, 0}}, {"B", "Onto", T::Vec3, {0, 1, 0, 0}}},
         {{"Out", "Proj", T::Vec3}}},
        {"Reject", "Vector", "正交平面分量 (Reject)",
         {{"A", "Vec", T::Vec3, {1, 1, 0, 0}}, {"B", "Normal", T::Vec3, {0, 1, 0, 0}}},
         {{"Out", "Planar", T::Vec3}}},
        {"RotateAboutAxis", "Vector", "绕轴旋转 (Rotate Axis)",
         {{"In", "Vec", T::Vec3, {1, 0, 0, 0}}, {"Axis", "Axis", T::Vec3, {0, 1, 0, 0}}, {"Angle", "Rad", T::Float, {0.0f, 0, 0, 0}}},
         {{"Out", "Rotated", T::Vec3}}},
        {"Split", "Vector", "通道拆分 (Split)", {{"In", "RGBA", T::Vec4, {0, 0, 0, 1}}},
         {{"R", "X / R", T::Float}, {"G", "Y / G", T::Float}, {"B", "Z / B", T::Float}, {"A", "W / A", T::Float}}},
        {"Combine", "Vector", "通道合并 (Combine)",
         {{"R", "X / R", T::Float, {0, 0, 0, 0}}, {"G", "Y / G", T::Float, {0, 0, 0, 0}},
          {"B", "Z / B", T::Float, {0, 0, 0, 0}}, {"A", "W / A", T::Float, {1, 0, 0, 0}}},
         {{"XYZ", "XYZ", T::Vec3}, {"XYZW", "XYZW", T::Vec4}}},
        {"Append", "Vector", "向量追加 (Append Vector)",
         {{"A", "XY / X", T::Vec2, {0, 0, 0, 0}}, {"B", "Z / Y", T::Float, {0, 0, 0, 0}}, {"C", "W", T::Float, {1, 0, 0, 0}}},
         {{"XY", "XY (A.x,B)", T::Vec2}, {"XYZ", "XYZ (A,B)", T::Vec3}, {"XYZW", "XYZW", T::Vec4}}},
        {"ComponentMask", "Vector", "通道重组掩码 (Swizzle)", {{"In", "XYZW", T::Vec4, {0, 0, 0, 1}}},
         {{"XY", "XY", T::Vec2}, {"XZ", "XZ", T::Vec2}, {"YZ", "YZ", T::Vec2}, {"ZYX", "ZYX / BGR", T::Vec3}}},
        {"TransformTS2WS", "Vector", "切线转世界空间 (TS->WS)",
         {{"In", "VecTS", T::Vec3, {0, 0, 1, 0}}},
         {{"Out", "VecWS", T::Vec3}}},

        // ==================== 7. Color & Blend (14) ====================
        {"RGBToHSV", "Color", "RGB 转 HSV", {{"RGB", "RGB", T::Vec3, {1, 0.5f, 0.2f, 0}}},
         {{"HSV", "HSV", T::Vec3}, {"H", "Hue", T::Float}, {"S", "Sat", T::Float}, {"V", "Val", T::Float}}},
        {"HSVToRGB", "Color", "HSV 转 RGB",
         {{"H", "Hue", T::Float, {0.0f, 0, 0, 0}}, {"S", "Sat", T::Float, {0.8f, 0, 0, 0}}, {"V", "Val", T::Float, {1.0f, 0, 0, 0}}},
         {{"RGB", "RGB", T::Vec3}}},
        {"HueShift", "Color", "色相偏移 (Hue Shift)",
         {{"In", "RGB", T::Vec3, {1, 0.4f, 0.2f, 0}}, {"Shift", "Offset", T::Float, {0.2f, 0, 0, 0}}},
         {{"Out", "RGB", T::Vec3}}},
        {"Saturation", "Color", "饱和度调节 (Saturation)",
         {{"In", "RGB", T::Vec3, {0.8f, 0.5f, 0.3f, 0}}, {"Saturation", "Sat", T::Float, {1.0f, 0, 0, 0}}},
         {{"Out", "RGB", T::Vec3}}},
        {"Contrast", "Color", "对比度调节 (Contrast)",
         {{"In", "RGB", T::Vec3, {0.5f, 0.5f, 0.5f, 0}}, {"Contrast", "Contrast", T::Float, {1.2f, 0, 0, 0}}, {"Midpoint", "Mid", T::Float, {0.5f, 0, 0, 0}}},
         {{"Out", "RGB", T::Vec3}}},
        {"BrightnessGamma", "Color", "亮度与伽马 (Brightness/Gamma)",
         {{"In", "RGB", T::Vec3, {0.5f, 0.5f, 0.5f, 0}}, {"Brightness", "Gain", T::Float, {1.0f, 0, 0, 0}}, {"Gamma", "Gamma", T::Float, {1.0f, 0, 0, 0}}},
         {{"Out", "RGB", T::Vec3}}},
        {"Luminance", "Color", "感知明度灰度 (Luminance)", {{"In", "RGB", T::Vec3, {0.5f, 0.5f, 0.5f, 0}}}, {{"Out", "Gray", T::Float}}},
        {"Posterize", "Color", "色阶量化 (Posterize)",
         {{"In", "Color", T::Vec3, {0.5f, 0.5f, 0.5f, 0}}, {"Steps", "阶数", T::Float, {4.0f, 0, 0, 0}}},
         {{"Out", "Out", T::Vec3}}},
        {"ColorRamp", "Color", "双色渐变映射 (Color Ramp)",
         {{"T", "Factor", T::Float, {0.5f, 0, 0, 0}}, {"ColorA", "Dark", T::Vec3, {0.06f, 0.10f, 0.26f, 1}}, {"ColorB", "Bright", T::Vec3, {0.98f, 0.64f, 0.18f, 1}}},
         {{"Out", "RGB", T::Vec3}}},
        {"ColorMask", "Color", "颜色抠像遮罩 (Color Mask)",
         {{"In", "RGB", T::Vec3, {1, 0, 0, 0}}, {"MaskColor", "Target", T::Vec3, {1, 0, 0, 0}},
          {"Range", "Range", T::Float, {0.25f, 0, 0, 0}}, {"Fuzziness", "Soft", T::Float, {0.1f, 0, 0, 0}}},
         {{"Out", "Mask", T::Float}}},
        {"BlendScreen", "Color", "滤色混合 (Blend: Screen)",
         {{"Base", "Base", T::Vec3, {0.3f, 0.3f, 0.3f, 0}}, {"Blend", "Blend", T::Vec3, {0.5f, 0.5f, 0.5f, 0}}, {"Opacity", "Opacity", T::Float, {1.0f, 0, 0, 0}}},
         {{"Out", "RGB", T::Vec3}}},
        {"BlendOverlay", "Color", "叠加混合 (Blend: Overlay)",
         {{"Base", "Base", T::Vec3, {0.5f, 0.5f, 0.5f, 0}}, {"Blend", "Blend", T::Vec3, {0.5f, 0.5f, 0.5f, 0}}, {"Opacity", "Opacity", T::Float, {1.0f, 0, 0, 0}}},
         {{"Out", "RGB", T::Vec3}}},
        {"BlendSoftLight", "Color", "柔光混合 (Blend: SoftLight)",
         {{"Base", "Base", T::Vec3, {0.5f, 0.5f, 0.5f, 0}}, {"Blend", "Blend", T::Vec3, {0.5f, 0.5f, 0.5f, 0}}, {"Opacity", "Opacity", T::Float, {1.0f, 0, 0, 0}}},
         {{"Out", "RGB", T::Vec3}}},
        {"HDRBoost", "Color", "HDR 发光曝光倍增 (HDR Boost)",
         {{"Color", "RGB", T::Vec3, {0.2f, 0.7f, 1.0f, 1}}, {"Intensity", "EV", T::Float, {2.0f, 0, 0, 0}}},
         {{"Out", "HDR RGB", T::Vec3}}},

        // ==================== 8. Procedural Noise & Shapes (12) ====================
        {"SimpleNoise", "Procedural", "平滑价值噪波 (Simple Noise)",
         {{"UV", "UV", T::Vec2, {0, 0, 0, 0}}, {"Scale", "Scale", T::Float, {12.0f, 0, 0, 0}}},
         {{"Out", "Noise", T::Float}}},
        {"GradientNoise", "Procedural", "柏林梯度噪波 (Perlin Noise)",
         {{"UV", "UV", T::Vec2, {0, 0, 0, 0}}, {"Scale", "Scale", T::Float, {10.0f, 0, 0, 0}}},
         {{"Out", "Noise", T::Float}}},
        {"FbmNoise", "Procedural", "分形湍流噪波 (FBM Clouds)",
         {{"UV", "UV", T::Vec2, {0, 0, 0, 0}}, {"Scale", "Scale", T::Float, {6.0f, 0, 0, 0}}, {"Roughness", "Detail", T::Float, {0.5f, 0, 0, 0}}},
         {{"Out", "FBM", T::Float}}},
        {"Voronoi", "Procedural", "细胞噪波 (Voronoi / Worley)",
         {{"UV", "UV", T::Vec2, {0, 0, 0, 0}}, {"Scale", "Scale", T::Float, {8.0f, 0, 0, 0}}},
         {{"Out", "Dist", T::Float}, {"CellId", "Cell ID", T::Float}}},
        {"Checkerboard", "Procedural", "棋盘格图案 (Checkerboard)",
         {{"UV", "UV", T::Vec2, {0, 0, 0, 0}}, {"ColorA", "Color A", T::Vec3, {0.14f, 0.15f, 0.18f, 1}},
          {"ColorB", "Color B", T::Vec3, {0.86f, 0.88f, 0.92f, 1}}, {"Frequency", "Tiles", T::Vec2, {4.0f, 4.0f, 0, 0}}},
         {{"Out", "RGB", T::Vec3}}},
        {"GridPattern", "Procedural", "科幻网格线 (Grid Lines)",
         {{"UV", "UV", T::Vec2, {0, 0, 0, 0}}, {"Tiling", "Tiling", T::Vec2, {8.0f, 8.0f, 0, 0}}, {"Thickness", "LineW", T::Float, {0.06f, 0, 0, 0}}},
         {{"Line", "Line", T::Float}, {"Cell", "Fill", T::Float}}},
        {"BrickPattern", "Procedural", "错缝砖墙图案 (Brick Tile)",
         {{"UV", "UV", T::Vec2, {0, 0, 0, 0}}, {"Tiling", "Cols/Rows", T::Vec2, {4.0f, 8.0f, 0, 0}}, {"Mortar", "Gap", T::Float, {0.06f, 0, 0, 0}}},
         {{"Brick", "Brick", T::Float}, {"BrickId", "Rand ID", T::Float}}},
        {"EllipseShape", "Procedural", "圆形/椭圆遮罩 (Ellipse)",
         {{"UV", "UV", T::Vec2, {0, 0, 0, 0}}, {"Width", "Width", T::Float, {0.7f, 0, 0, 0}},
          {"Height", "Height", T::Float, {0.7f, 0, 0, 0}}, {"Feather", "Soft", T::Float, {0.03f, 0, 0, 0}}},
         {{"Out", "Mask", T::Float}}},
        {"RectangleShape", "Procedural", "圆角矩形遮罩 (Rounded Rect)",
         {{"UV", "UV", T::Vec2, {0, 0, 0, 0}}, {"Width", "Width", T::Float, {0.7f, 0, 0, 0}},
          {"Height", "Height", T::Float, {0.7f, 0, 0, 0}}, {"Radius", "Corner", T::Float, {0.1f, 0, 0, 0}}},
         {{"Out", "Mask", T::Float}}},
        {"PolygonShape", "Procedural", "正多边形遮罩 (Polygon)",
         {{"UV", "UV", T::Vec2, {0, 0, 0, 0}}, {"Sides", "Sides", T::Float, {6.0f, 0, 0, 0}},
          {"Width", "Width", T::Float, {0.65f, 0, 0, 0}}, {"Height", "Height", T::Float, {0.65f, 0, 0, 0}}},
         {{"Out", "Mask", T::Float}}},
        {"SphereMask", "Procedural", "球形体积遮罩 (Sphere Mask)",
         {{"CoordsWS", "Pos", T::Vec3, {0, 0, 0, 0}}, {"Center", "Center", T::Vec3, {0, 0, 0, 0}},
          {"Radius", "Radius", T::Float, {0.65f, 0, 0, 0}}, {"Hardness", "Hardness", T::Float, {0.7f, 0, 0, 0}}},
         {{"Out", "Mask", T::Float}}},
        {"WaveRing", "Procedural", "径向波纹环 (Radial Wave)",
         {{"UV", "UV", T::Vec2, {0, 0, 0, 0}}, {"Center", "Center", T::Vec2, {0.5f, 0.5f, 0, 0}},
          {"Frequency", "Rings", T::Float, {6.0f, 0, 0, 0}}, {"Speed", "Speed", T::Float, {2.0f, 0, 0, 0}}},
         {{"Out", "Wave", T::Float}}},

        // ==================== 9. Artistic, Stylized & Logic (12) ====================
        {"Fresnel", "Artistic", "菲涅尔边缘光 (Fresnel)",
         {{"Power", "锐度 (Power)", T::Float, {3.0f, 0, 0, 0}}},
         {{"Out", "Rim", T::Float}}},
        {"RimLight", "Artistic", "受光边缘光 (Directional Rim)",
         {{"Color", "Color", T::Vec3, {0.25f, 0.78f, 1.0f, 1}}, {"Power", "Power", T::Float, {3.2f, 0, 0, 0}},
          {"Threshold", "LightMask", T::Float, {0.25f, 0, 0, 0}}, {"Softness", "Soft", T::Float, {0.2f, 0, 0, 0}}},
         {{"RGB", "RGB", T::Vec3}, {"Mask", "Mask", T::Float}}},
        {"HalfLambert", "Artistic", "半兰伯特漫反射 (Half-Lambert)",
         {{"NormalWS", "Normal", T::Vec3, {0, 1, 0, 0}}, {"Wrap", "Wrap", T::Float, {0.5f, 0, 0, 0}}},
         {{"HalfLambert", "Half", T::Float}, {"NdotL", "N·L", T::Float}}},
        {"CelShadeBand", "Artistic", "赛璐璐二分阴影 (Cel Band)",
         {{"In", "N·L / In", T::Float, {0.5f, 0, 0, 0}}, {"Threshold", "ShadowCut", T::Float, {0.48f, 0, 0, 0}},
          {"Smoothness", "Feather", T::Float, {0.03f, 0, 0, 0}}},
         {{"Out", "Band", T::Float}}},
        {"SpecularStylized", "Artistic", "风格化高光点 (Toon Specular)",
         {{"NormalWS", "Normal", T::Vec3, {0, 1, 0, 0}}, {"Gloss", "Size", T::Float, {0.72f, 0, 0, 0}},
          {"Hardness", "Crisp", T::Float, {0.85f, 0, 0, 0}}},
         {{"Out", "Spec", T::Float}}},
        {"MatCapUV", "Artistic", "球面环境捕捉 UV (MatCap)",
         {{"NormalWS", "Normal", T::Vec3, {0, 0, 1, 0}}},
         {{"UV", "MatCap UV", T::Vec2}}},
        {"DissolveEdge", "Artistic", "消融灼烧边缘 (Dissolve Edge)",
         {{"Noise", "Noise", T::Float, {0.5f, 0, 0, 0}}, {"Progress", "Cut", T::Float, {0.35f, 0, 0, 0}},
          {"EdgeWidth", "EdgeW", T::Float, {0.06f, 0, 0, 0}}, {"EdgeColor", "GlowCol", T::Vec3, {1.0f, 0.36f, 0.06f, 1}}},
         {{"Alpha", "Alpha", T::Float}, {"EdgeMask", "Edge", T::Float}, {"Emissive", "GlowRGB", T::Vec3}}},
        {"DitherThreshold", "Artistic", "抖动半透明裁切 (Bayer Dither)",
         {{"Opacity", "Alpha", T::Float, {0.5f, 0, 0, 0}}},
         {{"ClipAlpha", "MaskAlpha", T::Float}, {"Bayer", "Pattern", T::Float}}},
        {"HeightFogMask", "Artistic", "世界高度渐变遮罩 (Height Gradient)",
         {{"HeightWS", "Y", T::Float, {0.0f, 0, 0, 0}}, {"BaseY", "BaseY", T::Float, {-0.3f, 0, 0, 0}},
          {"Falloff", "Range", T::Float, {0.8f, 0, 0, 0}}},
         {{"Out", "0..1", T::Float}}},
        {"VertexWindWave", "Artistic", "顶点风吹摇摆 (Vertex Wind)",
         {{"Amplitude", "AmpXYZ", T::Vec3, {0.08f, 0.02f, 0.04f, 0}}, {"Frequency", "Freq", T::Float, {2.5f, 0, 0, 0}},
          {"Speed", "Speed", T::Float, {2.2f, 0, 0, 0}}},
         {{"Offset", "WorldOffset", T::Vec3}}},
        {"CompareIf", "Artistic", "条件比较分支 (Compare If)",
         {{"A", "A", T::Float, {0.5f, 0, 0, 0}}, {"B", "B", T::Float, {0.5f, 0, 0, 0}},
          {"Greater", "A > B", T::Vec3, {1, 1, 1, 1}}, {"Equal", "A == B", T::Vec3, {1, 1, 1, 1}}, {"Less", "A < B", T::Vec3, {0, 0, 0, 0}}},
         {{"Out", "Result", T::Vec3}}},
        {"StaticSwitch", "Artistic", "开关二选一 (Switch)",
         {{"Condition", "Cond", T::Float, {1.0f, 0, 0, 0}}, {"True", "True", T::Vec3, {1, 1, 1, 1}}, {"False", "False", T::Vec3, {0, 0, 0, 0}}},
         {{"Out", "Result", T::Vec3}}},
    };
    return specs;
}

const ShaderNodeTypeSpec* FindShaderNodeSpec(std::string_view type)
{
    for (const auto& spec : BuiltinShaderNodeSpecs())
    {
        if (spec.type == type) return &spec;
    }
    return nullptr;
}

ShaderGraphDocument MakeDefaultShaderGraph(std::string_view name)
{
    ShaderGraphDocument doc;
    doc.name = name.empty() ? "NewShaderGraph" : std::string(name);
    doc.shadingModel = SurfaceShadingModel::Lit;
    doc.alphaMode = MaterialAlphaMode::Opaque;
    doc.panX = 12.0f;
    doc.panY = 8.0f;
    doc.zoom = 0.62f;

    // Default starter ASE graph: Base Color + Animated Fresnel Rim Glow -> Master Surface!
    ShaderGraphNode colorNode;
    colorNode.id = doc.nextNodeId++;
    colorNode.type = "Color";
    colorNode.x = 16.0f;
    colorNode.y = 36.0f;
    colorNode.exposeAsParam = true;
    colorNode.paramName = "_RimColor";
    colorNode.paramLabel = "边缘发光色";
    colorNode.constantValue = {0.16f, 0.72f, 1.0f, 1.0f};

    ShaderGraphNode powerNode;
    powerNode.id = doc.nextNodeId++;
    powerNode.type = "Range";
    powerNode.x = 16.0f;
    powerNode.y = 168.0f;
    powerNode.exposeAsParam = true;
    powerNode.paramName = "_RimPower";
    powerNode.paramLabel = "边缘光锐度";
    powerNode.rangeMin = 0.5f;
    powerNode.rangeMax = 8.0f;
    powerNode.constantValue = {3.0f, 0.0f, 0.0f, 0.0f};

    ShaderGraphNode fresnelNode;
    fresnelNode.id = doc.nextNodeId++;
    fresnelNode.type = "Fresnel";
    fresnelNode.x = 210.0f;
    fresnelNode.y = 164.0f;

    ShaderGraphNode mulNode;
    mulNode.id = doc.nextNodeId++;
    mulNode.type = "Multiply";
    mulNode.x = 404.0f;
    mulNode.y = 108.0f;

    ShaderGraphNode baseMapNode;
    baseMapNode.id = doc.nextNodeId++;
    baseMapNode.type = "BaseColorMap";
    baseMapNode.x = 210.0f;
    baseMapNode.y = 20.0f;

    ShaderGraphNode masterNode;
    masterNode.id = doc.nextNodeId++;
    masterNode.type = "MasterSurface";
    masterNode.x = 596.0f;
    masterNode.y = 28.0f;

    doc.nodes = {colorNode, powerNode, fresnelNode, mulNode, baseMapNode, masterNode};
    doc.links = {
        {powerNode.id, "Out", fresnelNode.id, "Power"},
        {colorNode.id, "RGB", mulNode.id, "A"},
        {fresnelNode.id, "Out", mulNode.id, "B"},
        {baseMapNode.id, "RGB", masterNode.id, "BaseColor"},
        {mulNode.id, "Out", masterNode.id, "Emissive"},
    };
    return doc;
}

bool SerializeShaderGraph(const ShaderGraphDocument& graph, std::string& outJson, std::string& errorMessage)
{
    errorMessage.clear();
    try
    {
        Json root;
        root["type"] = "FlareEngine.ShaderGraph";
        root["version"] = 1;
        root["name"] = graph.name;
        root["shadingModel"] = graph.shadingModel == SurfaceShadingModel::Unlit ? "Unlit"
                             : graph.shadingModel == SurfaceShadingModel::Toon  ? "Toon"
                                                                                : "Lit";
        root["alphaMode"] = MaterialAlphaModeName(graph.alphaMode);
        root["doubleSided"] = graph.doubleSided;
        root["nextNodeId"] = graph.nextNodeId;
        root["view"] = {{"panX", graph.panX}, {"panY", graph.panY}, {"zoom", graph.zoom}};

        Json nodesJson = Json::array();
        for (const auto& n : graph.nodes)
        {
            Json nj;
            nj["id"] = n.id;
            nj["type"] = n.type;
            nj["x"] = n.x;
            nj["y"] = n.y;
            if (n.exposeAsParam)
            {
                nj["expose"] = true;
                nj["paramName"] = n.paramName;
                nj["paramLabel"] = n.paramLabel;
            }
            nj["value"] = {n.constantValue.x, n.constantValue.y, n.constantValue.z, n.constantValue.w};
            if (n.type == "Range")
            {
                nj["rangeMin"] = n.rangeMin;
                nj["rangeMax"] = n.rangeMax;
            }
            if (!n.inputDefaults.empty())
            {
                Json defs = Json::object();
                for (const auto& [k, v] : n.inputDefaults)
                    defs[k] = {v.x, v.y, v.z, v.w};
                nj["inputs"] = std::move(defs);
            }
            nodesJson.push_back(std::move(nj));
        }
        root["nodes"] = std::move(nodesJson);

        Json linksJson = Json::array();
        for (const auto& l : graph.links)
        {
            linksJson.push_back({
                {"fromNode", l.fromNode},
                {"fromPort", l.fromPort},
                {"toNode", l.toNode},
                {"toPort", l.toPort},
            });
        }
        root["links"] = std::move(linksJson);

        outJson = root.dump(2) + "\n";
        return true;
    }
    catch (const std::exception& ex)
    {
        errorMessage = ex.what();
        return false;
    }
}

bool DeserializeShaderGraph(std::string_view jsonText, ShaderGraphDocument& outGraph, std::string& errorMessage)
{
    errorMessage.clear();
    try
    {
        if (jsonText.empty() || jsonText.size() > 2 * 1024 * 1024)
        {
            errorMessage = "ShaderGraph JSON is empty or too large.";
            return false;
        }
        const Json root = Json::parse(jsonText);
        if (root.value("type", std::string{}) != "FlareEngine.ShaderGraph" || root.value("version", 0) != 1)
        {
            errorMessage = "Unsupported ShaderGraph format or version.";
            return false;
        }
        ShaderGraphDocument doc;
        doc.name = root.value("name", std::string{"ShaderGraph"});
        const std::string sm = root.value("shadingModel", std::string{"Lit"});
        doc.shadingModel = (sm == "Unlit") ? SurfaceShadingModel::Unlit
                         : (sm == "Toon")  ? SurfaceShadingModel::Toon
                                           : SurfaceShadingModel::Lit;
        MaterialAlphaMode parsedAlpha = MaterialAlphaMode::Opaque;
        if (ParseMaterialAlphaMode(root.value("alphaMode", std::string{"OPAQUE"}), parsedAlpha))
            doc.alphaMode = parsedAlpha;
        doc.doubleSided = root.value("doubleSided", false);
        doc.nextNodeId = root.value("nextNodeId", 1u);
        if (root.contains("view") && root["view"].is_object())
        {
            doc.panX = root["view"].value("panX", 0.0f);
            doc.panY = root["view"].value("panY", 0.0f);
            doc.zoom = std::clamp(root["view"].value("zoom", 1.0f), 0.25f, 2.5f);
        }

        std::uint32_t maxId = 0;
        if (root.contains("nodes") && root["nodes"].is_array())
        {
            for (const auto& nj : root["nodes"])
            {
                ShaderGraphNode n;
                n.id = nj.at("id").get<std::uint32_t>();
                n.type = nj.at("type").get<std::string>();
                if (!FindShaderNodeSpec(n.type))
                {
                    errorMessage = "Unknown ShaderGraph node type: " + n.type;
                    return false;
                }
                n.x = nj.value("x", 0.0f);
                n.y = nj.value("y", 0.0f);
                n.exposeAsParam = nj.value("expose", false);
                n.paramName = nj.value("paramName", std::string{});
                n.paramLabel = nj.value("paramLabel", std::string{});
                if (nj.contains("value") && nj["value"].is_array() && nj["value"].size() == 4)
                {
                    n.constantValue = {nj["value"][0].get<float>(), nj["value"][1].get<float>(),
                                       nj["value"][2].get<float>(), nj["value"][3].get<float>()};
                }
                n.rangeMin = nj.value("rangeMin", 0.0f);
                n.rangeMax = nj.value("rangeMax", 1.0f);
                if (nj.contains("inputs") && nj["inputs"].is_object())
                {
                    for (auto it = nj["inputs"].begin(); it != nj["inputs"].end(); ++it)
                    {
                        if (it.value().is_array() && it.value().size() == 4)
                        {
                            n.inputDefaults[it.key()] = {it.value()[0].get<float>(), it.value()[1].get<float>(),
                                                         it.value()[2].get<float>(), it.value()[3].get<float>()};
                        }
                    }
                }
                maxId = std::max(maxId, n.id);
                doc.nodes.push_back(std::move(n));
            }
        }
        doc.nextNodeId = std::max(doc.nextNodeId, maxId + 1);

        if (root.contains("links") && root["links"].is_array())
        {
            std::set<std::uint32_t> validNodeIds;
            for (const auto& n : doc.nodes) validNodeIds.insert(n.id);
            for (const auto& lj : root["links"])
            {
                ShaderGraphLink link{
                    lj.at("fromNode").get<std::uint32_t>(),
                    lj.at("fromPort").get<std::string>(),
                    lj.at("toNode").get<std::uint32_t>(),
                    lj.at("toPort").get<std::string>(),
                };
                if (link.fromNode == link.toNode || link.fromPort.empty() || link.toPort.empty()) continue;
                if (!validNodeIds.contains(link.fromNode) || !validNodeIds.contains(link.toNode)) continue;
                doc.links.erase(
                    std::remove_if(doc.links.begin(), doc.links.end(), [&](const ShaderGraphLink& existing) {
                        return existing.toNode == link.toNode && existing.toPort == link.toPort;
                    }),
                    doc.links.end());
                doc.links.push_back(std::move(link));
            }
        }

        const bool hasMaster = std::any_of(doc.nodes.begin(), doc.nodes.end(),
                                           [](const auto& n) { return n.type == "MasterSurface"; });
        if (!hasMaster)
        {
            ShaderGraphNode master;
            master.id = doc.nextNodeId++;
            master.type = "MasterSurface";
            master.x = 700.0f;
            master.y = 80.0f;
            doc.nodes.push_back(std::move(master));
        }

        outGraph = std::move(doc);
        return true;
    }
    catch (const std::exception& ex)
    {
        errorMessage = ex.what();
        return false;
    }
}

bool SaveShaderGraph(const std::filesystem::path& path, const ShaderGraphDocument& graph, std::string& errorMessage)
{
    std::string json;
    if (!SerializeShaderGraph(graph, json, errorMessage)) return false;
    std::error_code ec;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), ec);
    if (!asset_io::WriteAtomic(path, {reinterpret_cast<const std::uint8_t*>(json.data()), json.size()}))
    {
        errorMessage = "Failed to write ShaderGraph file.";
        return false;
    }
    return true;
}

bool LoadShaderGraph(const std::filesystem::path& path, ShaderGraphDocument& outGraph, std::string& errorMessage)
{
    std::vector<std::uint8_t> bytes;
    if (!asset_io::Read(path, bytes))
    {
        errorMessage = "Failed to read ShaderGraph file.";
        return false;
    }
    return DeserializeShaderGraph(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
                                  outGraph, errorMessage);
}

namespace
{
bool TopoSortGraph(const ShaderGraphDocument& graph, std::uint32_t rootId,
                   std::vector<std::uint32_t>& outOrder, std::string& errorMessage,
                   bool includeRoot = false)
{
    std::unordered_map<std::uint32_t, const ShaderGraphNode*> nodeById;
    for (const auto& n : graph.nodes) nodeById[n.id] = &n;

    std::unordered_map<std::uint32_t, int> state; // 0=unvisited, 1=visiting, 2=done
    const auto dfs = [&](auto& self, std::uint32_t nid) -> bool {
        const int st = state[nid];
        if (st == 2) return true;
        if (st == 1)
        {
            errorMessage = "ShaderGraph contains a cycle involving node #" + std::to_string(nid) + ".";
            return false;
        }
        state[nid] = 1;
        for (const auto& l : graph.links)
        {
            if (l.toNode == nid && nodeById.contains(l.fromNode))
            {
                if (!self(self, l.fromNode)) return false;
            }
        }
        state[nid] = 2;
        if (includeRoot || nid != rootId) outOrder.push_back(nid);
        return true;
    };
    return dfs(dfs, rootId);
}
} // namespace

bool CompileShaderGraphToHlsl(const ShaderGraphDocument& graph, std::string& outHlsl, std::string& errorMessage)
{
    errorMessage.clear();
    outHlsl.clear();

    const ShaderGraphNode* master = nullptr;
    std::unordered_map<std::uint32_t, const ShaderGraphNode*> nodeById;
    for (const auto& n : graph.nodes)
    {
        nodeById[n.id] = &n;
        if (n.type == "MasterSurface") master = &n;
    }
    if (!master)
    {
        errorMessage = "ShaderGraph is missing MasterSurface node.";
        return false;
    }

    std::vector<std::uint32_t> order;
    if (!TopoSortGraph(graph, master->id, order, errorMessage)) return false;

    std::ostringstream hdr;
    std::string shaderTitle = SanitizeShaderLabQuoted(graph.name.empty() ? "ShaderGraph" : graph.name);
    if (shaderTitle.empty()) shaderTitle = "ShaderGraph";
    hdr << "Shader \"" << shaderTitle << "\"\n{\n    Properties\n    {\n";

    std::unordered_map<std::uint32_t, std::string> exposedIdent;
    std::set<std::string> usedIdents;
    for (std::uint32_t nid : order)
    {
        const ShaderGraphNode& n = *nodeById[nid];
        if (!n.exposeAsParam) continue;
        std::string ident = SanitizeParamIdent(n.paramName, n.id);
        while (usedIdents.contains(ident)) ident += "_" + std::to_string(n.id);
        usedIdents.insert(ident);
        exposedIdent[nid] = ident;
        std::string label = SanitizeShaderLabQuoted(n.paramLabel.empty() ? ident : n.paramLabel);
        if (label.empty()) label = ident;
        if (n.type == "Float")
            hdr << "        " << ident << " (\"" << label << "\", Float) = " << FormatFloat(n.constantValue.x) << "\n";
        else if (n.type == "Range")
            hdr << "        " << ident << " (\"" << label << "\", Range(" << FormatFloat(n.rangeMin) << ", "
                << FormatFloat(n.rangeMax) << ")) = " << FormatFloat(n.constantValue.x) << "\n";
        else if (n.type == "Boolean")
            hdr << "        [Toggle] " << ident << " (\"" << label << "\", Float) = "
                << (n.constantValue.x >= 0.5f ? "1" : "0") << "\n";
        else if (n.type == "Color")
            hdr << "        " << ident << " (\"" << label << "\", Color) = (" << FormatFloat(n.constantValue.x) << ", "
                << FormatFloat(n.constantValue.y) << ", " << FormatFloat(n.constantValue.z) << ", "
                << FormatFloat(n.constantValue.w) << ")\n";
        else if (n.type == "Vector2")
            hdr << "        " << ident << " (\"" << label << "\", Vector2) = (" << FormatFloat(n.constantValue.x) << ", "
                << FormatFloat(n.constantValue.y) << ")\n";
        else if (n.type == "Vector3")
            hdr << "        " << ident << " (\"" << label << "\", Vector3) = (" << FormatFloat(n.constantValue.x) << ", "
                << FormatFloat(n.constantValue.y) << ", " << FormatFloat(n.constantValue.z) << ")\n";
        else if (n.type == "Vector4")
            hdr << "        " << ident << " (\"" << label << "\", Vector4) = (" << FormatFloat(n.constantValue.x) << ", "
                << FormatFloat(n.constantValue.y) << ", " << FormatFloat(n.constantValue.z) << ", "
                << FormatFloat(n.constantValue.w) << ")\n";
    }
    hdr << "    }\n\n    SubShader\n    {\n"
        << "        Shading "
        << (graph.shadingModel == SurfaceShadingModel::Unlit ? "Unlit"
            : graph.shadingModel == SurfaceShadingModel::Toon ? "Toon" : "Lit") << "\n"
        << "        Blend   "
        << (graph.alphaMode == MaterialAlphaMode::Mask ? "Cutout"
            : graph.alphaMode == MaterialAlphaMode::Blend ? "Transparent" : "Opaque") << "\n"
        << "        Cull    " << (graph.doubleSided ? "Off" : "Back") << "\n\n"
        << "        HLSLPROGRAM\n";

    const auto hasAnyNode = [&](std::initializer_list<std::string_view> types) {
        return std::any_of(order.begin(), order.end(), [&](std::uint32_t nid) {
            const std::string_view t = nodeById[nid]->type;
            return std::find(types.begin(), types.end(), t) != types.end();
        });
    };

    const bool usesNoise = hasAnyNode({"SimpleNoise", "GradientNoise", "FbmNoise", "Voronoi", "BrickPattern"});
    const bool usesHsv = hasAnyNode({"RGBToHSV", "HSVToRGB", "HueShift"});
    const bool usesRotAxis = hasAnyNode({"RotateAboutAxis"});
    const bool usesPolygon = hasAnyNode({"PolygonShape"});

    std::ostringstream helpers;
    if (usesNoise)
    {
        helpers << "\nfloat FlareSgHash21(float2 p)\n{\n"
                << "    return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453);\n}\n"
                << "float FlareSgNoise(float2 uv)\n{\n"
                << "    float2 i = floor(uv), f = frac(uv);\n"
                << "    float2 u = f * f * (3.0 - 2.0 * f);\n"
                << "    return lerp(lerp(FlareSgHash21(i), FlareSgHash21(i + float2(1, 0)), u.x),\n"
                << "                lerp(FlareSgHash21(i + float2(0, 1)), FlareSgHash21(i + float2(1, 1)), u.x), u.y);\n}\n"
                << "float FlareSgGradNoise(float2 uv)\n{\n"
                << "    float2 i = floor(uv), f = frac(uv);\n"
                << "    float2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);\n"
                << "    float a00 = FlareSgHash21(i) * 6.2831853;\n"
                << "    float a10 = FlareSgHash21(i + float2(1, 0)) * 6.2831853;\n"
                << "    float a01 = FlareSgHash21(i + float2(0, 1)) * 6.2831853;\n"
                << "    float a11 = FlareSgHash21(i + float2(1, 1)) * 6.2831853;\n"
                << "    float n00 = dot(float2(cos(a00), sin(a00)), f);\n"
                << "    float n10 = dot(float2(cos(a10), sin(a10)), f - float2(1, 0));\n"
                << "    float n01 = dot(float2(cos(a01), sin(a01)), f - float2(0, 1));\n"
                << "    float n11 = dot(float2(cos(a11), sin(a11)), f - float2(1, 1));\n"
                << "    return saturate(lerp(lerp(n00, n10, u.x), lerp(n01, n11, u.x), u.y) * 0.7071 + 0.5);\n}\n"
                << "float FlareSgFbm(float2 uv, float detail)\n{\n"
                << "    float gain = clamp(detail, 0.1, 0.9);\n"
                << "    float sum = 0.0, amp = 0.5, norm = 0.0, freq = 1.0;\n"
                << "    [unroll] for (int oct = 0; oct < 4; ++oct)\n"
                << "    {\n"
                << "        sum += amp * FlareSgNoise(uv * freq + float2(oct * 13.7, oct * 29.3));\n"
                << "        norm += amp; amp *= gain; freq *= 2.02;\n"
                << "    }\n"
                << "    return saturate(sum / max(norm, 1e-5));\n}\n"
                << "float2 FlareSgVoronoi(float2 uv)\n{\n"
                << "    float2 n = floor(uv), f = frac(uv);\n"
                << "    float md = 8.0, cid = 0.0;\n"
                << "    [unroll] for (int j = -1; j <= 1; ++j)\n"
                << "    [unroll] for (int i = -1; i <= 1; ++i)\n"
                << "    {\n"
                << "        float2 g = float2(i, j);\n"
                << "        float2 o = float2(FlareSgHash21(n + g), FlareSgHash21(n + g + float2(17.0, 31.0)));\n"
                << "        float d = length(g + o - f);\n"
                << "        if (d < md) { md = d; cid = FlareSgHash21(n + g + float2(53.0, 97.0)); }\n"
                << "    }\n"
                << "    return float2(saturate(md), cid);\n}\n";
    }
    if (usesHsv)
    {
        helpers << "\nfloat3 FlareSgRgb2Hsv(float3 c)\n{\n"
                << "    float4 K = float4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);\n"
                << "    float4 p = lerp(float4(c.bg, K.wz), float4(c.gb, K.xy), step(c.b, c.g));\n"
                << "    float4 q = lerp(float4(p.xyw, c.r), float4(c.r, p.yzx), step(p.x, c.r));\n"
                << "    float d = q.x - min(q.w, q.y);\n"
                << "    return float3(abs(q.z + (q.w - q.y) / (6.0 * d + 1e-6)), d / (q.x + 1e-6), q.x);\n}\n"
                << "float3 FlareSgHsv2Rgb(float3 c)\n{\n"
                << "    float3 p = abs(frac(c.xxx + float3(1.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);\n"
                << "    return c.z * lerp(float3(1, 1, 1), saturate(p - 1.0), saturate(c.y));\n}\n";
    }
    if (usesRotAxis)
    {
        helpers << "\nfloat3 FlareSgRotateAxis(float3 v, float3 axisRaw, float angle)\n{\n"
                << "    float3 k = UnitVector(axisRaw, float3(0, 1, 0));\n"
                << "    float s = sin(angle), c = cos(angle);\n"
                << "    return v * c + cross(k, v) * s + k * (dot(k, v) * (1.0 - c));\n}\n";
    }
    if (usesPolygon)
    {
        helpers << "\nfloat FlareSgPolygon(float2 uv, float sides, float w, float h)\n{\n"
                << "    float2 p = (uv * 2.0 - 1.0) / max(float2(w, h), 1e-4);\n"
                << "    float n = max(3.0, floor(sides));\n"
                << "    float denom = (abs(p.x) < 1e-5 && abs(p.y) < 1e-5) ? 1e-5 : p.y;\n"
                << "    float a = atan2(p.x, denom) + 3.14159265;\n"
                << "    float r = 6.2831853 / n;\n"
                << "    float d = cos(floor(0.5 + a / r) * r - a) * length(p);\n"
                << "    return 1.0 - smoothstep(0.98, 1.02, d);\n}\n";
    }

    struct PortExpr
    {
        std::string expr;
        ShaderPortValueType type = ShaderPortValueType::Float;
    };
    std::map<std::pair<std::uint32_t, std::string>, PortExpr> portExprs;

    const auto findLinkTo = [&](std::uint32_t toNode, std::string_view toPort) -> const ShaderGraphLink* {
        for (const auto& l : graph.links)
        {
            if (l.toNode == toNode && l.toPort == toPort) return &l;
        }
        return nullptr;
    };

    const auto resolveInput = [&](const ShaderGraphNode& node, const ShaderNodePortSpec& portSpec,
                                  ShaderPortValueType desiredType, bool vertexStage = false) -> PortExpr {
        if (const auto* link = findLinkTo(node.id, portSpec.id))
        {
            const auto found = portExprs.find({link->fromNode, link->fromPort});
            if (found != portExprs.end())
            {
                return {CastHlslExpr(found->second.expr, found->second.type, desiredType), desiredType};
            }
        }
        // Smart unconnected defaults across all UV / Time / WorldNormal / WorldPos ports
        if (portSpec.id == "UV" && portSpec.type == ShaderPortValueType::Vec2 && !node.inputDefaults.contains("UV"))
        {
            return {vertexStage ? "V.uv" : "IN.uv", ShaderPortValueType::Vec2};
        }
        if (portSpec.id == "Time" && portSpec.type == ShaderPortValueType::Float && portSpec.defaultValue.x < 0.0f &&
            !node.inputDefaults.contains("Time"))
        {
            return {vertexStage ? "V.time.x" : "IN.time.x", ShaderPortValueType::Float};
        }
        if (portSpec.id == "NormalWS" && portSpec.type == ShaderPortValueType::Vec3 && !node.inputDefaults.contains("NormalWS"))
        {
            return {"IN.worldNormal", ShaderPortValueType::Vec3};
        }
        if (portSpec.id == "CoordsWS" && portSpec.type == ShaderPortValueType::Vec3 && !node.inputDefaults.contains("CoordsWS"))
        {
            return {"IN.worldPos", ShaderPortValueType::Vec3};
        }
        if (portSpec.id == "HeightWS" && portSpec.type == ShaderPortValueType::Float && !node.inputDefaults.contains("HeightWS"))
        {
            return {"IN.worldPos.y", ShaderPortValueType::Float};
        }
        const auto defIt = node.inputDefaults.find(portSpec.id);
        const Vec4 val = (defIt != node.inputDefaults.end()) ? defIt->second : portSpec.defaultValue;
        return {LiteralExpr(val, desiredType), desiredType};
    };

    const auto resolveRawInput = [&](const ShaderGraphNode& node, const ShaderNodePortSpec& portSpec) -> PortExpr {
        if (const auto* link = findLinkTo(node.id, portSpec.id))
        {
            const auto found = portExprs.find({link->fromNode, link->fromPort});
            if (found != portExprs.end()) return found->second;
        }
        const auto defIt = node.inputDefaults.find(portSpec.id);
        const Vec4 val = (defIt != node.inputDefaults.end()) ? defIt->second : portSpec.defaultValue;
        const ShaderPortValueType rawType = HasUniformComponents(val, portSpec.type)
            ? ShaderPortValueType::Float
            : portSpec.type;
        return {LiteralExpr(val, rawType), rawType};
    };

    const auto emitStageNodes = [&](std::ostringstream& stageOut, const std::vector<std::uint32_t>& stageOrder,
                                    bool vertexStage) {
        portExprs.clear();
        for (std::uint32_t nid : stageOrder)
        {
            const ShaderGraphNode& n = *nodeById[nid];
            const ShaderNodeTypeSpec* spec = FindShaderNodeSpec(n.type);
            if (!spec) continue;
            const std::string v = "n" + std::to_string(n.id);
            using T = ShaderPortValueType;

            // ---- 1. Inputs ----
            if (n.type == "UV")
            {
                portExprs[{n.id, "Out"}] = {"IN.uv", T::Vec2};
                portExprs[{n.id, "U"}] = {"IN.uv.x", T::Float};
                portExprs[{n.id, "V"}] = {"IN.uv.y", T::Float};
            }
            else if (n.type == "Time")
            {
                portExprs[{n.id, "Time"}] = {"IN.time.x", T::Float};
                portExprs[{n.id, "SinTime"}] = {"IN.time.y", T::Float};
                portExprs[{n.id, "CosTime"}] = {"IN.time.z", T::Float};
                portExprs[{n.id, "Delta"}] = {"IN.time.w", T::Float};
            }
            else if (n.type == "WorldPos")
            {
                portExprs[{n.id, "Out"}] = {"IN.worldPos", T::Vec3};
                portExprs[{n.id, "X"}] = {"IN.worldPos.x", T::Float};
                portExprs[{n.id, "Y"}] = {"IN.worldPos.y", T::Float};
                portExprs[{n.id, "Z"}] = {"IN.worldPos.z", T::Float};
            }
            else if (n.type == "LocalPos")
            {
                portExprs[{n.id, "Out"}] = {"IN.localPos", T::Vec3};
                portExprs[{n.id, "X"}] = {"IN.localPos.x", T::Float};
                portExprs[{n.id, "Y"}] = {"IN.localPos.y", T::Float};
                portExprs[{n.id, "Z"}] = {"IN.localPos.z", T::Float};
            }
            else if (n.type == "ObjectPos") portExprs[{n.id, "Out"}] = {"IN.objectPos", T::Vec3};
            else if (n.type == "CameraPos") portExprs[{n.id, "Out"}] = {"IN.cameraPos", T::Vec3};
            else if (n.type == "CameraDistance")
            {
                stageOut << "    float " << v << " = distance(IN.worldPos, IN.cameraPos);\n";
                portExprs[{n.id, "Out"}] = {v, T::Float};
            }
            else if (n.type == "WorldNormal") portExprs[{n.id, "Out"}] = {"IN.worldNormal", T::Vec3};
            else if (n.type == "WorldTangent") portExprs[{n.id, "Out"}] = {"IN.worldTangent", T::Vec3};
            else if (n.type == "WorldBitangent") portExprs[{n.id, "Out"}] = {"IN.worldBitangent", T::Vec3};
            else if (n.type == "ViewDir") portExprs[{n.id, "Out"}] = {"IN.viewDir", T::Vec3};
            else if (n.type == "LightDir") portExprs[{n.id, "Out"}] = {"IN.lightDir", T::Vec3};
            else if (n.type == "LightColor")
            {
                portExprs[{n.id, "RGB"}] = {"IN.lightColor", T::Vec3};
                portExprs[{n.id, "Intensity"}] = {"max(IN.lightColor.r, max(IN.lightColor.g, IN.lightColor.b))", T::Float};
            }
            else if (n.type == "ScreenUV") portExprs[{n.id, "Out"}] = {"IN.screenUv", T::Vec2};
            else if (n.type == "Facing")
            {
                portExprs[{n.id, "Out"}] = {"IN.facing", T::Float};
                portExprs[{n.id, "Front"}] = {"step(0.0, IN.facing)", T::Float};
            }
            else if (n.type == "VertexColor")
            {
                portExprs[{n.id, "RGBA"}] = {"IN.vertexColor", T::Vec4};
                portExprs[{n.id, "RGB"}] = {"IN.vertexColor.rgb", T::Vec3};
                portExprs[{n.id, "A"}] = {"IN.vertexColor.a", T::Float};
            }
            // ---- 2. Properties & Constants ----
            else if (n.type == "Float" || n.type == "Range")
            {
                const std::string expr = exposedIdent.contains(n.id) ? exposedIdent[n.id] : FormatFloat(n.constantValue.x);
                portExprs[{n.id, "Out"}] = {expr, T::Float};
            }
            else if (n.type == "Boolean")
            {
                const std::string expr = exposedIdent.contains(n.id)
                    ? ("step(0.5, " + exposedIdent[n.id] + ")")
                    : (n.constantValue.x >= 0.5f ? "1.0" : "0.0");
                portExprs[{n.id, "Out"}] = {expr, T::Float};
            }
            else if (n.type == "Color")
            {
                const std::string expr = exposedIdent.contains(n.id) ? exposedIdent[n.id] : LiteralExpr(n.constantValue, T::Vec4);
                stageOut << "    float4 " << v << " = " << expr << ";\n";
                portExprs[{n.id, "RGBA"}] = {v, T::Vec4};
                portExprs[{n.id, "RGB"}] = {v + ".rgb", T::Vec3};
                portExprs[{n.id, "A"}] = {v + ".a", T::Float};
            }
            else if (n.type == "Vector2")
            {
                const std::string expr = exposedIdent.contains(n.id) ? exposedIdent[n.id] : LiteralExpr(n.constantValue, T::Vec2);
                portExprs[{n.id, "Out"}] = {expr, T::Vec2};
            }
            else if (n.type == "Vector3")
            {
                const std::string expr = exposedIdent.contains(n.id) ? exposedIdent[n.id] : LiteralExpr(n.constantValue, T::Vec3);
                portExprs[{n.id, "Out"}] = {expr, T::Vec3};
            }
            else if (n.type == "Vector4")
            {
                const std::string expr = exposedIdent.contains(n.id) ? exposedIdent[n.id] : LiteralExpr(n.constantValue, T::Vec4);
                portExprs[{n.id, "Out"}] = {expr, T::Vec4};
            }
            else if (n.type == "PI")
            {
                portExprs[{n.id, "PI"}] = {"3.14159265", T::Float};
                portExprs[{n.id, "TAU"}] = {"6.28318531", T::Float};
                portExprs[{n.id, "HalfPI"}] = {"1.57079633", T::Float};
                portExprs[{n.id, "InvPI"}] = {"0.31830989", T::Float};
            }
            else if (n.type == "GoldenRatio")
            {
                portExprs[{n.id, "Phi"}] = {"1.61803399", T::Float};
                portExprs[{n.id, "E"}] = {"2.71828183", T::Float};
                portExprs[{n.id, "Sqrt2"}] = {"1.41421356", T::Float};
            }
            // ---- 3. Texture & Normal ----
            else if (n.type == "BaseColorMap")
            {
                const auto uv = resolveInput(n, spec->inputs[0], T::Vec2, vertexStage);
                if (vertexStage) stageOut << "    float4 " << v << " = float4(1.0, 1.0, 1.0, 1.0);\n";
                else stageOut << "    float4 " << v << " = SampleBaseColor(" << uv.expr << ");\n";
                portExprs[{n.id, "RGBA"}] = {v, T::Vec4};
                portExprs[{n.id, "RGB"}] = {v + ".rgb", T::Vec3};
                portExprs[{n.id, "A"}] = {v + ".a", T::Float};
            }
            else if (n.type == "NormalMap")
            {
                const auto uv = resolveInput(n, spec->inputs[0], T::Vec2, vertexStage);
                if (vertexStage) stageOut << "    float3 " << v << " = float3(0.0, 0.0, 1.0);\n";
                else stageOut << "    float3 " << v << " = SampleNormalTS(" << uv.expr << ");\n";
                portExprs[{n.id, "NormalTS"}] = {v, T::Vec3};
            }
            else if (n.type == "MetallicRoughnessMap")
            {
                const auto uv = resolveInput(n, spec->inputs[0], T::Vec2, vertexStage);
                if (vertexStage) stageOut << "    float2 " << v << " = float2(0.0, 0.5);\n";
                else stageOut << "    float2 " << v << " = SampleMetallicRoughness(" << uv.expr << ");\n";
                portExprs[{n.id, "Metallic"}] = {v + ".x", T::Float};
                portExprs[{n.id, "Roughness"}] = {v + ".y", T::Float};
            }
            else if (n.type == "NormalStrength")
            {
                const auto inN = resolveInput(n, spec->inputs[0], T::Vec3, vertexStage);
                const auto str = resolveInput(n, spec->inputs[1], T::Float, vertexStage);
                stageOut << "    float3 " << v << "_in = " << inN.expr << ";\n";
                stageOut << "    float3 " << v << " = UnitVector(float3(" << v << "_in.xy * (" << str.expr << "), max(" << v << "_in.z, 1e-4)), float3(0, 0, 1));\n";
                portExprs[{n.id, "Out"}] = {v, T::Vec3};
            }
            else if (n.type == "NormalBlend")
            {
                const auto a = resolveInput(n, spec->inputs[0], T::Vec3, vertexStage);
                const auto b = resolveInput(n, spec->inputs[1], T::Vec3, vertexStage);
                stageOut << "    float3 " << v << "_a = (" << a.expr << ") + float3(0, 0, 1);\n";
                stageOut << "    float3 " << v << "_b = (" << b.expr << ") * float3(-1, -1, 1);\n";
                stageOut << "    float3 " << v << " = UnitVector(" << v << "_a * dot(" << v << "_a, " << v << "_b) - " << v << "_b * " << v << "_a.z, float3(0, 0, 1));\n";
                portExprs[{n.id, "Out"}] = {v, T::Vec3};
            }
            else if (n.type == "NormalFromHeight")
            {
                const auto h = resolveInput(n, spec->inputs[0], T::Float, vertexStage);
                const auto str = resolveInput(n, spec->inputs[1], T::Float, vertexStage);
                if (vertexStage)
                {
                    stageOut << "    float3 " << v << " = float3(0.0, 0.0, 1.0);\n";
                }
                else
                {
                    stageOut << "    float " << v << "_h = " << h.expr << ";\n";
                    stageOut << "    float3 " << v << " = UnitVector(float3(-ddx(" << v << "_h) * (" << str.expr
                             << "), -ddy(" << v << "_h) * (" << str.expr << "), 1.0), float3(0, 0, 1));\n";
                }
                portExprs[{n.id, "Out"}] = {v, T::Vec3};
            }
            else if (n.type == "TriplanarBaseColor")
            {
                const auto sc = resolveInput(n, spec->inputs[0], T::Float, vertexStage);
                const auto sh = resolveInput(n, spec->inputs[1], T::Float, vertexStage);
                if (vertexStage)
                {
                    stageOut << "    float4 " << v << " = float4(1.0, 1.0, 1.0, 1.0);\n";
                }
                else
                {
                    stageOut << "    float3 " << v << "_w = pow(max(abs(IN.worldNormal), 1e-4), max(" << sh.expr << ", 0.5));\n";
                    stageOut << "    " << v << "_w /= max(" << v << "_w.x + " << v << "_w.y + " << v << "_w.z, 1e-5);\n";
                    stageOut << "    float " << v << "_s = " << sc.expr << ";\n";
                    stageOut << "    float4 " << v << " = SampleBaseColor(IN.worldPos.zy * " << v << "_s) * " << v << "_w.x + "
                             << "SampleBaseColor(IN.worldPos.xz * " << v << "_s) * " << v << "_w.y + "
                             << "SampleBaseColor(IN.worldPos.xy * " << v << "_s) * " << v << "_w.z;\n";
                }
                portExprs[{n.id, "RGB"}] = {v + ".rgb", T::Vec3};
                portExprs[{n.id, "A"}] = {v + ".a", T::Float};
            }
            else if (n.type == "ParallaxOffset")
            {
                const auto uv = resolveInput(n, spec->inputs[0], T::Vec2, vertexStage);
                const auto h = resolveInput(n, spec->inputs[1], T::Float, vertexStage);
                const auto amp = resolveInput(n, spec->inputs[2], T::Float, vertexStage);
                stageOut << "    float3 " << v << "_vt = float3(dot(IN.viewDir, IN.worldTangent), dot(IN.viewDir, IN.worldBitangent), max(dot(IN.viewDir, IN.worldNormal), 0.25));\n";
                stageOut << "    float2 " << v << " = (" << uv.expr << ") + (" << v << "_vt.xy / " << v << "_vt.z) * (((" << h.expr << ") - 0.5) * (" << amp.expr << "));\n";
                portExprs[{n.id, "Out"}] = {v, T::Vec2};
            }
            else if (n.type == "Flipbook")
            {
                const auto uv = resolveInput(n, spec->inputs[0], T::Vec2, vertexStage);
                const auto cols = resolveInput(n, spec->inputs[1], T::Float, vertexStage);
                const auto rows = resolveInput(n, spec->inputs[2], T::Float, vertexStage);
                const auto frm = resolveInput(n, spec->inputs[3], T::Float, vertexStage);
                stageOut << "    float2 " << v << "_grid = max(floor(float2(" << cols.expr << ", " << rows.expr << ")), 1.0);\n";
                stageOut << "    float " << v << "_idx = floor(" << frm.expr << ");\n";
                stageOut << "    float " << v << "_cx = " << v << "_idx - " << v << "_grid.x * floor(" << v << "_idx / " << v << "_grid.x);\n";
                stageOut << "    float " << v << "_cy = floor(" << v << "_idx / " << v << "_grid.x);\n";
                stageOut << "    " << v << "_cy = " << v << "_cy - " << v << "_grid.y * floor(" << v << "_cy / " << v << "_grid.y);\n";
                stageOut << "    float2 " << v << " = (frac(" << uv.expr << ") + float2(" << v << "_cx, " << v << "_cy)) / " << v << "_grid;\n";
                portExprs[{n.id, "Out"}] = {v, T::Vec2};
            }
            // ---- 4. UV Coordinates ----
            else if (n.type == "TilingOffset")
            {
                const auto uv = resolveInput(n, spec->inputs[0], T::Vec2, vertexStage);
                const auto til = resolveInput(n, spec->inputs[1], T::Vec2, vertexStage);
                const auto off = resolveInput(n, spec->inputs[2], T::Vec2, vertexStage);
                stageOut << "    float2 " << v << " = (" << uv.expr << ") * (" << til.expr << ") + (" << off.expr << ");\n";
                portExprs[{n.id, "Out"}] = {v, T::Vec2};
            }
            else if (n.type == "Panner")
            {
                const auto uv = resolveInput(n, spec->inputs[0], T::Vec2, vertexStage);
                const auto spd = resolveInput(n, spec->inputs[1], T::Vec2, vertexStage);
                const auto tm = resolveInput(n, spec->inputs[2], T::Float, vertexStage);
                stageOut << "    float2 " << v << " = (" << uv.expr << ") + (" << spd.expr << ") * (" << tm.expr << ");\n";
                portExprs[{n.id, "Out"}] = {v, T::Vec2};
            }
            else if (n.type == "Rotator")
            {
                const auto uv = resolveInput(n, spec->inputs[0], T::Vec2, vertexStage);
                const auto ctr = resolveInput(n, spec->inputs[1], T::Vec2, vertexStage);
                const auto ang = resolveInput(n, spec->inputs[2], T::Float, vertexStage);
                stageOut << "    float2 " << v << "_d = (" << uv.expr << ") - (" << ctr.expr << ");\n";
                stageOut << "    float " << v << "_s = sin(" << ang.expr << "), " << v << "_c = cos(" << ang.expr << ");\n";
                stageOut << "    float2 " << v << " = float2(" << v << "_d.x * " << v << "_c - " << v << "_d.y * " << v << "_s, "
                         << v << "_d.x * " << v << "_s + " << v << "_d.y * " << v << "_c) + (" << ctr.expr << ");\n";
                portExprs[{n.id, "Out"}] = {v, T::Vec2};
            }
            else if (n.type == "PolarCoordinates")
            {
                const auto uv = resolveInput(n, spec->inputs[0], T::Vec2, vertexStage);
                const auto ctr = resolveInput(n, spec->inputs[1], T::Vec2, vertexStage);
                const auto rs = resolveInput(n, spec->inputs[2], T::Float, vertexStage);
                const auto ls = resolveInput(n, spec->inputs[3], T::Float, vertexStage);
                stageOut << "    float2 " << v << "_d = (" << uv.expr << ") - (" << ctr.expr << ");\n";
                stageOut << "    float " << v << "_r = length(" << v << "_d) * 2.0 * (" << rs.expr << ");\n";
                stageOut << "    float " << v << "_dx = (abs(" << v << "_d.x) < 1e-6 && abs(" << v << "_d.y) < 1e-6) ? 1e-6 : " << v << "_d.x;\n";
                stageOut << "    float " << v << "_a = (atan2(" << v << "_d.y, " << v << "_dx) / 6.2831853 + 0.5) * (" << ls.expr << ");\n";
                stageOut << "    float2 " << v << " = float2(" << v << "_r, " << v << "_a);\n";
                portExprs[{n.id, "Out"}] = {v, T::Vec2};
                portExprs[{n.id, "Radius"}] = {v + ".x", T::Float};
                portExprs[{n.id, "Angle"}] = {v + ".y", T::Float};
            }
            else if (n.type == "Twirl")
            {
                const auto uv = resolveInput(n, spec->inputs[0], T::Vec2, vertexStage);
                const auto ctr = resolveInput(n, spec->inputs[1], T::Vec2, vertexStage);
                const auto str = resolveInput(n, spec->inputs[2], T::Float, vertexStage);
                const auto off = resolveInput(n, spec->inputs[3], T::Vec2, vertexStage);
                stageOut << "    float2 " << v << "_d = (" << uv.expr << ") - (" << ctr.expr << ");\n";
                stageOut << "    float " << v << "_ang = (" << str.expr << ") * length(" << v << "_d);\n";
                stageOut << "    float " << v << "_s = sin(" << v << "_ang), " << v << "_c = cos(" << v << "_ang);\n";
                stageOut << "    float2 " << v << " = float2(" << v << "_d.x * " << v << "_c - " << v << "_d.y * " << v << "_s, "
                         << v << "_d.x * " << v << "_s + " << v << "_d.y * " << v << "_c) + (" << ctr.expr << ") + (" << off.expr << ");\n";
                portExprs[{n.id, "Out"}] = {v, T::Vec2};
            }
            else if (n.type == "RadialShear")
            {
                const auto uv = resolveInput(n, spec->inputs[0], T::Vec2, vertexStage);
                const auto ctr = resolveInput(n, spec->inputs[1], T::Vec2, vertexStage);
                const auto str = resolveInput(n, spec->inputs[2], T::Vec2, vertexStage);
                const auto off = resolveInput(n, spec->inputs[3], T::Vec2, vertexStage);
                stageOut << "    float2 " << v << "_d = (" << uv.expr << ") - (" << ctr.expr << ");\n";
                stageOut << "    float " << v << "_r2 = dot(" << v << "_d, " << v << "_d);\n";
                stageOut << "    float2 " << v << " = (" << uv.expr << ") + float2(-" << v << "_d.y, " << v << "_d.x) * (" << str.expr << ") * " << v << "_r2 + (" << off.expr << ");\n";
                portExprs[{n.id, "Out"}] = {v, T::Vec2};
            }
            else if (n.type == "Spherize")
            {
                const auto uv = resolveInput(n, spec->inputs[0], T::Vec2, vertexStage);
                const auto ctr = resolveInput(n, spec->inputs[1], T::Vec2, vertexStage);
                const auto str = resolveInput(n, spec->inputs[2], T::Float, vertexStage);
                const auto off = resolveInput(n, spec->inputs[3], T::Vec2, vertexStage);
                stageOut << "    float2 " << v << "_d = (" << uv.expr << ") - (" << ctr.expr << ");\n";
                stageOut << "    float " << v << "_r2 = dot(" << v << "_d, " << v << "_d);\n";
                stageOut << "    float2 " << v << " = (" << ctr.expr << ") + " << v << "_d * (1.0 + " << v << "_r2 * " << v << "_r2 * (" << str.expr << ")) + (" << off.expr << ");\n";
                portExprs[{n.id, "Out"}] = {v, T::Vec2};
            }
            else if (n.type == "PixelateUV")
            {
                const auto uv = resolveInput(n, spec->inputs[0], T::Vec2, vertexStage);
                const auto cells = resolveInput(n, spec->inputs[1], T::Vec2, vertexStage);
                stageOut << "    float2 " << v << "_c = max(floor(" << cells.expr << "), 1.0);\n";
                stageOut << "    float2 " << v << " = (floor((" << uv.expr << ") * " << v << "_c) + 0.5) / " << v << "_c;\n";
                portExprs[{n.id, "Out"}] = {v, T::Vec2};
            }
            else if (n.type == "MirrorUV")
            {
                const auto uv = resolveInput(n, spec->inputs[0], T::Vec2, vertexStage);
                stageOut << "    float2 " << v << " = 1.0 - abs(frac((" << uv.expr << ") * 0.5) * 2.0 - 1.0);\n";
                portExprs[{n.id, "Out"}] = {v, T::Vec2};
            }
            else if (n.type == "WorldAlignedUV")
            {
                const auto sc = resolveInput(n, spec->inputs[0], T::Float, vertexStage);
                stageOut << "    float3 " << v << " = IN.worldPos * (" << sc.expr << ");\n";
                portExprs[{n.id, "XZ"}] = {v + ".xz", T::Vec2};
                portExprs[{n.id, "XY"}] = {v + ".xy", T::Vec2};
                portExprs[{n.id, "YZ"}] = {v + ".yz", T::Vec2};
            }
            // ---- 5. Math ----
            else if (n.type == "Add" || n.type == "Subtract" || n.type == "Multiply" || n.type == "Divide" ||
                     n.type == "Min" || n.type == "Max" || n.type == "Fmod")
            {
                const auto rawA = resolveRawInput(n, spec->inputs[0]);
                const auto rawB = resolveRawInput(n, spec->inputs[1]);
                const T outT = WiderType(rawA.type, rawB.type);
                const std::string a = CastHlslExpr(rawA.expr, rawA.type, outT);
                const std::string b = CastHlslExpr(rawB.expr, rawB.type, outT);
                if (n.type == "Divide")
                {
                    stageOut << "    " << HlslTypeName(outT) << " " << v << "_b = " << b << ";\n";
                    stageOut << "    " << HlslTypeName(outT) << " " << v << " = (" << a << ") * ((step(0.0, " << v
                             << "_b) * 2.0 - 1.0) / max(abs(" << v << "_b), 1e-5));\n";
                }
                else if (n.type == "Min")
                    stageOut << "    " << HlslTypeName(outT) << " " << v << " = min(" << a << ", " << b << ");\n";
                else if (n.type == "Max")
                    stageOut << "    " << HlslTypeName(outT) << " " << v << " = max(" << a << ", " << b << ");\n";
                else if (n.type == "Fmod")
                {
                    stageOut << "    " << HlslTypeName(outT) << " " << v << "_m = max(abs(" << b << "), 1e-5);\n";
                    stageOut << "    " << HlslTypeName(outT) << " " << v << " = (" << a << ") - " << v << "_m * floor((" << a << ") / " << v << "_m);\n";
                }
                else
                {
                    const char* op = (n.type == "Add") ? " + " : (n.type == "Subtract") ? " - " : " * ";
                    stageOut << "    " << HlslTypeName(outT) << " " << v << " = (" << a << ")" << op << "(" << b << ");\n";
                }
                portExprs[{n.id, "Out"}] = {v, outT};
            }
            else if (n.type == "MultiplyAdd")
            {
                const auto rawA = resolveRawInput(n, spec->inputs[0]);
                const auto rawB = resolveRawInput(n, spec->inputs[1]);
                const auto rawC = resolveRawInput(n, spec->inputs[2]);
                const T outT = WiderType(WiderType(rawA.type, rawB.type), rawC.type);
                stageOut << "    " << HlslTypeName(outT) << " " << v << " = ("
                         << CastHlslExpr(rawA.expr, rawA.type, outT) << ") * ("
                         << CastHlslExpr(rawB.expr, rawB.type, outT) << ") + ("
                         << CastHlslExpr(rawC.expr, rawC.type, outT) << ");\n";
                portExprs[{n.id, "Out"}] = {v, outT};
            }
            else if (n.type == "OneMinus" || n.type == "Negate" || n.type == "Reciprocal" ||
                     n.type == "Abs" || n.type == "Sign" || n.type == "Saturate" ||
                     n.type == "Floor" || n.type == "Ceil" || n.type == "Round" || n.type == "Frac" ||
                     n.type == "Sqrt" || n.type == "Rsqrt")
            {
                const auto raw = resolveRawInput(n, spec->inputs[0]);
                const T outT = raw.type;
                const std::string tp = HlslTypeName(outT);
                if (n.type == "OneMinus") stageOut << "    " << tp << " " << v << " = 1.0 - (" << raw.expr << ");\n";
                else if (n.type == "Negate") stageOut << "    " << tp << " " << v << " = -(" << raw.expr << ");\n";
                else if (n.type == "Reciprocal")
                {
                    stageOut << "    " << tp << " " << v << "_x = " << raw.expr << ";\n";
                    stageOut << "    " << tp << " " << v << " = (step(0.0, " << v << "_x) * 2.0 - 1.0) / max(abs(" << v << "_x), 1e-5);\n";
                }
                else if (n.type == "Abs") stageOut << "    " << tp << " " << v << " = abs(" << raw.expr << ");\n";
                else if (n.type == "Sign") stageOut << "    " << tp << " " << v << " = sign(" << raw.expr << ");\n";
                else if (n.type == "Saturate") stageOut << "    " << tp << " " << v << " = saturate(" << raw.expr << ");\n";
                else if (n.type == "Floor") stageOut << "    " << tp << " " << v << " = floor(" << raw.expr << ");\n";
                else if (n.type == "Ceil") stageOut << "    " << tp << " " << v << " = ceil(" << raw.expr << ");\n";
                else if (n.type == "Round") stageOut << "    " << tp << " " << v << " = round(" << raw.expr << ");\n";
                else if (n.type == "Frac") stageOut << "    " << tp << " " << v << " = frac(" << raw.expr << ");\n";
                else if (n.type == "Sqrt") stageOut << "    " << tp << " " << v << " = sqrt(max(" << raw.expr << ", 0.0));\n";
                else stageOut << "    " << tp << " " << v << " = rsqrt(max(" << raw.expr << ", 1e-6));\n";
                portExprs[{n.id, "Out"}] = {v, outT};
            }
            else if (n.type == "Clamp")
            {
                const auto rx = resolveRawInput(n, spec->inputs[0]);
                const auto rmin = resolveRawInput(n, spec->inputs[1]);
                const auto rmax = resolveRawInput(n, spec->inputs[2]);
                const T outT = WiderType(WiderType(rx.type, rmin.type), rmax.type);
                const std::string mn = CastHlslExpr(rmin.expr, rmin.type, outT);
                const std::string mx = CastHlslExpr(rmax.expr, rmax.type, outT);
                stageOut << "    " << HlslTypeName(outT) << " " << v << " = clamp("
                         << CastHlslExpr(rx.expr, rx.type, outT) << ", " << mn << ", max(" << mn << ", " << mx << "));\n";
                portExprs[{n.id, "Out"}] = {v, outT};
            }
            else if (n.type == "Power")
            {
                const auto b = resolveInput(n, spec->inputs[0], T::Float, vertexStage);
                const auto e = resolveInput(n, spec->inputs[1], T::Float, vertexStage);
                stageOut << "    float " << v << " = pow(max(" << b.expr << ", 1e-5), " << e.expr << ");\n";
                portExprs[{n.id, "Out"}] = {v, T::Float};
            }
            else if (n.type == "Exp")
            {
                const auto x = resolveInput(n, spec->inputs[0], T::Float, vertexStage);
                stageOut << "    float " << v << "_x = clamp(" << x.expr << ", -20.0, 20.0);\n";
                portExprs[{n.id, "Out"}] = {"exp(" + v + "_x)", T::Float};
                portExprs[{n.id, "Exp2"}] = {"exp2(" + v + "_x)", T::Float};
            }
            else if (n.type == "Log")
            {
                const auto x = resolveInput(n, spec->inputs[0], T::Float, vertexStage);
                stageOut << "    float " << v << "_x = max(" << x.expr << ", 1e-6);\n";
                portExprs[{n.id, "Out"}] = {"log(" + v + "_x)", T::Float};
                portExprs[{n.id, "Log2"}] = {"log2(" + v + "_x)", T::Float};
            }
            else if (n.type == "Lerp")
            {
                const auto rawA = resolveRawInput(n, spec->inputs[0]);
                const auto rawB = resolveRawInput(n, spec->inputs[1]);
                const T outT = WiderType(rawA.type, rawB.type);
                const auto t = resolveInput(n, spec->inputs[2], T::Float, vertexStage);
                stageOut << "    " << HlslTypeName(outT) << " " << v << " = lerp("
                         << CastHlslExpr(rawA.expr, rawA.type, outT) << ", "
                         << CastHlslExpr(rawB.expr, rawB.type, outT) << ", " << t.expr << ");\n";
                portExprs[{n.id, "Out"}] = {v, outT};
            }
            else if (n.type == "InverseLerp")
            {
                const auto a = resolveInput(n, spec->inputs[0], T::Float, vertexStage);
                const auto b = resolveInput(n, spec->inputs[1], T::Float, vertexStage);
                const auto t = resolveInput(n, spec->inputs[2], T::Float, vertexStage);
                stageOut << "    float " << v << "_d = (" << b.expr << ") - (" << a.expr << ");\n";
                stageOut << "    float " << v << " = saturate(((" << t.expr << ") - (" << a.expr << ")) / ((abs(" << v << "_d) < 1e-5) ? 1e-5 : " << v << "_d));\n";
                portExprs[{n.id, "Out"}] = {v, T::Float};
            }
            else if (n.type == "Smoothstep")
            {
                const auto e0 = resolveInput(n, spec->inputs[0], T::Float, vertexStage);
                const auto e1 = resolveInput(n, spec->inputs[1], T::Float, vertexStage);
                const auto x = resolveInput(n, spec->inputs[2], T::Float, vertexStage);
                stageOut << "    float " << v << " = smoothstep(" << e0.expr << ", max(" << e0.expr << " + 1e-4, " << e1.expr << "), " << x.expr << ");\n";
                portExprs[{n.id, "Out"}] = {v, T::Float};
            }
            else if (n.type == "Step")
            {
                const auto e = resolveInput(n, spec->inputs[0], T::Float, vertexStage);
                const auto x = resolveInput(n, spec->inputs[1], T::Float, vertexStage);
                stageOut << "    float " << v << " = step(" << e.expr << ", " << x.expr << ");\n";
                portExprs[{n.id, "Out"}] = {v, T::Float};
            }
            else if (n.type == "PingPong")
            {
                const auto x = resolveInput(n, spec->inputs[0], T::Float, vertexStage);
                const auto len = resolveInput(n, spec->inputs[1], T::Float, vertexStage);
                stageOut << "    float " << v << "_l = max(abs(" << len.expr << "), 1e-5);\n";
                stageOut << "    float " << v << "_t = (" << x.expr << ") / (" << v << "_l * 2.0);\n";
                stageOut << "    float " << v << " = (1.0 - abs(frac(" << v << "_t) * 2.0 - 1.0)) * " << v << "_l;\n";
                portExprs[{n.id, "Out"}] = {v, T::Float};
            }
            else if (n.type == "Sin" || n.type == "Cos" || n.type == "Tan" ||
                     n.type == "Asin" || n.type == "Acos" || n.type == "Radians" || n.type == "Degrees")
            {
                const auto x = resolveInput(n, spec->inputs[0], T::Float, vertexStage);
                if (n.type == "Sin") stageOut << "    float " << v << " = sin(" << x.expr << ");\n";
                else if (n.type == "Cos") stageOut << "    float " << v << " = cos(" << x.expr << ");\n";
                else if (n.type == "Tan") stageOut << "    float " << v << " = tan(" << x.expr << ");\n";
                else if (n.type == "Asin") stageOut << "    float " << v << " = asin(clamp(" << x.expr << ", -1.0, 1.0));\n";
                else if (n.type == "Acos") stageOut << "    float " << v << " = acos(clamp(" << x.expr << ", -1.0, 1.0));\n";
                else if (n.type == "Radians") stageOut << "    float " << v << " = radians(" << x.expr << ");\n";
                else stageOut << "    float " << v << " = degrees(" << x.expr << ");\n";
                portExprs[{n.id, "Out"}] = {v, T::Float};
            }
            else if (n.type == "Atan2")
            {
                const auto y = resolveInput(n, spec->inputs[0], T::Float, vertexStage);
                const auto x = resolveInput(n, spec->inputs[1], T::Float, vertexStage);
                stageOut << "    float " << v << "_y = " << y.expr << ", " << v << "_x = " << x.expr << ";\n";
                stageOut << "    float " << v << " = atan2(" << v << "_y, (abs(" << v << "_x) < 1e-6 && abs(" << v << "_y) < 1e-6) ? 1e-6 : " << v << "_x);\n";
                portExprs[{n.id, "Out"}] = {v, T::Float};
            }
            else if (n.type == "TriangleWave")
            {
                const auto x = resolveInput(n, spec->inputs[0], T::Float, vertexStage);
                stageOut << "    float " << v << " = 1.0 - abs(frac(" << x.expr << ") * 2.0 - 1.0);\n";
                portExprs[{n.id, "Out"}] = {v, T::Float};
            }
            else if (n.type == "SquareWave")
            {
                const auto x = resolveInput(n, spec->inputs[0], T::Float, vertexStage);
                const auto duty = resolveInput(n, spec->inputs[1], T::Float, vertexStage);
                stageOut << "    float " << v << " = step(frac(" << x.expr << "), saturate(" << duty.expr << "));\n";
                portExprs[{n.id, "Out"}] = {v, T::Float};
            }
            else if (n.type == "SawtoothWave")
            {
                const auto x = resolveInput(n, spec->inputs[0], T::Float, vertexStage);
                stageOut << "    float " << v << " = frac(" << x.expr << ") * 2.0 - 1.0;\n";
                portExprs[{n.id, "Out"}] = {v, T::Float};
            }
            else if (n.type == "Remap")
            {
                const auto x = resolveInput(n, spec->inputs[0], T::Float, vertexStage);
                const auto imin = resolveInput(n, spec->inputs[1], T::Float, vertexStage);
                const auto imax = resolveInput(n, spec->inputs[2], T::Float, vertexStage);
                const auto omin = resolveInput(n, spec->inputs[3], T::Float, vertexStage);
                const auto omax = resolveInput(n, spec->inputs[4], T::Float, vertexStage);
                stageOut << "    float " << v << "_d = (" << imax.expr << ") - (" << imin.expr << ");\n";
                stageOut << "    float " << v << " = lerp(" << omin.expr << ", " << omax.expr << ", ((" << x.expr << ") - (" << imin.expr
                         << ")) * ((step(0.0, " << v << "_d) * 2.0 - 1.0) / max(abs(" << v << "_d), 1e-5)));\n";
                portExprs[{n.id, "Out"}] = {v, T::Float};
            }
            // ---- 6. Vector & Channel ----
            else if (n.type == "Dot")
            {
                const auto a = resolveInput(n, spec->inputs[0], T::Vec3, vertexStage);
                const auto b = resolveInput(n, spec->inputs[1], T::Vec3, vertexStage);
                stageOut << "    float " << v << " = dot(" << a.expr << ", " << b.expr << ");\n";
                portExprs[{n.id, "Out"}] = {v, T::Float};
            }
            else if (n.type == "Cross")
            {
                const auto a = resolveInput(n, spec->inputs[0], T::Vec3, vertexStage);
                const auto b = resolveInput(n, spec->inputs[1], T::Vec3, vertexStage);
                stageOut << "    float3 " << v << " = cross(" << a.expr << ", " << b.expr << ");\n";
                portExprs[{n.id, "Out"}] = {v, T::Vec3};
            }
            else if (n.type == "Normalize")
            {
                const auto x = resolveInput(n, spec->inputs[0], T::Vec3, vertexStage);
                stageOut << "    float3 " << v << " = UnitVector(" << x.expr << ", float3(0, 0, 1));\n";
                portExprs[{n.id, "Out"}] = {v, T::Vec3};
            }
            else if (n.type == "Length")
            {
                const auto x = resolveInput(n, spec->inputs[0], T::Vec3, vertexStage);
                stageOut << "    float " << v << " = length(" << x.expr << ");\n";
                portExprs[{n.id, "Out"}] = {v, T::Float};
            }
            else if (n.type == "Distance")
            {
                const auto a = resolveInput(n, spec->inputs[0], T::Vec3, vertexStage);
                const auto b = resolveInput(n, spec->inputs[1], T::Vec3, vertexStage);
                stageOut << "    float " << v << " = distance(" << a.expr << ", " << b.expr << ");\n";
                portExprs[{n.id, "Out"}] = {v, T::Float};
            }
            else if (n.type == "Reflect")
            {
                const auto inc = resolveInput(n, spec->inputs[0], T::Vec3, vertexStage);
                const auto nrm = resolveInput(n, spec->inputs[1], T::Vec3, vertexStage);
                stageOut << "    float3 " << v << " = reflect(" << inc.expr << ", UnitVector(" << nrm.expr << ", float3(0, 1, 0)));\n";
                portExprs[{n.id, "Out"}] = {v, T::Vec3};
            }
            else if (n.type == "Project" || n.type == "Reject")
            {
                const auto a = resolveInput(n, spec->inputs[0], T::Vec3, vertexStage);
                const auto b = resolveInput(n, spec->inputs[1], T::Vec3, vertexStage);
                stageOut << "    float3 " << v << "_u = UnitVector(" << b.expr << ", float3(0, 1, 0));\n";
                stageOut << "    float3 " << v << "_p = " << v << "_u * dot(" << a.expr << ", " << v << "_u);\n";
                if (n.type == "Project") stageOut << "    float3 " << v << " = " << v << "_p;\n";
                else stageOut << "    float3 " << v << " = (" << a.expr << ") - " << v << "_p;\n";
                portExprs[{n.id, "Out"}] = {v, T::Vec3};
            }
            else if (n.type == "RotateAboutAxis")
            {
                const auto x = resolveInput(n, spec->inputs[0], T::Vec3, vertexStage);
                const auto ax = resolveInput(n, spec->inputs[1], T::Vec3, vertexStage);
                const auto ang = resolveInput(n, spec->inputs[2], T::Float, vertexStage);
                stageOut << "    float3 " << v << " = FlareSgRotateAxis(" << x.expr << ", " << ax.expr << ", " << ang.expr << ");\n";
                portExprs[{n.id, "Out"}] = {v, T::Vec3};
            }
            else if (n.type == "Split")
            {
                const auto x = resolveInput(n, spec->inputs[0], T::Vec4, vertexStage);
                stageOut << "    float4 " << v << " = " << x.expr << ";\n";
                portExprs[{n.id, "R"}] = {v + ".x", T::Float};
                portExprs[{n.id, "G"}] = {v + ".y", T::Float};
                portExprs[{n.id, "B"}] = {v + ".z", T::Float};
                portExprs[{n.id, "A"}] = {v + ".w", T::Float};
            }
            else if (n.type == "Combine")
            {
                const auto r = resolveInput(n, spec->inputs[0], T::Float, vertexStage);
                const auto g = resolveInput(n, spec->inputs[1], T::Float, vertexStage);
                const auto b = resolveInput(n, spec->inputs[2], T::Float, vertexStage);
                const auto a = resolveInput(n, spec->inputs[3], T::Float, vertexStage);
                stageOut << "    float4 " << v << " = float4(" << r.expr << ", " << g.expr << ", " << b.expr << ", " << a.expr << ");\n";
                portExprs[{n.id, "XYZ"}] = {v + ".xyz", T::Vec3};
                portExprs[{n.id, "XYZW"}] = {v, T::Vec4};
            }
            else if (n.type == "Append")
            {
                const auto a = resolveInput(n, spec->inputs[0], T::Vec2, vertexStage);
                const auto b = resolveInput(n, spec->inputs[1], T::Float, vertexStage);
                const auto c = resolveInput(n, spec->inputs[2], T::Float, vertexStage);
                stageOut << "    float4 " << v << " = float4(" << a.expr << ", " << b.expr << ", " << c.expr << ");\n";
                portExprs[{n.id, "XY"}] = {"float2((" + a.expr + ").x, " + b.expr + ")", T::Vec2};
                portExprs[{n.id, "XYZ"}] = {v + ".xyz", T::Vec3};
                portExprs[{n.id, "XYZW"}] = {v, T::Vec4};
            }
            else if (n.type == "ComponentMask")
            {
                const auto x = resolveInput(n, spec->inputs[0], T::Vec4, vertexStage);
                stageOut << "    float4 " << v << " = " << x.expr << ";\n";
                portExprs[{n.id, "XY"}] = {v + ".xy", T::Vec2};
                portExprs[{n.id, "XZ"}] = {v + ".xz", T::Vec2};
                portExprs[{n.id, "YZ"}] = {v + ".yz", T::Vec2};
                portExprs[{n.id, "ZYX"}] = {v + ".zyx", T::Vec3};
            }
            else if (n.type == "TransformTS2WS")
            {
                const auto x = resolveInput(n, spec->inputs[0], T::Vec3, vertexStage);
                stageOut << "    float3 " << v << "_ts = " << x.expr << ";\n";
                stageOut << "    float3 " << v << " = IN.worldTangent * " << v << "_ts.x + IN.worldBitangent * " << v << "_ts.y + IN.worldNormal * " << v << "_ts.z;\n";
                portExprs[{n.id, "Out"}] = {v, T::Vec3};
            }
            // ---- 7. Color & Blend ----
            else if (n.type == "RGBToHSV")
            {
                const auto c = resolveInput(n, spec->inputs[0], T::Vec3, vertexStage);
                stageOut << "    float3 " << v << " = FlareSgRgb2Hsv(max(" << c.expr << ", 0.0));\n";
                portExprs[{n.id, "HSV"}] = {v, T::Vec3};
                portExprs[{n.id, "H"}] = {v + ".x", T::Float};
                portExprs[{n.id, "S"}] = {v + ".y", T::Float};
                portExprs[{n.id, "V"}] = {v + ".z", T::Float};
            }
            else if (n.type == "HSVToRGB")
            {
                const auto h = resolveInput(n, spec->inputs[0], T::Float, vertexStage);
                const auto s = resolveInput(n, spec->inputs[1], T::Float, vertexStage);
                const auto val = resolveInput(n, spec->inputs[2], T::Float, vertexStage);
                stageOut << "    float3 " << v << " = FlareSgHsv2Rgb(float3(" << h.expr << ", " << s.expr << ", " << val.expr << "));\n";
                portExprs[{n.id, "RGB"}] = {v, T::Vec3};
            }
            else if (n.type == "HueShift")
            {
                const auto c = resolveInput(n, spec->inputs[0], T::Vec3, vertexStage);
                const auto sh = resolveInput(n, spec->inputs[1], T::Float, vertexStage);
                stageOut << "    float3 " << v << "_hsv = FlareSgRgb2Hsv(max(" << c.expr << ", 0.0));\n";
                stageOut << "    " << v << "_hsv.x = frac(" << v << "_hsv.x + (" << sh.expr << "));\n";
                stageOut << "    float3 " << v << " = FlareSgHsv2Rgb(" << v << "_hsv);\n";
                portExprs[{n.id, "Out"}] = {v, T::Vec3};
            }
            else if (n.type == "Saturation")
            {
                const auto c = resolveInput(n, spec->inputs[0], T::Vec3, vertexStage);
                const auto sat = resolveInput(n, spec->inputs[1], T::Float, vertexStage);
                stageOut << "    float3 " << v << "_c = " << c.expr << ";\n";
                stageOut << "    float " << v << "_l = dot(" << v << "_c, float3(0.2126, 0.7152, 0.0722));\n";
                stageOut << "    float3 " << v << " = lerp(float3(" << v << "_l, " << v << "_l, " << v << "_l), " << v << "_c, " << sat.expr << ");\n";
                portExprs[{n.id, "Out"}] = {v, T::Vec3};
            }
            else if (n.type == "Contrast")
            {
                const auto c = resolveInput(n, spec->inputs[0], T::Vec3, vertexStage);
                const auto con = resolveInput(n, spec->inputs[1], T::Float, vertexStage);
                const auto mid = resolveInput(n, spec->inputs[2], T::Float, vertexStage);
                stageOut << "    float3 " << v << " = ((" << c.expr << ") - (" << mid.expr << ")) * (" << con.expr << ") + (" << mid.expr << ");\n";
                portExprs[{n.id, "Out"}] = {v, T::Vec3};
            }
            else if (n.type == "BrightnessGamma")
            {
                const auto c = resolveInput(n, spec->inputs[0], T::Vec3, vertexStage);
                const auto br = resolveInput(n, spec->inputs[1], T::Float, vertexStage);
                const auto gm = resolveInput(n, spec->inputs[2], T::Float, vertexStage);
                stageOut << "    float3 " << v << " = pow(max((" << c.expr << ") * (" << br.expr << "), 1e-5), 1.0 / max(" << gm.expr << ", 0.05));\n";
                portExprs[{n.id, "Out"}] = {v, T::Vec3};
            }
            else if (n.type == "Luminance")
            {
                const auto c = resolveInput(n, spec->inputs[0], T::Vec3, vertexStage);
                stageOut << "    float " << v << " = dot(" << c.expr << ", float3(0.2126, 0.7152, 0.0722));\n";
                portExprs[{n.id, "Out"}] = {v, T::Float};
            }
            else if (n.type == "Posterize")
            {
                const auto c = resolveInput(n, spec->inputs[0], T::Vec3, vertexStage);
                const auto s = resolveInput(n, spec->inputs[1], T::Float, vertexStage);
                stageOut << "    float " << v << "_steps = max(floor(" << s.expr << "), 1.0);\n";
                stageOut << "    float3 " << v << " = floor((" << c.expr << ") * " << v << "_steps) / " << v << "_steps;\n";
                portExprs[{n.id, "Out"}] = {v, T::Vec3};
            }
            else if (n.type == "ColorRamp")
            {
                const auto t = resolveInput(n, spec->inputs[0], T::Float, vertexStage);
                const auto ca = resolveInput(n, spec->inputs[1], T::Vec3, vertexStage);
                const auto cb = resolveInput(n, spec->inputs[2], T::Vec3, vertexStage);
                stageOut << "    float3 " << v << " = lerp(" << ca.expr << ", " << cb.expr << ", smoothstep(0.0, 1.0, saturate(" << t.expr << ")));\n";
                portExprs[{n.id, "Out"}] = {v, T::Vec3};
            }
            else if (n.type == "ColorMask")
            {
                const auto c = resolveInput(n, spec->inputs[0], T::Vec3, vertexStage);
                const auto mc = resolveInput(n, spec->inputs[1], T::Vec3, vertexStage);
                const auto rng = resolveInput(n, spec->inputs[2], T::Float, vertexStage);
                const auto fuz = resolveInput(n, spec->inputs[3], T::Float, vertexStage);
                stageOut << "    float " << v << "_d = distance(" << c.expr << ", " << mc.expr << ");\n";
                stageOut << "    float " << v << " = saturate(1.0 - (" << v << "_d - (" << rng.expr << ")) / max(" << fuz.expr << ", 1e-4));\n";
                portExprs[{n.id, "Out"}] = {v, T::Float};
            }
            else if (n.type == "BlendScreen" || n.type == "BlendOverlay" || n.type == "BlendSoftLight")
            {
                const auto a = resolveInput(n, spec->inputs[0], T::Vec3, vertexStage);
                const auto b = resolveInput(n, spec->inputs[1], T::Vec3, vertexStage);
                const auto op = resolveInput(n, spec->inputs[2], T::Float, vertexStage);
                stageOut << "    float3 " << v << "_a = saturate(" << a.expr << "), " << v << "_b = saturate(" << b.expr << ");\n";
                if (n.type == "BlendScreen")
                    stageOut << "    float3 " << v << "_r = 1.0 - (1.0 - " << v << "_a) * (1.0 - " << v << "_b);\n";
                else if (n.type == "BlendOverlay")
                    stageOut << "    float3 " << v << "_r = lerp(2.0 * " << v << "_a * " << v << "_b, 1.0 - 2.0 * (1.0 - " << v << "_a) * (1.0 - " << v << "_b), step(0.5, " << v << "_a));\n";
                else
                    stageOut << "    float3 " << v << "_r = lerp(2.0 * " << v << "_a * " << v << "_b + " << v << "_a * " << v << "_a * (1.0 - 2.0 * " << v << "_b), sqrt(max(" << v << "_a, 0.0)) * (2.0 * " << v << "_b - 1.0) + 2.0 * " << v << "_a * (1.0 - " << v << "_b), step(0.5, " << v << "_b));\n";
                stageOut << "    float3 " << v << " = lerp(" << a.expr << ", " << v << "_r, saturate(" << op.expr << "));\n";
                portExprs[{n.id, "Out"}] = {v, T::Vec3};
            }
            else if (n.type == "HDRBoost")
            {
                const auto c = resolveInput(n, spec->inputs[0], T::Vec3, vertexStage);
                const auto ev = resolveInput(n, spec->inputs[1], T::Float, vertexStage);
                stageOut << "    float3 " << v << " = (" << c.expr << ") * exp2(clamp(" << ev.expr << ", -10.0, 12.0));\n";
                portExprs[{n.id, "Out"}] = {v, T::Vec3};
            }
            // ---- 8. Procedural Noise & Shapes ----
            else if (n.type == "SimpleNoise")
            {
                const auto uv = resolveInput(n, spec->inputs[0], T::Vec2, vertexStage);
                const auto sc = resolveInput(n, spec->inputs[1], T::Float, vertexStage);
                stageOut << "    float " << v << " = FlareSgNoise((" << uv.expr << ") * (" << sc.expr << "));\n";
                portExprs[{n.id, "Out"}] = {v, T::Float};
            }
            else if (n.type == "GradientNoise")
            {
                const auto uv = resolveInput(n, spec->inputs[0], T::Vec2, vertexStage);
                const auto sc = resolveInput(n, spec->inputs[1], T::Float, vertexStage);
                stageOut << "    float " << v << " = FlareSgGradNoise((" << uv.expr << ") * (" << sc.expr << "));\n";
                portExprs[{n.id, "Out"}] = {v, T::Float};
            }
            else if (n.type == "FbmNoise")
            {
                const auto uv = resolveInput(n, spec->inputs[0], T::Vec2, vertexStage);
                const auto sc = resolveInput(n, spec->inputs[1], T::Float, vertexStage);
                const auto det = resolveInput(n, spec->inputs[2], T::Float, vertexStage);
                stageOut << "    float " << v << " = FlareSgFbm((" << uv.expr << ") * (" << sc.expr << "), " << det.expr << ");\n";
                portExprs[{n.id, "Out"}] = {v, T::Float};
            }
            else if (n.type == "Voronoi")
            {
                const auto uv = resolveInput(n, spec->inputs[0], T::Vec2, vertexStage);
                const auto sc = resolveInput(n, spec->inputs[1], T::Float, vertexStage);
                stageOut << "    float2 " << v << " = FlareSgVoronoi((" << uv.expr << ") * (" << sc.expr << "));\n";
                portExprs[{n.id, "Out"}] = {v + ".x", T::Float};
                portExprs[{n.id, "CellId"}] = {v + ".y", T::Float};
            }
            else if (n.type == "Checkerboard")
            {
                const auto uv = resolveInput(n, spec->inputs[0], T::Vec2, vertexStage);
                const auto ca = resolveInput(n, spec->inputs[1], T::Vec3, vertexStage);
                const auto cb = resolveInput(n, spec->inputs[2], T::Vec3, vertexStage);
                const auto fq = resolveInput(n, spec->inputs[3], T::Vec2, vertexStage);
                stageOut << "    float2 " << v << "_p = floor((" << uv.expr << ") * (" << fq.expr << ") * 2.0);\n";
                stageOut << "    float " << v << "_m = frac((" << v << "_p.x + " << v << "_p.y) * 0.5) * 2.0;\n";
                stageOut << "    float3 " << v << " = lerp(" << ca.expr << ", " << cb.expr << ", step(0.5, " << v << "_m));\n";
                portExprs[{n.id, "Out"}] = {v, T::Vec3};
            }
            else if (n.type == "GridPattern")
            {
                const auto uv = resolveInput(n, spec->inputs[0], T::Vec2, vertexStage);
                const auto til = resolveInput(n, spec->inputs[1], T::Vec2, vertexStage);
                const auto th = resolveInput(n, spec->inputs[2], T::Float, vertexStage);
                stageOut << "    float2 " << v << "_f = abs(frac((" << uv.expr << ") * (" << til.expr << ")) - 0.5);\n";
                stageOut << "    float " << v << "_d = 0.5 - max(" << v << "_f.x, " << v << "_f.y);\n";
                stageOut << "    float " << v << "_ln = 1.0 - smoothstep((" << th.expr << ") * 0.5, (" << th.expr << ") * 0.5 + 0.015, " << v << "_d);\n";
                portExprs[{n.id, "Line"}] = {v + "_ln", T::Float};
                portExprs[{n.id, "Cell"}] = {"(1.0 - " + v + "_ln)", T::Float};
            }
            else if (n.type == "BrickPattern")
            {
                const auto uv = resolveInput(n, spec->inputs[0], T::Vec2, vertexStage);
                const auto til = resolveInput(n, spec->inputs[1], T::Vec2, vertexStage);
                const auto gap = resolveInput(n, spec->inputs[2], T::Float, vertexStage);
                stageOut << "    float2 " << v << "_st = (" << uv.expr << ") * (" << til.expr << ");\n";
                stageOut << "    " << v << "_st.x += step(1.0, frac(" << v << "_st.y * 0.5) * 2.0) * 0.5;\n";
                stageOut << "    float2 " << v << "_id = floor(" << v << "_st), " << v << "_f = frac(" << v << "_st);\n";
                stageOut << "    float2 " << v << "_b = smoothstep(0.0, max(" << gap.expr << ", 0.005), " << v << "_f) * smoothstep(0.0, max(" << gap.expr << ", 0.005), 1.0 - " << v << "_f);\n";
                stageOut << "    float " << v << " = " << v << "_b.x * " << v << "_b.y;\n";
                stageOut << "    float " << v << "_rid = FlareSgHash21(" << v << "_id);\n";
                portExprs[{n.id, "Brick"}] = {v, T::Float};
                portExprs[{n.id, "BrickId"}] = {v + "_rid", T::Float};
            }
            else if (n.type == "EllipseShape")
            {
                const auto uv = resolveInput(n, spec->inputs[0], T::Vec2, vertexStage);
                const auto w = resolveInput(n, spec->inputs[1], T::Float, vertexStage);
                const auto h = resolveInput(n, spec->inputs[2], T::Float, vertexStage);
                const auto fea = resolveInput(n, spec->inputs[3], T::Float, vertexStage);
                stageOut << "    float " << v << "_d = length(((" << uv.expr << ") * 2.0 - 1.0) / max(float2(" << w.expr << ", " << h.expr << "), 1e-4));\n";
                stageOut << "    float " << v << " = 1.0 - smoothstep(1.0 - max(" << fea.expr << ", 0.002), 1.0 + max(" << fea.expr << ", 0.002), " << v << "_d);\n";
                portExprs[{n.id, "Out"}] = {v, T::Float};
            }
            else if (n.type == "RectangleShape")
            {
                const auto uv = resolveInput(n, spec->inputs[0], T::Vec2, vertexStage);
                const auto w = resolveInput(n, spec->inputs[1], T::Float, vertexStage);
                const auto h = resolveInput(n, spec->inputs[2], T::Float, vertexStage);
                const auto rad = resolveInput(n, spec->inputs[3], T::Float, vertexStage);
                stageOut << "    float " << v << "_r = clamp(" << rad.expr << ", 0.0, min(" << w.expr << ", " << h.expr << ") * 0.5);\n";
                stageOut << "    float2 " << v << "_q = abs((" << uv.expr << ") - 0.5) - (float2(" << w.expr << ", " << h.expr << ") * 0.5 - " << v << "_r);\n";
                stageOut << "    float " << v << "_sd = length(max(" << v << "_q, 0.0)) + min(max(" << v << "_q.x, " << v << "_q.y), 0.0) - " << v << "_r;\n";
                stageOut << "    float " << v << " = 1.0 - smoothstep(-0.01, 0.01, " << v << "_sd);\n";
                portExprs[{n.id, "Out"}] = {v, T::Float};
            }
            else if (n.type == "PolygonShape")
            {
                const auto uv = resolveInput(n, spec->inputs[0], T::Vec2, vertexStage);
                const auto sd = resolveInput(n, spec->inputs[1], T::Float, vertexStage);
                const auto w = resolveInput(n, spec->inputs[2], T::Float, vertexStage);
                const auto h = resolveInput(n, spec->inputs[3], T::Float, vertexStage);
                stageOut << "    float " << v << " = FlareSgPolygon(" << uv.expr << ", " << sd.expr << ", " << w.expr << ", " << h.expr << ");\n";
                portExprs[{n.id, "Out"}] = {v, T::Float};
            }
            else if (n.type == "SphereMask")
            {
                const auto p = resolveInput(n, spec->inputs[0], T::Vec3, vertexStage);
                const auto ctr = resolveInput(n, spec->inputs[1], T::Vec3, vertexStage);
                const auto rad = resolveInput(n, spec->inputs[2], T::Float, vertexStage);
                const auto hrd = resolveInput(n, spec->inputs[3], T::Float, vertexStage);
                stageOut << "    float " << v << "_d = distance(" << p.expr << ", " << ctr.expr << ") / max(" << rad.expr << ", 1e-4);\n";
                stageOut << "    float " << v << " = saturate((1.0 - " << v << "_d) / max(1.0 - clamp(" << hrd.expr << ", 0.0, 0.999), 1e-4));\n";
                portExprs[{n.id, "Out"}] = {v, T::Float};
            }
            else if (n.type == "WaveRing")
            {
                const auto uv = resolveInput(n, spec->inputs[0], T::Vec2, vertexStage);
                const auto ctr = resolveInput(n, spec->inputs[1], T::Vec2, vertexStage);
                const auto fq = resolveInput(n, spec->inputs[2], T::Float, vertexStage);
                const auto spd = resolveInput(n, spec->inputs[3], T::Float, vertexStage);
                stageOut << "    float " << v << " = sin(distance(" << uv.expr << ", " << ctr.expr << ") * (" << fq.expr << ") * 6.2831853 - IN.time.x * (" << spd.expr << ")) * 0.5 + 0.5;\n";
                portExprs[{n.id, "Out"}] = {v, T::Float};
            }
            // ---- 9. Artistic, Stylized & Logic ----
            else if (n.type == "Fresnel")
            {
                const auto p = resolveInput(n, spec->inputs[0], T::Float, vertexStage);
                stageOut << "    float " << v << " = pow(max(saturate(1.0 - dot(IN.worldNormal, IN.viewDir)), 1e-5), max(" << p.expr << ", 0.05));\n";
                portExprs[{n.id, "Out"}] = {v, T::Float};
            }
            else if (n.type == "RimLight")
            {
                const auto col = resolveInput(n, spec->inputs[0], T::Vec3, vertexStage);
                const auto p = resolveInput(n, spec->inputs[1], T::Float, vertexStage);
                const auto thr = resolveInput(n, spec->inputs[2], T::Float, vertexStage);
                const auto sft = resolveInput(n, spec->inputs[3], T::Float, vertexStage);
                stageOut << "    float " << v << "_f = pow(max(saturate(1.0 - dot(IN.worldNormal, IN.viewDir)), 1e-5), max(" << p.expr << ", 0.05));\n";
                stageOut << "    float " << v << "_l = smoothstep((" << thr.expr << ") - max(" << sft.expr << ", 0.01), (" << thr.expr << ") + max(" << sft.expr << ", 0.01), dot(IN.worldNormal, IN.lightDir) * 0.5 + 0.5);\n";
                stageOut << "    float " << v << "_m = " << v << "_f * " << v << "_l;\n";
                portExprs[{n.id, "RGB"}] = {"((" + col.expr + ") * " + v + "_m)", T::Vec3};
                portExprs[{n.id, "Mask"}] = {v + "_m", T::Float};
            }
            else if (n.type == "HalfLambert")
            {
                const auto nrm = resolveInput(n, spec->inputs[0], T::Vec3, vertexStage);
                const auto wrp = resolveInput(n, spec->inputs[1], T::Float, vertexStage);
                stageOut << "    float " << v << "_nl = dot(UnitVector(" << nrm.expr << ", float3(0, 1, 0)), IN.lightDir);\n";
                stageOut << "    float " << v << "_w = clamp(" << wrp.expr << ", 0.0, 1.0);\n";
                stageOut << "    float " << v << "_hl = saturate((" << v << "_nl + " << v << "_w) / (1.0 + " << v << "_w));\n";
                portExprs[{n.id, "HalfLambert"}] = {v + "_hl", T::Float};
                portExprs[{n.id, "NdotL"}] = {"saturate(" + v + "_nl)", T::Float};
            }
            else if (n.type == "CelShadeBand")
            {
                const auto x = resolveInput(n, spec->inputs[0], T::Float, vertexStage);
                const auto thr = resolveInput(n, spec->inputs[1], T::Float, vertexStage);
                const auto sm = resolveInput(n, spec->inputs[2], T::Float, vertexStage);
                stageOut << "    float " << v << "_s = max(" << sm.expr << ", 0.001);\n";
                stageOut << "    float " << v << " = smoothstep((" << thr.expr << ") - " << v << "_s, (" << thr.expr << ") + " << v << "_s, " << x.expr << ");\n";
                portExprs[{n.id, "Out"}] = {v, T::Float};
            }
            else if (n.type == "SpecularStylized")
            {
                const auto nrm = resolveInput(n, spec->inputs[0], T::Vec3, vertexStage);
                const auto gls = resolveInput(n, spec->inputs[1], T::Float, vertexStage);
                const auto hrd = resolveInput(n, spec->inputs[2], T::Float, vertexStage);
                stageOut << "    float3 " << v << "_h = UnitVector(IN.lightDir + IN.viewDir, float3(0, 1, 0));\n";
                stageOut << "    float " << v << "_nh = pow(max(saturate(dot(UnitVector(" << nrm.expr << ", float3(0, 1, 0)), " << v << "_h)), 1e-5), exp2(2.0 + saturate(" << gls.expr << ") * 7.0));\n";
                stageOut << "    float " << v << "_f = max(1.0 - saturate(" << hrd.expr << "), 0.01);\n";
                stageOut << "    float " << v << " = smoothstep(0.5 - " << v << "_f * 0.45, 0.5 + " << v << "_f * 0.45, " << v << "_nh);\n";
                portExprs[{n.id, "Out"}] = {v, T::Float};
            }
            else if (n.type == "MatCapUV")
            {
                const auto nrm = resolveInput(n, spec->inputs[0], T::Vec3, vertexStage);
                stageOut << "    float3 " << v << "_n = UnitVector(" << nrm.expr << ", float3(0, 1, 0));\n";
                stageOut << "    float3 " << v << "_r = UnitVector(cross(float3(0, 1, 0), IN.viewDir), float3(1, 0, 0));\n";
                stageOut << "    float3 " << v << "_u = cross(IN.viewDir, " << v << "_r);\n";
                stageOut << "    float2 " << v << " = float2(dot(" << v << "_r, " << v << "_n), -dot(" << v << "_u, " << v << "_n)) * 0.49 + 0.5;\n";
                portExprs[{n.id, "UV"}] = {v, T::Vec2};
            }
            else if (n.type == "DissolveEdge")
            {
                const auto ns = resolveInput(n, spec->inputs[0], T::Float, vertexStage);
                const auto prg = resolveInput(n, spec->inputs[1], T::Float, vertexStage);
                const auto ew = resolveInput(n, spec->inputs[2], T::Float, vertexStage);
                const auto ecol = resolveInput(n, spec->inputs[3], T::Vec3, vertexStage);
                stageOut << "    float " << v << "_d = (" << ns.expr << ") - (" << prg.expr << ");\n";
                stageOut << "    float " << v << "_a = step(0.0, " << v << "_d);\n";
                stageOut << "    float " << v << "_e = " << v << "_a * (1.0 - smoothstep(0.0, max(" << ew.expr << ", 0.002), " << v << "_d));\n";
                portExprs[{n.id, "Alpha"}] = {v + "_a", T::Float};
                portExprs[{n.id, "EdgeMask"}] = {v + "_e", T::Float};
                portExprs[{n.id, "Emissive"}] = {"((" + ecol.expr + ") * (" + v + "_e * 3.0))", T::Vec3};
            }
            else if (n.type == "DitherThreshold")
            {
                const auto op = resolveInput(n, spec->inputs[0], T::Float, vertexStage);
                stageOut << "    float " << v << "_b = FlareBayer4x4(IN.pixelPos);\n";
                stageOut << "    float " << v << " = step(" << v << "_b, saturate(" << op.expr << "));\n";
                portExprs[{n.id, "ClipAlpha"}] = {v, T::Float};
                portExprs[{n.id, "Bayer"}] = {v + "_b", T::Float};
            }
            else if (n.type == "HeightFogMask")
            {
                const auto hy = resolveInput(n, spec->inputs[0], T::Float, vertexStage);
                const auto by = resolveInput(n, spec->inputs[1], T::Float, vertexStage);
                const auto fo = resolveInput(n, spec->inputs[2], T::Float, vertexStage);
                stageOut << "    float " << v << " = saturate(((" << hy.expr << ") - (" << by.expr << ")) / max(" << fo.expr << ", 1e-4));\n";
                portExprs[{n.id, "Out"}] = {v, T::Float};
            }
            else if (n.type == "VertexWindWave")
            {
                const auto amp = resolveInput(n, spec->inputs[0], T::Vec3, vertexStage);
                const auto fq = resolveInput(n, spec->inputs[1], T::Float, vertexStage);
                const auto spd = resolveInput(n, spec->inputs[2], T::Float, vertexStage);
                stageOut << "    float " << v << "_w = sin((IN.localPos.x + IN.localPos.z) * (" << fq.expr << ") + IN.time.x * (" << spd.expr << ")) * saturate(IN.localPos.y + 0.5);\n";
                stageOut << "    float3 " << v << " = (" << amp.expr << ") * " << v << "_w;\n";
                portExprs[{n.id, "Offset"}] = {v, T::Vec3};
            }
            else if (n.type == "CompareIf")
            {
                const auto a = resolveInput(n, spec->inputs[0], T::Float, vertexStage);
                const auto b = resolveInput(n, spec->inputs[1], T::Float, vertexStage);
                const auto gt = resolveRawInput(n, spec->inputs[2]);
                const auto eq = resolveRawInput(n, spec->inputs[3]);
                const auto lt = resolveRawInput(n, spec->inputs[4]);
                const T outT = WiderType(WiderType(gt.type, eq.type), lt.type);
                stageOut << "    float " << v << "_da = (" << a.expr << ") - (" << b.expr << ");\n";
                stageOut << "    " << HlslTypeName(outT) << " " << v << " = abs(" << v << "_da) < 1e-5 ? ("
                         << CastHlslExpr(eq.expr, eq.type, outT) << ") : (" << v << "_da > 0.0 ? ("
                         << CastHlslExpr(gt.expr, gt.type, outT) << ") : ("
                         << CastHlslExpr(lt.expr, lt.type, outT) << "));\n";
                portExprs[{n.id, "Out"}] = {v, outT};
            }
            else if (n.type == "StaticSwitch")
            {
                const auto cond = resolveInput(n, spec->inputs[0], T::Float, vertexStage);
                const auto tr = resolveRawInput(n, spec->inputs[1]);
                const auto fa = resolveRawInput(n, spec->inputs[2]);
                const T outT = WiderType(tr.type, fa.type);
                stageOut << "    " << HlslTypeName(outT) << " " << v << " = (" << cond.expr << ") >= 0.5 ? ("
                         << CastHlslExpr(tr.expr, tr.type, outT) << ") : ("
                         << CastHlslExpr(fa.expr, fa.type, outT) << ");\n";
                portExprs[{n.id, "Out"}] = {v, outT};
            }
        }
    };

    const ShaderNodeTypeSpec* masterSpec = FindShaderNodeSpec("MasterSurface");
    std::ostringstream body;
    if (const auto* offsetLink = findLinkTo(master->id, "WorldOffset"); offsetLink != nullptr && nodeById.contains(offsetLink->fromNode))
    {
        std::vector<std::uint32_t> vertexOrder;
        if (!TopoSortGraph(graph, offsetLink->fromNode, vertexOrder, errorMessage, true)) return false;
        body << "\nvoid FlareVertex(inout FlareVertexInput V)\n{\n"
             << "    FlareSurfaceInput IN = (FlareSurfaceInput)0;\n"
             << "    IN.uv = V.uv;\n"
             << "    IN.localPos = V.localPos;\n"
             << "    IN.worldPos = V.localPos;\n"
             << "    IN.cameraPos = float3(0.0, 0.0, 5.0);\n"
             << "    IN.worldNormal = V.localNormal;\n"
             << "    IN.worldTangent = float3(1.0, 0.0, 0.0);\n"
             << "    IN.worldBitangent = float3(0.0, 0.0, 1.0);\n"
             << "    IN.viewDir = float3(0.0, 0.0, 1.0);\n"
             << "    IN.lightDir = float3(0.35, 0.85, 0.38);\n"
             << "    IN.lightColor = float3(1.0, 1.0, 1.0);\n"
             << "    IN.vertexColor = float4(1.0, 1.0, 1.0, 1.0);\n"
             << "    IN.screenUv = V.uv;\n"
             << "    IN.screenPos = V.uv;\n"
             << "    IN.pixelPos = V.uv * 512.0;\n"
             << "    IN.facing = 1.0;\n"
             << "    IN.time = V.time;\n";
        emitStageNodes(body, vertexOrder, true);
        const auto offsetVal = resolveInput(*master, masterSpec->inputs[7], ShaderPortValueType::Vec3, true);
        body << "    V.localPos += " << offsetVal.expr << ";\n}\n";
    }

    body << "\nvoid FlareSurface(in FlareSurfaceInput IN, inout FlareSurfaceOutput S)\n{\n";
    emitStageNodes(body, order, false);
    const auto emitMasterAssign = [&](std::size_t portIndex, const char* member, ShaderPortValueType targetType) {
        const auto& ps = masterSpec->inputs[portIndex];
        if (findLinkTo(master->id, ps.id) != nullptr || master->inputDefaults.contains(ps.id))
        {
            const auto val = resolveInput(*master, ps, targetType, false);
            body << "    S." << member << " = " << val.expr << ";\n";
        }
    };
    emitMasterAssign(0, "baseColor", ShaderPortValueType::Vec3);
    emitMasterAssign(1, "metallic", ShaderPortValueType::Float);
    emitMasterAssign(2, "roughness", ShaderPortValueType::Float);
    emitMasterAssign(3, "normalTS", ShaderPortValueType::Vec3);
    emitMasterAssign(4, "emissive", ShaderPortValueType::Vec3);
    emitMasterAssign(5, "opacity", ShaderPortValueType::Float);
    emitMasterAssign(6, "alphaCutoff", ShaderPortValueType::Float);
    body << "}\n";

    const std::string generated = hdr.str() + helpers.str() + body.str() + "\n        ENDHLSL\n    }\n}\n";
    SurfaceShaderMetadata validateMeta;
    if (!ParseSurfaceShaderSource(generated, validateMeta, errorMessage))
    {
        return false;
    }
    outHlsl = std::move(generated);
    return true;
}

ShaderPreviewImage RenderShaderGraphPreviewSphere(
    const ShaderGraphDocument& graph,
    float timeSeconds,
    float lightYawRadians,
    float lightPitchRadians,
    const std::map<std::string, Vec4>& materialOverrides,
    std::uint32_t size)
{
    ShaderPreviewImage img;
    img.width = std::clamp(size, 32u, 256u);
    img.height = img.width;
    img.rgba8.assign(static_cast<std::size_t>(img.width) * img.height * 4, 0);

    const ShaderGraphNode* master = nullptr;
    std::unordered_map<std::uint32_t, const ShaderGraphNode*> nodeById;
    for (const auto& n : graph.nodes)
    {
        nodeById[n.id] = &n;
        if (n.type == "MasterSurface") master = &n;
    }
    std::vector<std::uint32_t> order;
    std::string dummyErr;
    if (!master || !TopoSortGraph(graph, master->id, order, dummyErr))
    {
        return img;
    }

    std::unordered_map<std::uint32_t, std::string> exposedIdent;
    {
        std::set<std::string> usedIdents;
        for (std::uint32_t nid : order)
        {
            const ShaderGraphNode& n = *nodeById[nid];
            if (!n.exposeAsParam) continue;
            std::string ident = SanitizeParamIdent(n.paramName, n.id);
            while (usedIdents.contains(ident)) ident += "_" + std::to_string(n.id);
            usedIdents.insert(ident);
            exposedIdent[nid] = std::move(ident);
        }
    }

    struct LinkRef
    {
        std::uint32_t fromNode = 0;
        std::string fromPort;
    };
    std::map<std::pair<std::uint32_t, std::string>, LinkRef> inLinks;
    for (const auto& l : graph.links)
    {
        inLinks[{l.toNode, l.toPort}] = {l.fromNode, l.fromPort};
    }

    const Vec3 lightDir = Normalize(Vec3{
        std::sin(lightYawRadians) * std::cos(lightPitchRadians),
        std::sin(lightPitchRadians),
        std::cos(lightYawRadians) * std::cos(lightPitchRadians)});
    const Vec3 viewDir{0.0f, 0.0f, 1.0f};
    const Vec4 timeVec{timeSeconds, std::sin(timeSeconds), std::cos(timeSeconds), 1.0f / 60.0f};
    constexpr float kPi = 3.1415926535f;

    struct PortVal
    {
        Vec4 v{};
        ShaderPortValueType t = ShaderPortValueType::Float;
    };
    std::map<std::pair<std::uint32_t, std::string>, PortVal> vals;

    const ShaderNodeTypeSpec* masterSpec = FindShaderNodeSpec("MasterSurface");

    for (std::uint32_t y = 0; y < img.height; ++y)
    {
        for (std::uint32_t x = 0; x < img.width; ++x)
        {
            const std::size_t pxOffset = (static_cast<std::size_t>(y) * img.width + x) * 4;
            const bool checker = (((x / 10) + (y / 10)) & 1) != 0;
            const Vec3 bgCol = checker ? Vec3{0.055f, 0.06f, 0.072f} : Vec3{0.032f, 0.035f, 0.044f};

            const float sx = ((static_cast<float>(x) + 0.5f) / img.width * 2.0f - 1.0f) * 1.12f;
            const float sy = (1.0f - (static_cast<float>(y) + 0.5f) / img.height * 2.0f) * 1.12f;
            const float r2 = sx * sx + sy * sy;
            if (r2 > 1.0f)
            {
                img.rgba8[pxOffset + 0] = ToSrgbByte(bgCol.x);
                img.rgba8[pxOffset + 1] = ToSrgbByte(bgCol.y);
                img.rgba8[pxOffset + 2] = ToSrgbByte(bgCol.z);
                img.rgba8[pxOffset + 3] = 255;
                continue;
            }

            const float edgeAa = std::clamp((1.0f - std::sqrt(r2)) * (img.width * 0.45f), 0.0f, 1.0f);
            const float sz = std::sqrt(std::max(0.0f, 1.0f - r2));
            const Vec3 normal{sx, sy, sz};
            const Vec3 tangent = Normalize(std::abs(normal.y) < 0.99f ? Cross(Vec3{0.0f, 1.0f, 0.0f}, normal) : Vec3{1.0f, 0.0f, 0.0f});
            const Vec3 bitangent = Cross(normal, tangent);
            const float u = 0.5f + std::atan2(sx, sz) / (2.0f * kPi);
            const float v = 0.5f - std::asin(std::clamp(sy, -1.0f, 1.0f)) / kPi;

            vals.clear();
            const auto getIn = [&](const ShaderGraphNode& node, const ShaderNodePortSpec& ps,
                                   ShaderPortValueType desired) -> Vec4 {
                const auto lit = inLinks.find({node.id, ps.id});
                if (lit != inLinks.end())
                {
                    const auto vit = vals.find({lit->second.fromNode, lit->second.fromPort});
                    if (vit != vals.end()) return CastCpuValue(vit->second.v, vit->second.t, desired);
                }
                if (ps.id == "UV" && ps.type == ShaderPortValueType::Vec2 && !node.inputDefaults.contains("UV"))
                    return {u, v, 0.0f, 0.0f};
                if (ps.id == "Time" && ps.type == ShaderPortValueType::Float && ps.defaultValue.x < 0.0f &&
                    !node.inputDefaults.contains("Time"))
                    return {timeVec.x, 0, 0, 0};
                if (ps.id == "NormalWS" && ps.type == ShaderPortValueType::Vec3 && !node.inputDefaults.contains("NormalWS"))
                    return {normal.x, normal.y, normal.z, 0.0f};
                if (ps.id == "CoordsWS" && ps.type == ShaderPortValueType::Vec3 && !node.inputDefaults.contains("CoordsWS"))
                    return {normal.x, normal.y, normal.z, 0.0f};
                if (ps.id == "HeightWS" && ps.type == ShaderPortValueType::Float && !node.inputDefaults.contains("HeightWS"))
                    return {normal.y, 0.0f, 0.0f, 0.0f};
                const auto dit = node.inputDefaults.find(ps.id);
                return CastCpuValue(dit != node.inputDefaults.end() ? dit->second : ps.defaultValue, ps.type, desired);
            };

            const auto getRaw = [&](const ShaderGraphNode& node, const ShaderNodePortSpec& ps) -> PortVal {
                const auto lit = inLinks.find({node.id, ps.id});
                if (lit != inLinks.end())
                {
                    const auto vit = vals.find({lit->second.fromNode, lit->second.fromPort});
                    if (vit != vals.end()) return vit->second;
                }
                const auto dit = node.inputDefaults.find(ps.id);
                const Vec4 val = dit != node.inputDefaults.end() ? dit->second : ps.defaultValue;
                const ShaderPortValueType rawType = HasUniformComponents(val, ps.type)
                    ? ShaderPortValueType::Float
                    : ps.type;
                return {val, rawType};
            };

            using T = ShaderPortValueType;
            for (std::uint32_t nid : order)
            {
                const ShaderGraphNode& n = *nodeById[nid];
                const ShaderNodeTypeSpec* spec = FindShaderNodeSpec(n.type);
                if (!spec) continue;

                Vec4 propVal = n.constantValue;
                if (n.exposeAsParam)
                {
                    const auto idIt = exposedIdent.find(n.id);
                    if (idIt != exposedIdent.end())
                    {
                        const auto oit = materialOverrides.find(idIt->second);
                        if (oit != materialOverrides.end()) propVal = oit->second;
                    }
                }

                // ---- 1. Inputs ----
                if (n.type == "UV")
                {
                    vals[{n.id, "Out"}] = {{u, v, 0, 0}, T::Vec2};
                    vals[{n.id, "U"}] = {{u, 0, 0, 0}, T::Float};
                    vals[{n.id, "V"}] = {{v, 0, 0, 0}, T::Float};
                }
                else if (n.type == "Time")
                {
                    vals[{n.id, "Time"}] = {{timeVec.x, 0, 0, 0}, T::Float};
                    vals[{n.id, "SinTime"}] = {{timeVec.y, 0, 0, 0}, T::Float};
                    vals[{n.id, "CosTime"}] = {{timeVec.z, 0, 0, 0}, T::Float};
                    vals[{n.id, "Delta"}] = {{timeVec.w, 0, 0, 0}, T::Float};
                }
                else if (n.type == "WorldPos" || n.type == "LocalPos")
                {
                    vals[{n.id, "Out"}] = {{normal.x, normal.y, normal.z, 0}, T::Vec3};
                    vals[{n.id, "X"}] = {{normal.x, 0, 0, 0}, T::Float};
                    vals[{n.id, "Y"}] = {{normal.y, 0, 0, 0}, T::Float};
                    vals[{n.id, "Z"}] = {{normal.z, 0, 0, 0}, T::Float};
                }
                else if (n.type == "ObjectPos") vals[{n.id, "Out"}] = {{0, 0, 0, 0}, T::Vec3};
                else if (n.type == "CameraPos") vals[{n.id, "Out"}] = {{0, 0, 3.0f, 0}, T::Vec3};
                else if (n.type == "CameraDistance")
                {
                    const float d = std::hypot(normal.x, std::hypot(normal.y, normal.z - 3.0f));
                    vals[{n.id, "Out"}] = {{d, 0, 0, 0}, T::Float};
                }
                else if (n.type == "WorldNormal") vals[{n.id, "Out"}] = {{normal.x, normal.y, normal.z, 0}, T::Vec3};
                else if (n.type == "WorldTangent") vals[{n.id, "Out"}] = {{tangent.x, tangent.y, tangent.z, 0}, T::Vec3};
                else if (n.type == "WorldBitangent") vals[{n.id, "Out"}] = {{bitangent.x, bitangent.y, bitangent.z, 0}, T::Vec3};
                else if (n.type == "ViewDir") vals[{n.id, "Out"}] = {{viewDir.x, viewDir.y, viewDir.z, 0}, T::Vec3};
                else if (n.type == "LightDir") vals[{n.id, "Out"}] = {{lightDir.x, lightDir.y, lightDir.z, 0}, T::Vec3};
                else if (n.type == "LightColor")
                {
                    vals[{n.id, "RGB"}] = {{1.0f, 1.0f, 1.0f, 0}, T::Vec3};
                    vals[{n.id, "Intensity"}] = {{1.0f, 0, 0, 0}, T::Float};
                }
                else if (n.type == "ScreenUV")
                {
                    vals[{n.id, "Out"}] = {{float(x) / img.width, float(y) / img.height, 0, 0}, T::Vec2};
                }
                else if (n.type == "Facing")
                {
                    vals[{n.id, "Out"}] = {{1.0f, 0, 0, 0}, T::Float};
                    vals[{n.id, "Front"}] = {{1.0f, 0, 0, 0}, T::Float};
                }
                else if (n.type == "VertexColor")
                {
                    vals[{n.id, "RGBA"}] = {{1, 1, 1, 1}, T::Vec4};
                    vals[{n.id, "RGB"}] = {{1, 1, 1, 0}, T::Vec3};
                    vals[{n.id, "A"}] = {{1, 0, 0, 0}, T::Float};
                }
                // ---- 2. Properties & Constants ----
                else if (n.type == "Float" || n.type == "Range")
                {
                    const float fv = (n.type == "Range") ? std::clamp(propVal.x, n.rangeMin, n.rangeMax) : propVal.x;
                    vals[{n.id, "Out"}] = {{fv, 0, 0, 0}, T::Float};
                }
                else if (n.type == "Boolean")
                {
                    vals[{n.id, "Out"}] = {{propVal.x >= 0.5f ? 1.0f : 0.0f, 0, 0, 0}, T::Float};
                }
                else if (n.type == "Color")
                {
                    vals[{n.id, "RGBA"}] = {propVal, T::Vec4};
                    vals[{n.id, "RGB"}] = {{propVal.x, propVal.y, propVal.z, 0}, T::Vec3};
                    vals[{n.id, "A"}] = {{propVal.w, 0, 0, 0}, T::Float};
                }
                else if (n.type == "Vector2") vals[{n.id, "Out"}] = {propVal, T::Vec2};
                else if (n.type == "Vector3") vals[{n.id, "Out"}] = {propVal, T::Vec3};
                else if (n.type == "Vector4") vals[{n.id, "Out"}] = {propVal, T::Vec4};
                else if (n.type == "PI")
                {
                    vals[{n.id, "PI"}] = {{3.14159265f, 0, 0, 0}, T::Float};
                    vals[{n.id, "TAU"}] = {{6.28318531f, 0, 0, 0}, T::Float};
                    vals[{n.id, "HalfPI"}] = {{1.57079633f, 0, 0, 0}, T::Float};
                    vals[{n.id, "InvPI"}] = {{0.31830989f, 0, 0, 0}, T::Float};
                }
                else if (n.type == "GoldenRatio")
                {
                    vals[{n.id, "Phi"}] = {{1.61803399f, 0, 0, 0}, T::Float};
                    vals[{n.id, "E"}] = {{2.71828183f, 0, 0, 0}, T::Float};
                    vals[{n.id, "Sqrt2"}] = {{1.41421356f, 0, 0, 0}, T::Float};
                }
                // ---- 3. Texture & Normal ----
                else if (n.type == "BaseColorMap")
                {
                    const Vec4 uvIn = getIn(n, spec->inputs[0], T::Vec2);
                    const bool grid = ((int(std::floor(uvIn.x * 8.0f)) + int(std::floor(uvIn.y * 8.0f))) & 1) != 0;
                    const float c = grid ? 0.86f : 0.68f;
                    vals[{n.id, "RGBA"}] = {{c, c, c, 1.0f}, T::Vec4};
                    vals[{n.id, "RGB"}] = {{c, c, c, 0.0f}, T::Vec3};
                    vals[{n.id, "A"}] = {{1.0f, 0, 0, 0}, T::Float};
                }
                else if (n.type == "NormalMap")
                {
                    vals[{n.id, "NormalTS"}] = {{0.0f, 0.0f, 1.0f, 0.0f}, T::Vec3};
                }
                else if (n.type == "MetallicRoughnessMap")
                {
                    vals[{n.id, "Metallic"}] = {{0.1f, 0, 0, 0}, T::Float};
                    vals[{n.id, "Roughness"}] = {{0.38f, 0, 0, 0}, T::Float};
                }
                else if (n.type == "NormalStrength")
                {
                    const Vec4 inN = getIn(n, spec->inputs[0], T::Vec3);
                    const float str = getIn(n, spec->inputs[1], T::Float).x;
                    const Vec3 uN = Normalize(Vec3{inN.x * str, inN.y * str, std::max(inN.z, 1e-4f)});
                    vals[{n.id, "Out"}] = {{uN.x, uN.y, uN.z, 0}, T::Vec3};
                }
                else if (n.type == "NormalBlend")
                {
                    const Vec4 a = getIn(n, spec->inputs[0], T::Vec3);
                    const Vec4 b = getIn(n, spec->inputs[1], T::Vec3);
                    const Vec3 ta{a.x, a.y, a.z + 1.0f};
                    const Vec3 tb{-b.x, -b.y, b.z};
                    const Vec3 rn = Normalize(ta * Dot(ta, tb) - tb * ta.z);
                    vals[{n.id, "Out"}] = {{rn.x, rn.y, rn.z, 0}, T::Vec3};
                }
                else if (n.type == "NormalFromHeight")
                {
                    const float h = getIn(n, spec->inputs[0], T::Float).x;
                    const float str = getIn(n, spec->inputs[1], T::Float).x;
                    const Vec3 rn = Normalize(Vec3{(0.5f - h) * str * 0.25f, (0.5f - h) * str * 0.25f, 1.0f});
                    vals[{n.id, "Out"}] = {{rn.x, rn.y, rn.z, 0}, T::Vec3};
                }
                else if (n.type == "TriplanarBaseColor")
                {
                    const float sc = getIn(n, spec->inputs[0], T::Float).x;
                    const float sh = std::max(0.5f, getIn(n, spec->inputs[1], T::Float).x);
                    float wx = std::pow(std::max(std::abs(normal.x), 1e-4f), sh);
                    float wy = std::pow(std::max(std::abs(normal.y), 1e-4f), sh);
                    float wz = std::pow(std::max(std::abs(normal.z), 1e-4f), sh);
                    const float ws = std::max(wx + wy + wz, 1e-5f);
                    wx /= ws; wy /= ws; wz /= ws;
                    const auto chk = [](float a, float b) {
                        return (((int(std::floor(a * 6.0f)) + int(std::floor(b * 6.0f))) & 1) != 0) ? 0.86f : 0.66f;
                    };
                    const float c = chk(normal.z * sc, normal.y * sc) * wx +
                                    chk(normal.x * sc, normal.z * sc) * wy +
                                    chk(normal.x * sc, normal.y * sc) * wz;
                    vals[{n.id, "RGB"}] = {{c, c, c, 0}, T::Vec3};
                    vals[{n.id, "A"}] = {{1.0f, 0, 0, 0}, T::Float};
                }
                else if (n.type == "ParallaxOffset")
                {
                    const Vec4 uvIn = getIn(n, spec->inputs[0], T::Vec2);
                    const float h = getIn(n, spec->inputs[1], T::Float).x;
                    const float amp = getIn(n, spec->inputs[2], T::Float).x;
                    const float vtx = Dot(viewDir, tangent), vty = Dot(viewDir, bitangent);
                    const float vtz = std::max(Dot(viewDir, normal), 0.25f);
                    vals[{n.id, "Out"}] = {{uvIn.x + (vtx / vtz) * ((h - 0.5f) * amp),
                                            uvIn.y + (vty / vtz) * ((h - 0.5f) * amp), 0, 0}, T::Vec2};
                }
                else if (n.type == "Flipbook")
                {
                    const Vec4 uvIn = getIn(n, spec->inputs[0], T::Vec2);
                    const float cols = std::max(1.0f, std::floor(getIn(n, spec->inputs[1], T::Float).x));
                    const float rows = std::max(1.0f, std::floor(getIn(n, spec->inputs[2], T::Float).x));
                    const float idx = std::floor(getIn(n, spec->inputs[3], T::Float).x);
                    const float cx = idx - cols * std::floor(idx / cols);
                    float cy = std::floor(idx / cols);
                    cy = cy - rows * std::floor(cy / rows);
                    const float fx = uvIn.x - std::floor(uvIn.x), fy = uvIn.y - std::floor(uvIn.y);
                    vals[{n.id, "Out"}] = {{(fx + cx) / cols, (fy + cy) / rows, 0, 0}, T::Vec2};
                }
                // ---- 4. UV Coordinates ----
                else if (n.type == "TilingOffset")
                {
                    const Vec4 uvIn = getIn(n, spec->inputs[0], T::Vec2);
                    const Vec4 til = getIn(n, spec->inputs[1], T::Vec2);
                    const Vec4 off = getIn(n, spec->inputs[2], T::Vec2);
                    vals[{n.id, "Out"}] = {{uvIn.x * til.x + off.x, uvIn.y * til.y + off.y, 0, 0}, T::Vec2};
                }
                else if (n.type == "Panner")
                {
                    const Vec4 uvIn = getIn(n, spec->inputs[0], T::Vec2);
                    const Vec4 spd = getIn(n, spec->inputs[1], T::Vec2);
                    const Vec4 tm = getIn(n, spec->inputs[2], T::Float);
                    vals[{n.id, "Out"}] = {{uvIn.x + spd.x * tm.x, uvIn.y + spd.y * tm.x, 0, 0}, T::Vec2};
                }
                else if (n.type == "Rotator")
                {
                    const Vec4 uvIn = getIn(n, spec->inputs[0], T::Vec2);
                    const Vec4 ctr = getIn(n, spec->inputs[1], T::Vec2);
                    const float ang = getIn(n, spec->inputs[2], T::Float).x;
                    const float dx = uvIn.x - ctr.x, dy = uvIn.y - ctr.y;
                    const float cs = std::cos(ang), sn = std::sin(ang);
                    vals[{n.id, "Out"}] = {{dx * cs - dy * sn + ctr.x, dx * sn + dy * cs + ctr.y, 0, 0}, T::Vec2};
                }
                else if (n.type == "PolarCoordinates")
                {
                    const Vec4 uvIn = getIn(n, spec->inputs[0], T::Vec2);
                    const Vec4 ctr = getIn(n, spec->inputs[1], T::Vec2);
                    const float rs = getIn(n, spec->inputs[2], T::Float).x;
                    const float ls = getIn(n, spec->inputs[3], T::Float).x;
                    const float dx = uvIn.x - ctr.x, dy = uvIn.y - ctr.y;
                    const float rad = std::hypot(dx, dy) * 2.0f * rs;
                    const float ang = (std::atan2(dy, (std::abs(dx) < 1e-6f && std::abs(dy) < 1e-6f) ? 1e-6f : dx) / 6.2831853f + 0.5f) * ls;
                    vals[{n.id, "Out"}] = {{rad, ang, 0, 0}, T::Vec2};
                    vals[{n.id, "Radius"}] = {{rad, 0, 0, 0}, T::Float};
                    vals[{n.id, "Angle"}] = {{ang, 0, 0, 0}, T::Float};
                }
                else if (n.type == "Twirl")
                {
                    const Vec4 uvIn = getIn(n, spec->inputs[0], T::Vec2);
                    const Vec4 ctr = getIn(n, spec->inputs[1], T::Vec2);
                    const float str = getIn(n, spec->inputs[2], T::Float).x;
                    const Vec4 off = getIn(n, spec->inputs[3], T::Vec2);
                    const float dx = uvIn.x - ctr.x, dy = uvIn.y - ctr.y;
                    const float ang = str * std::hypot(dx, dy);
                    const float cs = std::cos(ang), sn = std::sin(ang);
                    vals[{n.id, "Out"}] = {{dx * cs - dy * sn + ctr.x + off.x, dx * sn + dy * cs + ctr.y + off.y, 0, 0}, T::Vec2};
                }
                else if (n.type == "RadialShear")
                {
                    const Vec4 uvIn = getIn(n, spec->inputs[0], T::Vec2);
                    const Vec4 ctr = getIn(n, spec->inputs[1], T::Vec2);
                    const Vec4 str = getIn(n, spec->inputs[2], T::Vec2);
                    const Vec4 off = getIn(n, spec->inputs[3], T::Vec2);
                    const float dx = uvIn.x - ctr.x, dy = uvIn.y - ctr.y;
                    const float d2 = dx * dx + dy * dy;
                    vals[{n.id, "Out"}] = {{uvIn.x - dy * str.x * d2 + off.x, uvIn.y + dx * str.y * d2 + off.y, 0, 0}, T::Vec2};
                }
                else if (n.type == "Spherize")
                {
                    const Vec4 uvIn = getIn(n, spec->inputs[0], T::Vec2);
                    const Vec4 ctr = getIn(n, spec->inputs[1], T::Vec2);
                    const float str = getIn(n, spec->inputs[2], T::Float).x;
                    const Vec4 off = getIn(n, spec->inputs[3], T::Vec2);
                    const float dx = uvIn.x - ctr.x, dy = uvIn.y - ctr.y;
                    const float d2 = dx * dx + dy * dy;
                    const float f = 1.0f + d2 * d2 * str;
                    vals[{n.id, "Out"}] = {{ctr.x + dx * f + off.x, ctr.y + dy * f + off.y, 0, 0}, T::Vec2};
                }
                else if (n.type == "PixelateUV")
                {
                    const Vec4 uvIn = getIn(n, spec->inputs[0], T::Vec2);
                    const Vec4 cl = getIn(n, spec->inputs[1], T::Vec2);
                    const float cx = std::max(1.0f, std::floor(cl.x)), cy = std::max(1.0f, std::floor(cl.y));
                    vals[{n.id, "Out"}] = {{(std::floor(uvIn.x * cx) + 0.5f) / cx, (std::floor(uvIn.y * cy) + 0.5f) / cy, 0, 0}, T::Vec2};
                }
                else if (n.type == "MirrorUV")
                {
                    const Vec4 uvIn = getIn(n, spec->inputs[0], T::Vec2);
                    const auto mir = [](float t) { const float f = t * 0.5f - std::floor(t * 0.5f); return 1.0f - std::abs(f * 2.0f - 1.0f); };
                    vals[{n.id, "Out"}] = {{mir(uvIn.x), mir(uvIn.y), 0, 0}, T::Vec2};
                }
                else if (n.type == "WorldAlignedUV")
                {
                    const float sc = getIn(n, spec->inputs[0], T::Float).x;
                    vals[{n.id, "XZ"}] = {{normal.x * sc, normal.z * sc, 0, 0}, T::Vec2};
                    vals[{n.id, "XY"}] = {{normal.x * sc, normal.y * sc, 0, 0}, T::Vec2};
                    vals[{n.id, "YZ"}] = {{normal.y * sc, normal.z * sc, 0, 0}, T::Vec2};
                }
                // ---- 5. Math ----
                else if (n.type == "Add" || n.type == "Subtract" || n.type == "Multiply" || n.type == "Divide" ||
                         n.type == "Min" || n.type == "Max" || n.type == "Fmod")
                {
                    const auto ra = getRaw(n, spec->inputs[0]);
                    const auto rb = getRaw(n, spec->inputs[1]);
                    const T outT = WiderType(ra.t, rb.t);
                    const Vec4 a = CastCpuValue(ra.v, ra.t, outT);
                    const Vec4 b = CastCpuValue(rb.v, rb.t, outT);
                    Vec4 r{};
                    const float* pa = &a.x;
                    const float* pb = &b.x;
                    float* pr = &r.x;
                    for (int k = 0; k < 4; ++k)
                    {
                        if (n.type == "Add") pr[k] = pa[k] + pb[k];
                        else if (n.type == "Subtract") pr[k] = pa[k] - pb[k];
                        else if (n.type == "Multiply") pr[k] = pa[k] * pb[k];
                        else if (n.type == "Divide") pr[k] = pa[k] * ((pb[k] >= 0.0f ? 1.0f : -1.0f) / std::max(std::abs(pb[k]), 1e-5f));
                        else if (n.type == "Min") pr[k] = std::min(pa[k], pb[k]);
                        else if (n.type == "Max") pr[k] = std::max(pa[k], pb[k]);
                        else { const float m = std::max(std::abs(pb[k]), 1e-5f); pr[k] = pa[k] - m * std::floor(pa[k] / m); }
                    }
                    vals[{n.id, "Out"}] = {r, outT};
                }
                else if (n.type == "MultiplyAdd")
                {
                    const auto ra = getRaw(n, spec->inputs[0]);
                    const auto rb = getRaw(n, spec->inputs[1]);
                    const auto rc = getRaw(n, spec->inputs[2]);
                    const T outT = WiderType(WiderType(ra.t, rb.t), rc.t);
                    const Vec4 a = CastCpuValue(ra.v, ra.t, outT);
                    const Vec4 b = CastCpuValue(rb.v, rb.t, outT);
                    const Vec4 c = CastCpuValue(rc.v, rc.t, outT);
                    vals[{n.id, "Out"}] = {{a.x * b.x + c.x, a.y * b.y + c.y, a.z * b.z + c.z, a.w * b.w + c.w}, outT};
                }
                else if (n.type == "OneMinus" || n.type == "Negate" || n.type == "Reciprocal" ||
                         n.type == "Abs" || n.type == "Sign" || n.type == "Saturate" ||
                         n.type == "Floor" || n.type == "Ceil" || n.type == "Round" || n.type == "Frac" ||
                         n.type == "Sqrt" || n.type == "Rsqrt")
                {
                    const auto rx = getRaw(n, spec->inputs[0]);
                    Vec4 r = rx.v;
                    float* pr = &r.x;
                    for (int k = 0; k < 4; ++k)
                    {
                        if (n.type == "OneMinus") pr[k] = 1.0f - pr[k];
                        else if (n.type == "Negate") pr[k] = -pr[k];
                        else if (n.type == "Reciprocal") pr[k] = (pr[k] >= 0.0f ? 1.0f : -1.0f) / std::max(std::abs(pr[k]), 1e-5f);
                        else if (n.type == "Abs") pr[k] = std::abs(pr[k]);
                        else if (n.type == "Sign") pr[k] = (pr[k] > 0.0f ? 1.0f : (pr[k] < 0.0f ? -1.0f : 0.0f));
                        else if (n.type == "Saturate") pr[k] = std::clamp(pr[k], 0.0f, 1.0f);
                        else if (n.type == "Floor") pr[k] = std::floor(pr[k]);
                        else if (n.type == "Ceil") pr[k] = std::ceil(pr[k]);
                        else if (n.type == "Round") pr[k] = std::round(pr[k]);
                        else if (n.type == "Frac") pr[k] = pr[k] - std::floor(pr[k]);
                        else if (n.type == "Sqrt") pr[k] = std::sqrt(std::max(pr[k], 0.0f));
                        else pr[k] = 1.0f / std::sqrt(std::max(pr[k], 1e-6f));
                    }
                    vals[{n.id, "Out"}] = {r, rx.t};
                }
                else if (n.type == "Clamp")
                {
                    const auto rx = getRaw(n, spec->inputs[0]);
                    const auto rmn = getRaw(n, spec->inputs[1]);
                    const auto rmx = getRaw(n, spec->inputs[2]);
                    const T outT = WiderType(WiderType(rx.t, rmn.t), rmx.t);
                    const Vec4 xv = CastCpuValue(rx.v, rx.t, outT);
                    const Vec4 mn = CastCpuValue(rmn.v, rmn.t, outT);
                    const Vec4 mx = CastCpuValue(rmx.v, rmx.t, outT);
                    vals[{n.id, "Out"}] = {{std::clamp(xv.x, mn.x, std::max(mn.x, mx.x)),
                                            std::clamp(xv.y, mn.y, std::max(mn.y, mx.y)),
                                            std::clamp(xv.z, mn.z, std::max(mn.z, mx.z)),
                                            std::clamp(xv.w, mn.w, std::max(mn.w, mx.w))}, outT};
                }
                else if (n.type == "Power")
                {
                    const Vec4 b = getIn(n, spec->inputs[0], T::Float);
                    const Vec4 e = getIn(n, spec->inputs[1], T::Float);
                    vals[{n.id, "Out"}] = {{std::pow(std::max(b.x, 1e-5f), e.x), 0, 0, 0}, T::Float};
                }
                else if (n.type == "Exp")
                {
                    const float xv = std::clamp(getIn(n, spec->inputs[0], T::Float).x, -20.0f, 20.0f);
                    vals[{n.id, "Out"}] = {{std::exp(xv), 0, 0, 0}, T::Float};
                    vals[{n.id, "Exp2"}] = {{std::exp2(xv), 0, 0, 0}, T::Float};
                }
                else if (n.type == "Log")
                {
                    const float xv = std::max(getIn(n, spec->inputs[0], T::Float).x, 1e-6f);
                    vals[{n.id, "Out"}] = {{std::log(xv), 0, 0, 0}, T::Float};
                    vals[{n.id, "Log2"}] = {{std::log2(xv), 0, 0, 0}, T::Float};
                }
                else if (n.type == "Lerp")
                {
                    const auto ra = getRaw(n, spec->inputs[0]);
                    const auto rb = getRaw(n, spec->inputs[1]);
                    const T outT = WiderType(ra.t, rb.t);
                    const Vec4 a = CastCpuValue(ra.v, ra.t, outT);
                    const Vec4 b = CastCpuValue(rb.v, rb.t, outT);
                    const float t = getIn(n, spec->inputs[2], T::Float).x;
                    vals[{n.id, "Out"}] = {{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                                            a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t}, outT};
                }
                else if (n.type == "InverseLerp")
                {
                    const float a = getIn(n, spec->inputs[0], T::Float).x;
                    const float b = getIn(n, spec->inputs[1], T::Float).x;
                    const float t = getIn(n, spec->inputs[2], T::Float).x;
                    const float d = std::abs(b - a) < 1e-5f ? 1e-5f : (b - a);
                    vals[{n.id, "Out"}] = {{std::clamp((t - a) / d, 0.0f, 1.0f), 0, 0, 0}, T::Float};
                }
                else if (n.type == "Smoothstep")
                {
                    const float e0 = getIn(n, spec->inputs[0], T::Float).x;
                    const float e1 = std::max(e0 + 1e-4f, getIn(n, spec->inputs[1], T::Float).x);
                    const float t = std::clamp((getIn(n, spec->inputs[2], T::Float).x - e0) / (e1 - e0), 0.0f, 1.0f);
                    vals[{n.id, "Out"}] = {{t * t * (3.0f - 2.0f * t), 0, 0, 0}, T::Float};
                }
                else if (n.type == "Step")
                {
                    const float e = getIn(n, spec->inputs[0], T::Float).x;
                    const float vIn = getIn(n, spec->inputs[1], T::Float).x;
                    vals[{n.id, "Out"}] = {{vIn >= e ? 1.0f : 0.0f, 0, 0, 0}, T::Float};
                }
                else if (n.type == "PingPong")
                {
                    const float xv = getIn(n, spec->inputs[0], T::Float).x;
                    const float ln = std::max(std::abs(getIn(n, spec->inputs[1], T::Float).x), 1e-5f);
                    const float t = xv / (ln * 2.0f);
                    const float f = t - std::floor(t);
                    vals[{n.id, "Out"}] = {{(1.0f - std::abs(f * 2.0f - 1.0f)) * ln, 0, 0, 0}, T::Float};
                }
                else if (n.type == "Sin" || n.type == "Cos" || n.type == "Tan" ||
                         n.type == "Asin" || n.type == "Acos" || n.type == "Radians" || n.type == "Degrees")
                {
                    const float vIn = getIn(n, spec->inputs[0], T::Float).x;
                    float rv = 0.0f;
                    if (n.type == "Sin") rv = std::sin(vIn);
                    else if (n.type == "Cos") rv = std::cos(vIn);
                    else if (n.type == "Tan") rv = std::tan(vIn);
                    else if (n.type == "Asin") rv = std::asin(std::clamp(vIn, -1.0f, 1.0f));
                    else if (n.type == "Acos") rv = std::acos(std::clamp(vIn, -1.0f, 1.0f));
                    else if (n.type == "Radians") rv = vIn * 0.0174532925f;
                    else rv = vIn * 57.2957795f;
                    vals[{n.id, "Out"}] = {{rv, 0, 0, 0}, T::Float};
                }
                else if (n.type == "Atan2")
                {
                    const float yv = getIn(n, spec->inputs[0], T::Float).x;
                    const float xv = getIn(n, spec->inputs[1], T::Float).x;
                    vals[{n.id, "Out"}] = {{std::atan2(yv, (std::abs(xv) < 1e-6f && std::abs(yv) < 1e-6f) ? 1e-6f : xv), 0, 0, 0}, T::Float};
                }
                else if (n.type == "TriangleWave")
                {
                    const float xv = getIn(n, spec->inputs[0], T::Float).x;
                    const float f = xv - std::floor(xv);
                    vals[{n.id, "Out"}] = {{1.0f - std::abs(f * 2.0f - 1.0f), 0, 0, 0}, T::Float};
                }
                else if (n.type == "SquareWave")
                {
                    const float xv = getIn(n, spec->inputs[0], T::Float).x;
                    const float dt = std::clamp(getIn(n, spec->inputs[1], T::Float).x, 0.0f, 1.0f);
                    const float f = xv - std::floor(xv);
                    vals[{n.id, "Out"}] = {{f <= dt ? 1.0f : 0.0f, 0, 0, 0}, T::Float};
                }
                else if (n.type == "SawtoothWave")
                {
                    const float xv = getIn(n, spec->inputs[0], T::Float).x;
                    const float f = xv - std::floor(xv);
                    vals[{n.id, "Out"}] = {{f * 2.0f - 1.0f, 0, 0, 0}, T::Float};
                }
                else if (n.type == "Remap")
                {
                    const float vIn = getIn(n, spec->inputs[0], T::Float).x;
                    const float imin = getIn(n, spec->inputs[1], T::Float).x;
                    const float imax = getIn(n, spec->inputs[2], T::Float).x;
                    const float omin = getIn(n, spec->inputs[3], T::Float).x;
                    const float omax = getIn(n, spec->inputs[4], T::Float).x;
                    const float d = imax - imin;
                    const float t = (vIn - imin) * ((d >= 0.0f ? 1.0f : -1.0f) / std::max(std::abs(d), 1e-5f));
                    vals[{n.id, "Out"}] = {{omin + (omax - omin) * t, 0, 0, 0}, T::Float};
                }
                // ---- 6. Vector & Channel ----
                else if (n.type == "Dot")
                {
                    const Vec4 a = getIn(n, spec->inputs[0], T::Vec3);
                    const Vec4 b = getIn(n, spec->inputs[1], T::Vec3);
                    vals[{n.id, "Out"}] = {{a.x * b.x + a.y * b.y + a.z * b.z, 0, 0, 0}, T::Float};
                }
                else if (n.type == "Cross")
                {
                    const Vec4 a = getIn(n, spec->inputs[0], T::Vec3);
                    const Vec4 b = getIn(n, spec->inputs[1], T::Vec3);
                    const Vec3 c = Cross(Vec3{a.x, a.y, a.z}, Vec3{b.x, b.y, b.z});
                    vals[{n.id, "Out"}] = {{c.x, c.y, c.z, 0}, T::Vec3};
                }
                else if (n.type == "Normalize")
                {
                    const Vec4 a = getIn(n, spec->inputs[0], T::Vec3);
                    const Vec3 uVec = Normalize(Vec3{a.x, a.y, a.z});
                    vals[{n.id, "Out"}] = {{uVec.x, uVec.y, uVec.z, 0}, T::Vec3};
                }
                else if (n.type == "Length")
                {
                    const Vec4 a = getIn(n, spec->inputs[0], T::Vec3);
                    vals[{n.id, "Out"}] = {{std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z), 0, 0, 0}, T::Float};
                }
                else if (n.type == "Distance")
                {
                    const Vec4 a = getIn(n, spec->inputs[0], T::Vec3);
                    const Vec4 b = getIn(n, spec->inputs[1], T::Vec3);
                    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
                    vals[{n.id, "Out"}] = {{std::sqrt(dx * dx + dy * dy + dz * dz), 0, 0, 0}, T::Float};
                }
                else if (n.type == "Reflect")
                {
                    const Vec4 inc4 = getIn(n, spec->inputs[0], T::Vec3);
                    const Vec4 nrm4 = getIn(n, spec->inputs[1], T::Vec3);
                    const Vec3 inc{inc4.x, inc4.y, inc4.z};
                    const Vec3 nrm = Normalize(Vec3{nrm4.x, nrm4.y, nrm4.z});
                    const Vec3 r = inc - nrm * (2.0f * Dot(inc, nrm));
                    vals[{n.id, "Out"}] = {{r.x, r.y, r.z, 0}, T::Vec3};
                }
                else if (n.type == "Project" || n.type == "Reject")
                {
                    const Vec4 a4 = getIn(n, spec->inputs[0], T::Vec3);
                    const Vec4 b4 = getIn(n, spec->inputs[1], T::Vec3);
                    const Vec3 a{a4.x, a4.y, a4.z};
                    const Vec3 uVec = Normalize(Vec3{b4.x, b4.y, b4.z});
                    const Vec3 p = uVec * Dot(a, uVec);
                    const Vec3 r = (n.type == "Project") ? p : (a - p);
                    vals[{n.id, "Out"}] = {{r.x, r.y, r.z, 0}, T::Vec3};
                }
                else if (n.type == "RotateAboutAxis")
                {
                    const Vec4 a = getIn(n, spec->inputs[0], T::Vec3);
                    const Vec4 ax = getIn(n, spec->inputs[1], T::Vec3);
                    const float ang = getIn(n, spec->inputs[2], T::Float).x;
                    const Vec3 r = RotateAboutAxisCpu({a.x, a.y, a.z}, {ax.x, ax.y, ax.z}, ang);
                    vals[{n.id, "Out"}] = {{r.x, r.y, r.z, 0}, T::Vec3};
                }
                else if (n.type == "Split")
                {
                    const Vec4 a = getIn(n, spec->inputs[0], T::Vec4);
                    vals[{n.id, "R"}] = {{a.x, 0, 0, 0}, T::Float};
                    vals[{n.id, "G"}] = {{a.y, 0, 0, 0}, T::Float};
                    vals[{n.id, "B"}] = {{a.z, 0, 0, 0}, T::Float};
                    vals[{n.id, "A"}] = {{a.w, 0, 0, 0}, T::Float};
                }
                else if (n.type == "Combine")
                {
                    const float r = getIn(n, spec->inputs[0], T::Float).x;
                    const float g = getIn(n, spec->inputs[1], T::Float).x;
                    const float b = getIn(n, spec->inputs[2], T::Float).x;
                    const float a = getIn(n, spec->inputs[3], T::Float).x;
                    vals[{n.id, "XYZ"}] = {{r, g, b, 0}, T::Vec3};
                    vals[{n.id, "XYZW"}] = {{r, g, b, a}, T::Vec4};
                }
                else if (n.type == "Append")
                {
                    const Vec4 a = getIn(n, spec->inputs[0], T::Vec2);
                    const float b = getIn(n, spec->inputs[1], T::Float).x;
                    const float c = getIn(n, spec->inputs[2], T::Float).x;
                    vals[{n.id, "XY"}] = {{a.x, b, 0, 0}, T::Vec2};
                    vals[{n.id, "XYZ"}] = {{a.x, a.y, b, 0}, T::Vec3};
                    vals[{n.id, "XYZW"}] = {{a.x, a.y, b, c}, T::Vec4};
                }
                else if (n.type == "ComponentMask")
                {
                    const Vec4 a = getIn(n, spec->inputs[0], T::Vec4);
                    vals[{n.id, "XY"}] = {{a.x, a.y, 0, 0}, T::Vec2};
                    vals[{n.id, "XZ"}] = {{a.x, a.z, 0, 0}, T::Vec2};
                    vals[{n.id, "YZ"}] = {{a.y, a.z, 0, 0}, T::Vec2};
                    vals[{n.id, "ZYX"}] = {{a.z, a.y, a.x, 0}, T::Vec3};
                }
                else if (n.type == "TransformTS2WS")
                {
                    const Vec4 a = getIn(n, spec->inputs[0], T::Vec3);
                    const Vec3 ws = tangent * a.x + bitangent * a.y + normal * a.z;
                    vals[{n.id, "Out"}] = {{ws.x, ws.y, ws.z, 0}, T::Vec3};
                }
                // ---- 7. Color & Blend ----
                else if (n.type == "RGBToHSV")
                {
                    const Vec4 c = getIn(n, spec->inputs[0], T::Vec3);
                    const Vec3 hsv = RgbToHsvCpu({c.x, c.y, c.z});
                    vals[{n.id, "HSV"}] = {{hsv.x, hsv.y, hsv.z, 0}, T::Vec3};
                    vals[{n.id, "H"}] = {{hsv.x, 0, 0, 0}, T::Float};
                    vals[{n.id, "S"}] = {{hsv.y, 0, 0, 0}, T::Float};
                    vals[{n.id, "V"}] = {{hsv.z, 0, 0, 0}, T::Float};
                }
                else if (n.type == "HSVToRGB")
                {
                    const float h = getIn(n, spec->inputs[0], T::Float).x;
                    const float s = getIn(n, spec->inputs[1], T::Float).x;
                    const float vVal = getIn(n, spec->inputs[2], T::Float).x;
                    const Vec3 rgb = HsvToRgbCpu(h, s, vVal);
                    vals[{n.id, "RGB"}] = {{rgb.x, rgb.y, rgb.z, 0}, T::Vec3};
                }
                else if (n.type == "HueShift")
                {
                    const Vec4 c = getIn(n, spec->inputs[0], T::Vec3);
                    const float sh = getIn(n, spec->inputs[1], T::Float).x;
                    const Vec3 hsv = RgbToHsvCpu({c.x, c.y, c.z});
                    const Vec3 rgb = HsvToRgbCpu(hsv.x + sh, hsv.y, hsv.z);
                    vals[{n.id, "Out"}] = {{rgb.x, rgb.y, rgb.z, 0}, T::Vec3};
                }
                else if (n.type == "Saturation")
                {
                    const Vec4 c = getIn(n, spec->inputs[0], T::Vec3);
                    const float sat = getIn(n, spec->inputs[1], T::Float).x;
                    const float l = c.x * 0.2126f + c.y * 0.7152f + c.z * 0.0722f;
                    vals[{n.id, "Out"}] = {{l + (c.x - l) * sat, l + (c.y - l) * sat, l + (c.z - l) * sat, 0}, T::Vec3};
                }
                else if (n.type == "Contrast")
                {
                    const Vec4 c = getIn(n, spec->inputs[0], T::Vec3);
                    const float con = getIn(n, spec->inputs[1], T::Float).x;
                    const float mid = getIn(n, spec->inputs[2], T::Float).x;
                    vals[{n.id, "Out"}] = {{(c.x - mid) * con + mid, (c.y - mid) * con + mid, (c.z - mid) * con + mid, 0}, T::Vec3};
                }
                else if (n.type == "BrightnessGamma")
                {
                    const Vec4 c = getIn(n, spec->inputs[0], T::Vec3);
                    const float br = getIn(n, spec->inputs[1], T::Float).x;
                    const float invGm = 1.0f / std::max(getIn(n, spec->inputs[2], T::Float).x, 0.05f);
                    vals[{n.id, "Out"}] = {{std::pow(std::max(c.x * br, 1e-5f), invGm),
                                            std::pow(std::max(c.y * br, 1e-5f), invGm),
                                            std::pow(std::max(c.z * br, 1e-5f), invGm), 0}, T::Vec3};
                }
                else if (n.type == "Luminance")
                {
                    const Vec4 c = getIn(n, spec->inputs[0], T::Vec3);
                    vals[{n.id, "Out"}] = {{c.x * 0.2126f + c.y * 0.7152f + c.z * 0.0722f, 0, 0, 0}, T::Float};
                }
                else if (n.type == "Posterize")
                {
                    const Vec4 c = getIn(n, spec->inputs[0], T::Vec3);
                    const float s = std::max(1.0f, std::floor(getIn(n, spec->inputs[1], T::Float).x));
                    vals[{n.id, "Out"}] = {{std::floor(c.x * s) / s, std::floor(c.y * s) / s, std::floor(c.z * s) / s, 0}, T::Vec3};
                }
                else if (n.type == "ColorRamp")
                {
                    const float t0 = std::clamp(getIn(n, spec->inputs[0], T::Float).x, 0.0f, 1.0f);
                    const float t = t0 * t0 * (3.0f - 2.0f * t0);
                    const Vec4 ca = getIn(n, spec->inputs[1], T::Vec3);
                    const Vec4 cb = getIn(n, spec->inputs[2], T::Vec3);
                    vals[{n.id, "Out"}] = {{ca.x + (cb.x - ca.x) * t, ca.y + (cb.y - ca.y) * t, ca.z + (cb.z - ca.z) * t, 0}, T::Vec3};
                }
                else if (n.type == "ColorMask")
                {
                    const Vec4 c = getIn(n, spec->inputs[0], T::Vec3);
                    const Vec4 mc = getIn(n, spec->inputs[1], T::Vec3);
                    const float rng = getIn(n, spec->inputs[2], T::Float).x;
                    const float fuz = std::max(getIn(n, spec->inputs[3], T::Float).x, 1e-4f);
                    const float d = std::sqrt((c.x - mc.x) * (c.x - mc.x) + (c.y - mc.y) * (c.y - mc.y) + (c.z - mc.z) * (c.z - mc.z));
                    vals[{n.id, "Out"}] = {{std::clamp(1.0f - (d - rng) / fuz, 0.0f, 1.0f), 0, 0, 0}, T::Float};
                }
                else if (n.type == "BlendScreen" || n.type == "BlendOverlay" || n.type == "BlendSoftLight")
                {
                    const Vec4 a4 = getIn(n, spec->inputs[0], T::Vec3);
                    const Vec4 b4 = getIn(n, spec->inputs[1], T::Vec3);
                    const float op = std::clamp(getIn(n, spec->inputs[2], T::Float).x, 0.0f, 1.0f);
                    float outC[3]{};
                    const float aa[3]{a4.x, a4.y, a4.z}, bb[3]{b4.x, b4.y, b4.z};
                    for (int k = 0; k < 3; ++k)
                    {
                        const float ca = std::clamp(aa[k], 0.0f, 1.0f), cb = std::clamp(bb[k], 0.0f, 1.0f);
                        float br = 0.0f;
                        if (n.type == "BlendScreen") br = 1.0f - (1.0f - ca) * (1.0f - cb);
                        else if (n.type == "BlendOverlay") br = ca < 0.5f ? (2.0f * ca * cb) : (1.0f - 2.0f * (1.0f - ca) * (1.0f - cb));
                        else br = cb < 0.5f ? (2.0f * ca * cb + ca * ca * (1.0f - 2.0f * cb)) : (std::sqrt(ca) * (2.0f * cb - 1.0f) + 2.0f * ca * (1.0f - cb));
                        outC[k] = aa[k] + (br - aa[k]) * op;
                    }
                    vals[{n.id, "Out"}] = {{outC[0], outC[1], outC[2], 0}, T::Vec3};
                }
                else if (n.type == "HDRBoost")
                {
                    const Vec4 c = getIn(n, spec->inputs[0], T::Vec3);
                    const float mul = std::exp2(std::clamp(getIn(n, spec->inputs[1], T::Float).x, -10.0f, 12.0f));
                    vals[{n.id, "Out"}] = {{c.x * mul, c.y * mul, c.z * mul, 0}, T::Vec3};
                }
                // ---- 8. Procedural Noise & Shapes ----
                else if (n.type == "SimpleNoise")
                {
                    const Vec4 uvIn = getIn(n, spec->inputs[0], T::Vec2);
                    const float sc = getIn(n, spec->inputs[1], T::Float).x;
                    vals[{n.id, "Out"}] = {{ValueNoise2D(uvIn.x * sc, uvIn.y * sc), 0, 0, 0}, T::Float};
                }
                else if (n.type == "GradientNoise")
                {
                    const Vec4 uvIn = getIn(n, spec->inputs[0], T::Vec2);
                    const float sc = getIn(n, spec->inputs[1], T::Float).x;
                    vals[{n.id, "Out"}] = {{GradientNoise2D(uvIn.x * sc, uvIn.y * sc), 0, 0, 0}, T::Float};
                }
                else if (n.type == "FbmNoise")
                {
                    const Vec4 uvIn = getIn(n, spec->inputs[0], T::Vec2);
                    const float sc = getIn(n, spec->inputs[1], T::Float).x;
                    const float det = getIn(n, spec->inputs[2], T::Float).x;
                    vals[{n.id, "Out"}] = {{FbmNoise2D(uvIn.x * sc, uvIn.y * sc, det), 0, 0, 0}, T::Float};
                }
                else if (n.type == "Voronoi")
                {
                    const Vec4 uvIn = getIn(n, spec->inputs[0], T::Vec2);
                    const float sc = getIn(n, spec->inputs[1], T::Float).x;
                    float dist = 0.0f, cid = 0.0f;
                    Voronoi2DEx(uvIn.x * sc, uvIn.y * sc, dist, cid);
                    vals[{n.id, "Out"}] = {{dist, 0, 0, 0}, T::Float};
                    vals[{n.id, "CellId"}] = {{cid, 0, 0, 0}, T::Float};
                }
                else if (n.type == "Checkerboard")
                {
                    const Vec4 uvIn = getIn(n, spec->inputs[0], T::Vec2);
                    const Vec4 ca = getIn(n, spec->inputs[1], T::Vec3);
                    const Vec4 cb = getIn(n, spec->inputs[2], T::Vec3);
                    const Vec4 fq = getIn(n, spec->inputs[3], T::Vec2);
                    const int px = static_cast<int>(std::floor(uvIn.x * fq.x * 2.0f));
                    const int py = static_cast<int>(std::floor(uvIn.y * fq.y * 2.0f));
                    vals[{n.id, "Out"}] = {(((px + py) & 1) != 0) ? cb : ca, T::Vec3};
                }
                else if (n.type == "GridPattern")
                {
                    const Vec4 uvIn = getIn(n, spec->inputs[0], T::Vec2);
                    const Vec4 til = getIn(n, spec->inputs[1], T::Vec2);
                    const float th = getIn(n, spec->inputs[2], T::Float).x * 0.5f;
                    const float fx = std::abs((uvIn.x * til.x - std::floor(uvIn.x * til.x)) - 0.5f);
                    const float fy = std::abs((uvIn.y * til.y - std::floor(uvIn.y * til.y)) - 0.5f);
                    const float d = 0.5f - std::max(fx, fy);
                    const float t = std::clamp((d - th) / 0.015f, 0.0f, 1.0f);
                    const float ln = 1.0f - t * t * (3.0f - 2.0f * t);
                    vals[{n.id, "Line"}] = {{ln, 0, 0, 0}, T::Float};
                    vals[{n.id, "Cell"}] = {{1.0f - ln, 0, 0, 0}, T::Float};
                }
                else if (n.type == "BrickPattern")
                {
                    const Vec4 uvIn = getIn(n, spec->inputs[0], T::Vec2);
                    const Vec4 til = getIn(n, spec->inputs[1], T::Vec2);
                    const float gap = std::max(getIn(n, spec->inputs[2], T::Float).x, 0.005f);
                    float stx = uvIn.x * til.x, sty = uvIn.y * til.y;
                    if (((static_cast<int>(std::floor(sty)) & 1) != 0)) stx += 0.5f;
                    const float ix = std::floor(stx), iy = std::floor(sty);
                    const float fx = stx - ix, fy = sty - iy;
                    const auto sm = [](float e1, float xv) { const float t = std::clamp(xv / e1, 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); };
                    const float br = sm(gap, fx) * sm(gap, 1.0f - fx) * sm(gap, fy) * sm(gap, 1.0f - fy);
                    vals[{n.id, "Brick"}] = {{br, 0, 0, 0}, T::Float};
                    vals[{n.id, "BrickId"}] = {{Hash21(ix, iy), 0, 0, 0}, T::Float};
                }
                else if (n.type == "EllipseShape")
                {
                    const Vec4 uvIn = getIn(n, spec->inputs[0], T::Vec2);
                    const float w = std::max(getIn(n, spec->inputs[1], T::Float).x, 1e-4f);
                    const float h = std::max(getIn(n, spec->inputs[2], T::Float).x, 1e-4f);
                    const float fea = std::max(getIn(n, spec->inputs[3], T::Float).x, 0.002f);
                    const float d = std::hypot((uvIn.x * 2.0f - 1.0f) / w, (uvIn.y * 2.0f - 1.0f) / h);
                    const float t = std::clamp((d - (1.0f - fea)) / (2.0f * fea), 0.0f, 1.0f);
                    vals[{n.id, "Out"}] = {{1.0f - t * t * (3.0f - 2.0f * t), 0, 0, 0}, T::Float};
                }
                else if (n.type == "RectangleShape")
                {
                    const Vec4 uvIn = getIn(n, spec->inputs[0], T::Vec2);
                    const float w = getIn(n, spec->inputs[1], T::Float).x;
                    const float h = getIn(n, spec->inputs[2], T::Float).x;
                    const float r = std::clamp(getIn(n, spec->inputs[3], T::Float).x, 0.0f, std::min(w, h) * 0.5f);
                    const float qx = std::abs(uvIn.x - 0.5f) - (w * 0.5f - r);
                    const float qy = std::abs(uvIn.y - 0.5f) - (h * 0.5f - r);
                    const float sd = std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - r;
                    const float t = std::clamp((sd + 0.01f) / 0.02f, 0.0f, 1.0f);
                    vals[{n.id, "Out"}] = {{1.0f - t * t * (3.0f - 2.0f * t), 0, 0, 0}, T::Float};
                }
                else if (n.type == "PolygonShape")
                {
                    const Vec4 uvIn = getIn(n, spec->inputs[0], T::Vec2);
                    const float sd = getIn(n, spec->inputs[1], T::Float).x;
                    const float w = getIn(n, spec->inputs[2], T::Float).x;
                    const float h = getIn(n, spec->inputs[3], T::Float).x;
                    vals[{n.id, "Out"}] = {{PolygonMaskCpu(uvIn.x, uvIn.y, sd, w, h), 0, 0, 0}, T::Float};
                }
                else if (n.type == "SphereMask")
                {
                    const Vec4 p = getIn(n, spec->inputs[0], T::Vec3);
                    const Vec4 ctr = getIn(n, spec->inputs[1], T::Vec3);
                    const float rad = std::max(getIn(n, spec->inputs[2], T::Float).x, 1e-4f);
                    const float hrd = std::clamp(getIn(n, spec->inputs[3], T::Float).x, 0.0f, 0.999f);
                    const float d = std::sqrt((p.x - ctr.x) * (p.x - ctr.x) + (p.y - ctr.y) * (p.y - ctr.y) + (p.z - ctr.z) * (p.z - ctr.z)) / rad;
                    vals[{n.id, "Out"}] = {{std::clamp((1.0f - d) / std::max(1.0f - hrd, 1e-4f), 0.0f, 1.0f), 0, 0, 0}, T::Float};
                }
                else if (n.type == "WaveRing")
                {
                    const Vec4 uvIn = getIn(n, spec->inputs[0], T::Vec2);
                    const Vec4 ctr = getIn(n, spec->inputs[1], T::Vec2);
                    const float fq = getIn(n, spec->inputs[2], T::Float).x;
                    const float spd = getIn(n, spec->inputs[3], T::Float).x;
                    const float d = std::hypot(uvIn.x - ctr.x, uvIn.y - ctr.y);
                    vals[{n.id, "Out"}] = {{std::sin(d * fq * 6.2831853f - timeVec.x * spd) * 0.5f + 0.5f, 0, 0, 0}, T::Float};
                }
                // ---- 9. Artistic, Stylized & Logic ----
                else if (n.type == "Fresnel")
                {
                    const float p = std::max(0.05f, getIn(n, spec->inputs[0], T::Float).x);
                    const float ndv = std::clamp(Dot(normal, viewDir), 0.0f, 1.0f);
                    vals[{n.id, "Out"}] = {{std::pow(1.0f - ndv, p), 0, 0, 0}, T::Float};
                }
                else if (n.type == "RimLight")
                {
                    const Vec4 col = getIn(n, spec->inputs[0], T::Vec3);
                    const float p = std::max(0.05f, getIn(n, spec->inputs[1], T::Float).x);
                    const float thr = getIn(n, spec->inputs[2], T::Float).x;
                    const float sft = std::max(0.01f, getIn(n, spec->inputs[3], T::Float).x);
                    const float f = std::pow(1.0f - std::clamp(Dot(normal, viewDir), 0.0f, 1.0f), p);
                    const float t = std::clamp(((Dot(normal, lightDir) * 0.5f + 0.5f) - (thr - sft)) / (2.0f * sft), 0.0f, 1.0f);
                    const float m = f * t * t * (3.0f - 2.0f * t);
                    vals[{n.id, "RGB"}] = {{col.x * m, col.y * m, col.z * m, 0}, T::Vec3};
                    vals[{n.id, "Mask"}] = {{m, 0, 0, 0}, T::Float};
                }
                else if (n.type == "HalfLambert")
                {
                    const Vec4 n4 = getIn(n, spec->inputs[0], T::Vec3);
                    const float wrp = std::clamp(getIn(n, spec->inputs[1], T::Float).x, 0.0f, 1.0f);
                    const float nl = Dot(Normalize(Vec3{n4.x, n4.y, n4.z}), lightDir);
                    vals[{n.id, "HalfLambert"}] = {{std::clamp((nl + wrp) / (1.0f + wrp), 0.0f, 1.0f), 0, 0, 0}, T::Float};
                    vals[{n.id, "NdotL"}] = {{std::clamp(nl, 0.0f, 1.0f), 0, 0, 0}, T::Float};
                }
                else if (n.type == "CelShadeBand")
                {
                    const float xv = getIn(n, spec->inputs[0], T::Float).x;
                    const float thr = getIn(n, spec->inputs[1], T::Float).x;
                    const float sm = std::max(0.001f, getIn(n, spec->inputs[2], T::Float).x);
                    const float t = std::clamp((xv - (thr - sm)) / (2.0f * sm), 0.0f, 1.0f);
                    vals[{n.id, "Out"}] = {{t * t * (3.0f - 2.0f * t), 0, 0, 0}, T::Float};
                }
                else if (n.type == "SpecularStylized")
                {
                    const Vec4 n4 = getIn(n, spec->inputs[0], T::Vec3);
                    const float gls = std::clamp(getIn(n, spec->inputs[1], T::Float).x, 0.0f, 1.0f);
                    const float hrd = std::clamp(getIn(n, spec->inputs[2], T::Float).x, 0.0f, 1.0f);
                    const Vec3 hv = Normalize(lightDir + viewDir);
                    const float nh = std::pow(std::clamp(Dot(Normalize(Vec3{n4.x, n4.y, n4.z}), hv), 0.0f, 1.0f), std::exp2(2.0f + gls * 7.0f));
                    const float f = std::max(1.0f - hrd, 0.01f) * 0.45f;
                    const float t = std::clamp((nh - (0.5f - f)) / (2.0f * f), 0.0f, 1.0f);
                    vals[{n.id, "Out"}] = {{t * t * (3.0f - 2.0f * t), 0, 0, 0}, T::Float};
                }
                else if (n.type == "MatCapUV")
                {
                    const Vec4 n4 = getIn(n, spec->inputs[0], T::Vec3);
                    const Vec3 nn = Normalize(Vec3{n4.x, n4.y, n4.z});
                    vals[{n.id, "UV"}] = {{nn.x * 0.49f + 0.5f, -nn.y * 0.49f + 0.5f, 0, 0}, T::Vec2};
                }
                else if (n.type == "DissolveEdge")
                {
                    const float ns = getIn(n, spec->inputs[0], T::Float).x;
                    const float prg = getIn(n, spec->inputs[1], T::Float).x;
                    const float ew = std::max(getIn(n, spec->inputs[2], T::Float).x, 0.002f);
                    const Vec4 ecol = getIn(n, spec->inputs[3], T::Vec3);
                    const float d = ns - prg;
                    const float a = d >= 0.0f ? 1.0f : 0.0f;
                    const float t = std::clamp(d / ew, 0.0f, 1.0f);
                    const float e = a * (1.0f - t * t * (3.0f - 2.0f * t));
                    vals[{n.id, "Alpha"}] = {{a, 0, 0, 0}, T::Float};
                    vals[{n.id, "EdgeMask"}] = {{e, 0, 0, 0}, T::Float};
                    vals[{n.id, "Emissive"}] = {{ecol.x * e * 3.0f, ecol.y * e * 3.0f, ecol.z * e * 3.0f, 0}, T::Vec3};
                }
                else if (n.type == "DitherThreshold")
                {
                    const float op = std::clamp(getIn(n, spec->inputs[0], T::Float).x, 0.0f, 1.0f);
                    static constexpr float kBayer[16]{0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
                    const float b = (kBayer[(y & 3u) * 4u + (x & 3u)] + 0.5f) / 16.0f;
                    vals[{n.id, "ClipAlpha"}] = {{op >= b ? 1.0f : 0.0f, 0, 0, 0}, T::Float};
                    vals[{n.id, "Bayer"}] = {{b, 0, 0, 0}, T::Float};
                }
                else if (n.type == "HeightFogMask")
                {
                    const float hy = getIn(n, spec->inputs[0], T::Float).x;
                    const float by = getIn(n, spec->inputs[1], T::Float).x;
                    const float fo = std::max(getIn(n, spec->inputs[2], T::Float).x, 1e-4f);
                    vals[{n.id, "Out"}] = {{std::clamp((hy - by) / fo, 0.0f, 1.0f), 0, 0, 0}, T::Float};
                }
                else if (n.type == "VertexWindWave")
                {
                    const Vec4 amp = getIn(n, spec->inputs[0], T::Vec3);
                    const float fq = getIn(n, spec->inputs[1], T::Float).x;
                    const float spd = getIn(n, spec->inputs[2], T::Float).x;
                    const float w = std::sin((normal.x + normal.z) * fq + timeVec.x * spd) * std::clamp(normal.y + 0.5f, 0.0f, 1.0f);
                    vals[{n.id, "Offset"}] = {{amp.x * w, amp.y * w, amp.z * w, 0}, T::Vec3};
                }
                else if (n.type == "CompareIf")
                {
                    const float a = getIn(n, spec->inputs[0], T::Float).x;
                    const float b = getIn(n, spec->inputs[1], T::Float).x;
                    const auto gt = getRaw(n, spec->inputs[2]);
                    const auto eq = getRaw(n, spec->inputs[3]);
                    const auto lt = getRaw(n, spec->inputs[4]);
                    const T outT = WiderType(WiderType(gt.t, eq.t), lt.t);
                    const Vec4 chosen = std::abs(a - b) < 1e-5f ? CastCpuValue(eq.v, eq.t, outT)
                                      : (a > b ? CastCpuValue(gt.v, gt.t, outT) : CastCpuValue(lt.v, lt.t, outT));
                    vals[{n.id, "Out"}] = {chosen, outT};
                }
                else if (n.type == "StaticSwitch")
                {
                    const float cond = getIn(n, spec->inputs[0], T::Float).x;
                    const auto tr = getRaw(n, spec->inputs[1]);
                    const auto fa = getRaw(n, spec->inputs[2]);
                    const T outT = WiderType(tr.t, fa.t);
                    vals[{n.id, "Out"}] = {cond >= 0.5f ? CastCpuValue(tr.v, tr.t, outT) : CastCpuValue(fa.v, fa.t, outT), outT};
                }
            }

            const Vec4 baseCol4 = getIn(*master, masterSpec->inputs[0], T::Vec3);
            const float metal = std::clamp(getIn(*master, masterSpec->inputs[1], T::Float).x, 0.0f, 1.0f);
            const float rough = std::clamp(getIn(*master, masterSpec->inputs[2], T::Float).x, 0.045f, 1.0f);
            const Vec4 nts4 = getIn(*master, masterSpec->inputs[3], T::Vec3);
            const Vec3 tsN = Normalize(Vec3{nts4.x, nts4.y, std::max(nts4.z, 1e-4f)});
            const Vec3 shadingNormal = Normalize(tangent * tsN.x + bitangent * tsN.y + normal * tsN.z);
            const Vec4 emis4 = getIn(*master, masterSpec->inputs[4], T::Vec3);
            const float opacity = std::clamp(getIn(*master, masterSpec->inputs[5], T::Float).x, 0.0f, 1.0f);
            const float cutoff = getIn(*master, masterSpec->inputs[6], T::Float).x;

            float finalAlpha = 1.0f;
            if (graph.alphaMode == MaterialAlphaMode::Mask && opacity < cutoff) finalAlpha = 0.0f;
            else if (graph.alphaMode == MaterialAlphaMode::Blend) finalAlpha = opacity;

            Vec3 shaded{};
            const Vec3 albedo{std::max(0.0f, baseCol4.x), std::max(0.0f, baseCol4.y), std::max(0.0f, baseCol4.z)};
            if (graph.shadingModel == SurfaceShadingModel::Unlit)
            {
                shaded = albedo;
            }
            else
            {
                float ndl = std::max(0.0f, Dot(shadingNormal, lightDir));
                if (graph.shadingModel == SurfaceShadingModel::Toon)
                {
                    ndl = std::ceil(ndl * 3.0f) / 3.0f;
                }
                const Vec3 halfV = Normalize(lightDir + viewDir);
                const float shininess = 8.0f + (1.0f - rough) * 140.0f;
                const float specStrength = (1.0f - rough) * 0.9f;
                const float spec = std::pow(std::max(0.0f, Dot(shadingNormal, halfV)), shininess) * specStrength;
                const Vec3 diffCol = albedo * (1.0f - metal);
                const Vec3 specCol = albedo * metal + Vec3{1.0f - metal, 1.0f - metal, 1.0f - metal} * 0.04f * 16.0f;
                const float skyHemi = 0.5f + 0.5f * shadingNormal.y;
                const Vec3 ambient = (diffCol * (0.12f + 0.14f * skyHemi)) + specCol * (0.08f * (1.0f - rough) * skyHemi);
                shaded = ambient + diffCol * (ndl * 1.15f) + specCol * spec;
            }
            shaded = shaded + Vec3{std::max(0.0f, emis4.x), std::max(0.0f, emis4.y), std::max(0.0f, emis4.z)};
            // Reinhard-ACES soft roll-off for HDR emissive on preview sphere
            shaded = {shaded.x / (1.0f + shaded.x * 0.35f),
                      shaded.y / (1.0f + shaded.y * 0.35f),
                      shaded.z / (1.0f + shaded.z * 0.35f)};

            const float cov = edgeAa * finalAlpha;
            const Vec3 outRgb = bgCol * (1.0f - cov) + shaded * cov;
            img.rgba8[pxOffset + 0] = ToSrgbByte(outRgb.x);
            img.rgba8[pxOffset + 1] = ToSrgbByte(outRgb.y);
            img.rgba8[pxOffset + 2] = ToSrgbByte(outRgb.z);
            img.rgba8[pxOffset + 3] = 255;
        }
    }
    return img;
}
} // namespace flare
