#include "SurfaceShader.h"
#include "AssetIO.h"
#include "CoreLayer/Hash.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <sstream>

namespace flare
{
namespace
{
std::string LowerAscii(std::string_view s)
{
    std::string out(s);
    for (char& ch : out)
    {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return out;
}

std::string_view Trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
    return s;
}

bool ValidHlslIdentifier(std::string_view s)
{
    if (s.empty() || s.size() > 64) return false;
    const auto isAlpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
    const auto isAlnum = [&](char c) { return isAlpha(c) || (c >= '0' && c <= '9'); };
    if (!isAlpha(s.front())) return false;
    for (char c : s)
    {
        if (!isAlnum(c)) return false;
    }
    return true;
}

bool ParseFloatToken(std::string_view token, float& out)
{
    token = Trim(token);
    if (!token.empty() && (token.back() == 'f' || token.back() == 'F')) token.remove_suffix(1);
    if (token.empty()) return false;
    std::string buf(token);
    char* end = nullptr;
    const float v = std::strtof(buf.c_str(), &end);
    if (end == buf.c_str() || *end != '\0' || !std::isfinite(v)) return false;
    out = v;
    return true;
}

bool ParseTupleValues(std::string_view text, float* values, std::size_t count)
{
    text = Trim(text);
    if (text.size() >= 2 && text.front() == '(' && text.back() == ')')
    {
        text.remove_prefix(1);
        text.remove_suffix(1);
    }
    std::size_t idx = 0;
    while (!text.empty() && idx < count)
    {
        const std::size_t comma = text.find(',');
        const std::string_view part = (comma == std::string_view::npos) ? text : text.substr(0, comma);
        if (!ParseFloatToken(part, values[idx])) return false;
        ++idx;
        if (comma == std::string_view::npos) break;
        text.remove_prefix(comma + 1);
    }
    return idx == count;
}

std::string ExtractTrailingQuoted(std::string_view& line)
{
    line = Trim(line);
    if (line.size() >= 2 && line.back() == '"')
    {
        const std::size_t openQuote = line.find_last_of('"', line.size() - 2);
        if (openQuote != std::string_view::npos)
        {
            std::string quoted(line.substr(openQuote + 1, line.size() - openQuote - 2));
            line = Trim(line.substr(0, openQuote));
            return quoted;
        }
    }
    return {};
}

bool AssignParameterSlot(std::array<std::uint32_t, MaxSurfaceShaderVec4Slots>& usedComponents,
                         SurfaceShaderParam& param)
{
    const std::uint32_t need = param.componentCount;
    for (std::uint32_t slot = 0; slot < MaxSurfaceShaderVec4Slots; ++slot)
    {
        std::uint32_t start = usedComponents[slot];
        if (need == 2 && start == 1) start = 2; // align Vec2 to .xy or .zw
        if (need >= 3 && start > 0) continue;   // Vec3/Vec4 starts at .x
        if (start + need <= 4)
        {
            param.slotIndex = slot;
            param.componentOffset = start;
            usedComponents[slot] = start + need;
            return true;
        }
    }
    return false;
}

std::string SwizzleFor(std::uint32_t offset, std::uint32_t count)
{
    constexpr char comps[] = "xyzw";
    std::string out;
    for (std::uint32_t i = 0; i < count && (offset + i) < 4; ++i)
    {
        out.push_back(comps[offset + i]);
    }
    return out;
}

bool CopyIfDifferent(const std::filesystem::path& src, const std::filesystem::path& dst)
{
    std::vector<std::uint8_t> srcBytes;
    if (!asset_io::Read(src, srcBytes)) return false;
    std::vector<std::uint8_t> dstBytes;
    if (std::filesystem::exists(dst) && asset_io::Read(dst, dstBytes) && dstBytes == srcBytes) return true;
    std::error_code ec;
    std::filesystem::create_directories(dst.parent_path(), ec);
    return asset_io::WriteAtomic(dst, srcBytes);
}

// 去除 // 与 /* */ 注释但原样保留引号内容与换行符，保证字节偏移与行号 100% 对齐。
std::string StripCommentsPreserveLines(std::string_view src)
{
    std::string out(src.size(), ' ');
    bool inQuote = false;
    bool inLineComment = false;
    bool inBlockComment = false;
    for (std::size_t i = 0; i < src.size(); ++i)
    {
        const char c = src[i];
        if (c == '\n')
        {
            out[i] = '\n';
            inLineComment = false;
            inQuote = false;
            continue;
        }
        if (c == '\r')
        {
            out[i] = '\r';
            continue;
        }
        if (inLineComment) continue;
        if (inBlockComment)
        {
            if (c == '*' && i + 1 < src.size() && src[i + 1] == '/')
            {
                inBlockComment = false;
                ++i;
            }
            continue;
        }
        if (!inQuote && c == '/' && i + 1 < src.size() && src[i + 1] == '/')
        {
            inLineComment = true;
            ++i;
            continue;
        }
        if (!inQuote && c == '/' && i + 1 < src.size() && src[i + 1] == '*')
        {
            inBlockComment = true;
            ++i;
            continue;
        }
        if (c == '"')
        {
            std::size_t bs = 0;
            while (bs < i && src[i - 1 - bs] == '\\') ++bs;
            if ((bs & 1u) == 0u) inQuote = !inQuote;
        }
        out[i] = c;
    }
    return out;
}

bool IsUnescapedQuote(std::string_view s, std::size_t idx) noexcept
{
    if (idx >= s.size() || s[idx] != '"') return false;
    std::size_t bs = 0;
    while (bs < idx && s[idx - 1 - bs] == '\\') ++bs;
    return (bs & 1u) == 0u;
}

bool IsIdentChar(char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

std::size_t FindKeywordOutsideQuotes(std::string_view cleanText, std::string_view kw, std::size_t from = 0)
{
    bool inQuote = false;
    for (std::size_t i = from; i < cleanText.size(); ++i)
    {
        const char c = cleanText[i];
        if (c == '\n') { inQuote = false; continue; }
        if (IsUnescapedQuote(cleanText, i))
        {
            inQuote = !inQuote;
            continue;
        }
        if (inQuote) continue;
        if (i + kw.size() <= cleanText.size() &&
            LowerAscii(cleanText.substr(i, kw.size())) == LowerAscii(kw))
        {
            const bool leftOk = (i == 0 || !IsIdentChar(cleanText[i - 1]));
            const bool rightOk = (i + kw.size() == cleanText.size() || !IsIdentChar(cleanText[i + kw.size()]));
            if (leftOk && rightOk) return i;
        }
    }
    return std::string_view::npos;
}

bool HasFunctionDefinition(std::string_view cleanBody, std::string_view fnName, bool* outHasMultipleParams = nullptr)
{
    if (outHasMultipleParams) *outHasMultipleParams = false;
    std::size_t pos = 0;
    while ((pos = FindKeywordOutsideQuotes(cleanBody, fnName, pos)) != std::string_view::npos)
    {
        // Check preceded by "void" on the left and followed by "(...) {" on the right
        std::size_t left = pos;
        while (left > 0 && (cleanBody[left - 1] == ' ' || cleanBody[left - 1] == '\t' || cleanBody[left - 1] == '\r' || cleanBody[left - 1] == '\n'))
            --left;
        const bool afterVoid = (left >= 4 && cleanBody.substr(left - 4, 4) == "void" &&
                                (left == 4 || !IsIdentChar(cleanBody[left - 5])));
        std::size_t right = pos + fnName.size();
        while (right < cleanBody.size() && (cleanBody[right] == ' ' || cleanBody[right] == '\t' || cleanBody[right] == '\r' || cleanBody[right] == '\n'))
            ++right;
        if (afterVoid && right < cleanBody.size() && cleanBody[right] == '(')
        {
            int depth = 0;
            bool sawTopComma = false;
            std::size_t closeP = right;
            for (; closeP < cleanBody.size(); ++closeP)
            {
                if (cleanBody[closeP] == '(') ++depth;
                else if (cleanBody[closeP] == ')')
                {
                    if (--depth == 0) break;
                }
                else if (cleanBody[closeP] == ',' && depth == 1) sawTopComma = true;
            }
            if (closeP < cleanBody.size())
            {
                std::size_t brace = closeP + 1;
                while (brace < cleanBody.size() && (cleanBody[brace] == ' ' || cleanBody[brace] == '\t' || cleanBody[brace] == '\r' || cleanBody[brace] == '\n'))
                    ++brace;
                if (brace < cleanBody.size() && cleanBody[brace] == '{')
                {
                    if (outHasMultipleParams) *outHasMultipleParams = sawTopComma;
                    return true;
                }
            }
        }
        pos += fnName.size();
    }
    return false;
}

std::size_t FindMatchingBrace(std::string_view cleanText, std::size_t openBracePos, char openCh = '{', char closeCh = '}')
{
    if (openBracePos >= cleanText.size() || cleanText[openBracePos] != openCh) return std::string_view::npos;
    bool inQuote = false;
    int depth = 0;
    for (std::size_t i = openBracePos; i < cleanText.size(); ++i)
    {
        const char c = cleanText[i];
        if (c == '\n') { inQuote = false; continue; }
        if (IsUnescapedQuote(cleanText, i)) { inQuote = !inQuote; continue; }
        if (inQuote) continue;
        if (c == openCh) ++depth;
        else if (c == closeCh)
        {
            if (--depth == 0) return i;
        }
    }
    return std::string_view::npos;
}

bool IsReservedUserMacroIdent(std::string_view s) noexcept
{
    return s == "_Time" || s == "_SinTime" || s == "_CosTime" || s == "unity_DeltaTime" ||
           s == "_MainTex" || s == "_BumpMap" || s == "_MetallicGlossMap" ||
           s == "Input" || s == "SurfaceOutput" || s == "SurfaceOutputStandard" ||
           s == "VertexInput" || s == "appdata_full" || s == "tex2D" || s == "tex2Dlod" || s == "tex2Dgrad" ||
           s == "flareTime" || s == "flareUserParams";
}

bool ParseTypedParam(std::string_view nameView, std::string label, std::string_view typeSpec,
                     std::string_view defSpec, bool isToggleAttr, std::uint32_t lineNum,
                     std::array<std::uint32_t, MaxSurfaceShaderVec4Slots>& usedComponents,
                     SurfaceShaderMetadata& output, std::string& errorMessage)
{
    if (!ValidHlslIdentifier(nameView))
    {
        errorMessage = "Line " + std::to_string(lineNum) + ": invalid parameter identifier '" + std::string(nameView) + "'.";
        return false;
    }
    const std::string lowerType = LowerAscii(Trim(typeSpec));
    // Built-in Unity texture slots in Properties (e.g. _MainTex ("Albedo", 2D) = "white" {}) are bound automatically by Material slots.
    if (lowerType == "2d")
    {
        if (nameView == "_MainTex" || nameView == "_BumpMap" || nameView == "_MetallicGlossMap")
            return true;
        if (IsReservedUserMacroIdent(nameView))
        {
            errorMessage = "Line " + std::to_string(lineNum) + ": texture property '" + std::string(nameView) +
                           "' conflicts with a built-in shader identifier.";
            return false;
        }
        for (const auto& existingTex : output.textures)
        {
            if (existingTex.name == nameView)
            {
                errorMessage = "Line " + std::to_string(lineNum) + ": duplicate texture property '" + std::string(nameView) + "'.";
                return false;
            }
        }
        for (const auto& existingParam : output.parameters)
        {
            if (existingParam.name == nameView)
            {
                errorMessage = "Line " + std::to_string(lineNum) + ": duplicate property '" + std::string(nameView) + "'.";
                return false;
            }
        }
        if (output.textures.size() >= MaxSurfaceShaderTextureSlots)
        {
            errorMessage = "Line " + std::to_string(lineNum) + ": exceeded maximum custom 2D texture slots (4 extra slots beyond _MainTex/_BumpMap/_MetallicGlossMap).";
            return false;
        }
        SurfaceShaderTexture tex;
        tex.name = std::string(nameView);
        tex.label = label.empty() ? tex.name : std::move(label);
        tex.slotIndex = static_cast<std::uint32_t>(output.textures.size());
        const std::string lowerDef = LowerAscii(defSpec);
        if (lowerDef.find("bump") != std::string::npos) tex.defaultKind = SurfaceTextureDefault::Bump;
        else if (lowerDef.find("black") != std::string::npos) tex.defaultKind = SurfaceTextureDefault::Black;
        else if (lowerDef.find("gray") != std::string::npos || lowerDef.find("grey") != std::string::npos)
            tex.defaultKind = SurfaceTextureDefault::Gray;
        else tex.defaultKind = SurfaceTextureDefault::White;
        output.textures.push_back(std::move(tex));
        return true;
    }
    if (lowerType == "2darray" || lowerType == "cube" || lowerType == "cubearray" || lowerType == "3d" || lowerType == "any")
    {
        return true;
    }
    if (IsReservedUserMacroIdent(nameView))
    {
        errorMessage = "Line " + std::to_string(lineNum) + ": parameter name '" + std::string(nameView) +
                       "' is a built-in shader identifier; please choose another name.";
        return false;
    }
    for (const auto& existingTex : output.textures)
    {
        if (existingTex.name == nameView)
        {
            errorMessage = "Line " + std::to_string(lineNum) + ": duplicate property '" + std::string(nameView) + "'.";
            return false;
        }
    }
    for (const auto& existing : output.parameters)
    {
        if (existing.name == nameView)
        {
            errorMessage = "Line " + std::to_string(lineNum) + ": duplicate parameter '" + std::string(nameView) + "'.";
            return false;
        }
    }

    SurfaceShaderParam param;
    param.name = std::string(nameView);
    param.label = label.empty() ? param.name : std::move(label);

    if (lowerType == "float" || lowerType == "int" || lowerType == "integer")
    {
        param.kind = isToggleAttr ? SurfaceParamKind::Range : SurfaceParamKind::Float;
        param.componentCount = 1;
        param.minValue = isToggleAttr ? 0.0f : -1000.0f;
        param.maxValue = isToggleAttr ? 1.0f : 1000.0f;
        if (!defSpec.empty() && !ParseFloatToken(defSpec, param.defaultValue.x))
        {
            errorMessage = "Line " + std::to_string(lineNum) + ": invalid Float default value.";
            return false;
        }
    }
    else if (lowerType.starts_with("range"))
    {
        param.kind = SurfaceParamKind::Range;
        param.componentCount = 1;
        const std::size_t lp = typeSpec.find('(');
        const std::size_t rp = typeSpec.rfind(')');
        float bounds[2]{0.0f, 1.0f};
        if (lp == std::string_view::npos || rp == std::string_view::npos || rp <= lp ||
            !ParseTupleValues(typeSpec.substr(lp, rp - lp + 1), bounds, 2))
        {
            errorMessage = "Line " + std::to_string(lineNum) + ": Range requires Range(min, max).";
            return false;
        }
        param.minValue = bounds[0];
        param.maxValue = std::max(bounds[0], bounds[1]);
        param.defaultValue.x = param.minValue;
        if (!defSpec.empty() && !ParseFloatToken(defSpec, param.defaultValue.x))
        {
            errorMessage = "Line " + std::to_string(lineNum) + ": invalid Range default value.";
            return false;
        }
    }
    else if (lowerType == "color")
    {
        param.kind = SurfaceParamKind::Color;
        param.componentCount = 4;
        param.defaultValue = {1.0f, 1.0f, 1.0f, 1.0f};
        float rgba[4]{1, 1, 1, 1};
        if (!defSpec.empty())
        {
            if (ParseTupleValues(defSpec, rgba, 4))
                param.defaultValue = {rgba[0], rgba[1], rgba[2], rgba[3]};
            else if (ParseTupleValues(defSpec, rgba, 3))
                param.defaultValue = {rgba[0], rgba[1], rgba[2], 1.0f};
            else
            {
                errorMessage = "Line " + std::to_string(lineNum) + ": Color default requires (r, g, b, a).";
                return false;
            }
        }
    }
    else if (lowerType == "vector2" || lowerType == "float2")
    {
        param.kind = SurfaceParamKind::Vector2;
        param.componentCount = 2;
        float xy[2]{0, 0};
        if (!defSpec.empty() && !ParseTupleValues(defSpec, xy, 2))
        {
            errorMessage = "Line " + std::to_string(lineNum) + ": Vector2 default requires (x, y).";
            return false;
        }
        param.defaultValue = {xy[0], xy[1], 0.0f, 0.0f};
    }
    else if (lowerType == "vector3" || lowerType == "float3")
    {
        param.kind = SurfaceParamKind::Vector3;
        param.componentCount = 3;
        float xyz[3]{0, 0, 0};
        if (!defSpec.empty() && !ParseTupleValues(defSpec, xyz, 3))
        {
            errorMessage = "Line " + std::to_string(lineNum) + ": Vector3 default requires (x, y, z).";
            return false;
        }
        param.defaultValue = {xyz[0], xyz[1], xyz[2], 0.0f};
    }
    else if (lowerType == "vector" || lowerType == "vector4" || lowerType == "float4")
    {
        param.kind = SurfaceParamKind::Vector4;
        param.componentCount = 4;
        float xyzw[4]{0, 0, 0, 0};
        if (!defSpec.empty())
        {
            if (ParseTupleValues(defSpec, xyzw, 4))
                param.defaultValue = {xyzw[0], xyzw[1], xyzw[2], xyzw[3]};
            else if (ParseTupleValues(defSpec, xyzw, 3))
                param.defaultValue = {xyzw[0], xyzw[1], xyzw[2], 0.0f};
            else if (ParseTupleValues(defSpec, xyzw, 2))
                param.defaultValue = {xyzw[0], xyzw[1], 0.0f, 0.0f};
            else
            {
                errorMessage = "Line " + std::to_string(lineNum) + ": Vector default requires (x, y, z, w).";
                return false;
            }
        }
    }
    else
    {
        errorMessage = "Line " + std::to_string(lineNum) + ": unsupported property type '" + std::string(Trim(typeSpec)) + "'.";
        return false;
    }

    if (!AssignParameterSlot(usedComponents, param))
    {
        errorMessage = "Line " + std::to_string(lineNum) + ": exceeded maximum custom parameter capacity (8 float4 slots).";
        return false;
    }
    output.parameters.push_back(std::move(param));
    return true;
}

std::uint32_t LineNumberAt(std::string_view src, std::size_t offset)
{
    std::uint32_t line = 1;
    const std::size_t lim = std::min(offset, src.size());
    for (std::size_t i = 0; i < lim; ++i)
    {
        if (src[i] == '\n') ++line;
    }
    return line;
}

std::string SanitizeUserHlslBody(std::string_view rawSlice,
                                 const std::vector<SurfaceShaderParam>& parameters,
                                 const std::vector<SurfaceShaderTexture>& textures,
                                 SurfaceShadingModel* ioShading)
{
    std::string rawBody(rawSlice);
    std::string cleanBody = StripCommentsPreserveLines(rawBody);

    // If a Unity user pasted "struct Input { ... };", blank it out preserving newlines so our superset Input struct is used cleanly!
    {
        std::size_t stPos = 0;
        while ((stPos = FindKeywordOutsideQuotes(cleanBody, "struct", stPos)) != std::string_view::npos)
        {
            std::size_t cur = stPos + 6;
            while (cur < cleanBody.size() && std::isspace(static_cast<unsigned char>(cleanBody[cur]))) ++cur;
            if (cur + 5 <= cleanBody.size() && cleanBody.substr(cur, 5) == "Input" &&
                (cur + 5 == cleanBody.size() || !IsIdentChar(cleanBody[cur + 5])))
            {
                std::size_t ob = cleanBody.find('{', cur + 5);
                std::size_t cb = (ob != std::string_view::npos) ? FindMatchingBrace(cleanBody, ob, '{', '}') : std::string_view::npos;
                if (cb != std::string_view::npos)
                {
                    std::size_t endSt = cb + 1;
                    while (endSt < cleanBody.size() && (cleanBody[endSt] == ' ' || cleanBody[endSt] == '\t')) ++endSt;
                    if (endSt < cleanBody.size() && cleanBody[endSt] == ';') ++endSt;
                    for (std::size_t k = stPos; k < endSt; ++k)
                    {
                        if (rawBody[k] != '\n' && rawBody[k] != '\r')
                        {
                            rawBody[k] = ' ';
                            cleanBody[k] = ' ';
                        }
                    }
                    stPos = endSt;
                    continue;
                }
            }
            stPos += 6;
        }
    }

    std::ostringstream sanitized;
    std::size_t lpos = 0;
    int braceDepth = 0;
    while (lpos <= rawBody.size())
    {
        const std::size_t nl = rawBody.find('\n', lpos);
        const std::string_view rLine = (nl == std::string_view::npos) ? std::string_view(rawBody).substr(lpos)
                                                                      : std::string_view(rawBody).substr(lpos, nl - lpos);
        const std::string_view cLine = (nl == std::string_view::npos) ? std::string_view(cleanBody).substr(lpos)
                                                                      : std::string_view(cleanBody).substr(lpos, nl - lpos);
        const std::string_view cTrim = Trim(cLine);
        bool commentOutLine = false;
        if (cTrim.starts_with("#pragma"))
        {
            if (ioShading)
            {
                const std::string lp = LowerAscii(cTrim);
                if (lp.find("surface") != std::string::npos)
                {
                    if (lp.find("unlit") != std::string::npos) *ioShading = SurfaceShadingModel::Unlit;
                    else if (lp.find("foliage") != std::string::npos || lp.find("vegetation") != std::string::npos ||
                             lp.find("twosidedfoliage") != std::string::npos)
                    {
                        *ioShading = SurfaceShadingModel::Foliage;
                    }
                    else if (lp.find("toon") != std::string::npos || lp.find("lambert") != std::string::npos ||
                             lp.find("ramp") != std::string::npos)
                    {
                        *ioShading = SurfaceShadingModel::Toon;
                    }
                    else if (lp.find("standard") != std::string::npos || lp.find("lit") != std::string::npos ||
                             lp.find("pbr") != std::string::npos)
                    {
                        *ioShading = SurfaceShadingModel::Lit;
                    }
                }
            }
            commentOutLine = true;
        }
        else if (cTrim.starts_with("#include"))
        {
            const std::string li = LowerAscii(cTrim);
            if (li.find("unitycg.cginc") != std::string::npos || li.find("lighting.cginc") != std::string::npos ||
                li.find("autolight.cginc") != std::string::npos)
            {
                commentOutLine = true;
            }
        }
        else if (braceDepth == 0 && cTrim.ends_with(";") && cTrim.find('{') == std::string_view::npos)
        {
            // Only strip top-level redundant uniform redeclarations like "fixed4 _Color;" or "float4 _Color = float4(1,1,1,1);"
            std::string_view decl = Trim(cTrim.substr(0, cTrim.size() - 1));
            const std::size_t eqPos = decl.find('=');
            if (eqPos != std::string_view::npos) decl = Trim(decl.substr(0, eqPos));
            if (decl.find('(') == std::string_view::npos && decl.find(',') == std::string_view::npos)
            {
                const std::size_t firstSp = decl.find_first_of(" \t");
                const std::size_t lastSp = decl.find_last_of(" \t");
                if (firstSp != std::string_view::npos && lastSp != std::string_view::npos)
                {
                    const std::string_view firstTok = Trim(decl.substr(0, firstSp));
                    const std::string_view varName = Trim(decl.substr(lastSp + 1));
                    if (firstTok != "return")
                    {
                        if (varName == "_MainTex" || varName == "_BumpMap" || varName == "_MetallicGlossMap")
                        {
                            commentOutLine = true;
                        }
                        else
                        {
                            for (const auto& p : parameters)
                            {
                                if (p.name == varName) { commentOutLine = true; break; }
                            }
                            if (!commentOutLine)
                            {
                                for (const auto& t : textures)
                                {
                                    if (t.name == varName) { commentOutLine = true; break; }
                                }
                            }
                        }
                    }
                }
            }
        }

        if (!commentOutLine)
        {
            bool q = false;
            for (std::size_t k = 0; k < cLine.size(); ++k)
            {
                if (IsUnescapedQuote(cLine, k)) { q = !q; continue; }
                if (q) continue;
                if (cLine[k] == '{') ++braceDepth;
                else if (cLine[k] == '}') braceDepth = std::max(0, braceDepth - 1);
            }
        }

        if (commentOutLine) sanitized << "// " << rLine;
        else sanitized << rLine;
        if (nl == std::string_view::npos) break;
        sanitized << '\n';
        lpos = nl + 1;
    }
    return sanitized.str();
}

bool ParsePassRenderStates(std::string_view rawHeader, std::string_view cleanHeader,
                           SurfaceShaderPass& pass, bool& outSkipUtilityPass, std::string& errorMessage,
                           bool* outExplicitShading = nullptr)
{
    outSkipUtilityPass = false;
    std::string mutableClean(cleanHeader);

    // Inspect & blank out Tags { ... } blocks so quoted tag tokens do not confuse keyword tokenization
    {
        std::size_t tagPos = 0;
        while ((tagPos = FindKeywordOutsideQuotes(mutableClean, "Tags", tagPos)) != std::string_view::npos)
        {
            const std::size_t ob = mutableClean.find('{', tagPos + 4);
            const std::size_t cb = (ob != std::string_view::npos) ? FindMatchingBrace(mutableClean, ob, '{', '}') : std::string_view::npos;
            if (cb == std::string_view::npos) break;
            std::string compactTags;
            for (std::size_t k = ob + 1; k < cb; ++k)
            {
                if (!std::isspace(static_cast<unsigned char>(mutableClean[k])))
                    compactTags.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(mutableClean[k]))));
            }
            if (compactTags.find("\"lightmode\"=\"shadowcaster\"") != std::string::npos ||
                compactTags.find("\"lightmode\"=\"depthnormals\"") != std::string::npos ||
                compactTags.find("\"lightmode\"=\"meta\"") != std::string::npos)
            {
                outSkipUtilityPass = true;
            }
            if (compactTags.find("\"queue\"=\"transparent\"") != std::string::npos && !pass.blendPreset.has_value())
            {
                pass.blendPreset = SurfaceBlendPreset::Transparent;
            }
            else if ((compactTags.find("transparentcutout") != std::string::npos ||
                      compactTags.find("\"queue\"=\"alphatest\"") != std::string::npos) &&
                     !pass.blendPreset.has_value())
            {
                pass.blendPreset = SurfaceBlendPreset::Cutout;
            }
            std::fill(mutableClean.begin() + static_cast<std::ptrdiff_t>(tagPos),
                      mutableClean.begin() + static_cast<std::ptrdiff_t>(cb + 1), ' ');
            tagPos = cb + 1;
        }
    }

    // Inspect optional Name "..." declaration
    {
        const std::size_t nameKw = FindKeywordOutsideQuotes(mutableClean, "Name", 0);
        if (nameKw != std::string_view::npos)
        {
            std::size_t q1 = mutableClean.find('"', nameKw + 4);
            std::size_t q2 = (q1 != std::string_view::npos) ? mutableClean.find('"', q1 + 1) : std::string_view::npos;
            if (q1 != std::string_view::npos && q2 != std::string_view::npos && q2 < rawHeader.size())
            {
                pass.name = std::string(rawHeader.substr(q1 + 1, q2 - q1 - 1));
                std::fill(mutableClean.begin() + static_cast<std::ptrdiff_t>(nameKw),
                          mutableClean.begin() + static_cast<std::ptrdiff_t>(q2 + 1), ' ');
            }
        }
    }

    std::istringstream iss(mutableClean);
    std::string tok;
    while (iss >> tok)
    {
        const std::string lt = LowerAscii(tok);
        if (lt == "shading")
        {
            std::string mode;
            if (iss >> mode)
            {
                const std::string lm = LowerAscii(mode);
                if (lm == "lit" || lm == "standard" || lm == "pbr") pass.shadingModel = SurfaceShadingModel::Lit;
                else if (lm == "foliage" || lm == "vegetation") pass.shadingModel = SurfaceShadingModel::Foliage;
                else if (lm == "toon" || lm == "cel") pass.shadingModel = SurfaceShadingModel::Toon;
                else if (lm == "unlit") pass.shadingModel = SurfaceShadingModel::Unlit;
                else
                {
                    errorMessage = "Unknown Shading mode '" + mode + "' (expected Lit, Foliage, Toon, or Unlit).";
                    return false;
                }
                if (outExplicitShading) *outExplicitShading = true;
            }
        }
        else if (lt == "blend")
        {
            std::string mode;
            if (iss >> mode)
            {
                const std::string lm = LowerAscii(mode);
                if (lm == "opaque" || lm == "off") pass.blendPreset = SurfaceBlendPreset::Opaque;
                else if (lm == "cutout" || lm == "mask" || lm == "alphatest") pass.blendPreset = SurfaceBlendPreset::Cutout;
                else if (lm == "transparent" || lm == "blend" || lm == "alpha") pass.blendPreset = SurfaceBlendPreset::Transparent;
                else if (lm == "additive" || lm == "add") pass.blendPreset = SurfaceBlendPreset::Additive;
                else if (lm == "srcalpha" || lm == "one")
                {
                    std::string dstFactor;
                    (void)(iss >> dstFactor);
                    const bool hasSeparateAlpha = !dstFactor.empty() && dstFactor.back() == ',';
                    if (hasSeparateAlpha)
                    {
                        dstFactor.pop_back();
                        std::string srcA, dstA;
                        (void)(iss >> srcA >> dstA);
                    }
                    const std::string ld = LowerAscii(dstFactor);
                    if (ld == "one") pass.blendPreset = SurfaceBlendPreset::Additive;
                    else if (lm == "one" && ld == "zero") pass.blendPreset = SurfaceBlendPreset::Opaque;
                    else pass.blendPreset = SurfaceBlendPreset::Transparent;
                }
                else
                {
                    errorMessage = "Unknown Blend mode '" + mode + "' (expected Opaque, Cutout, Transparent, or Additive).";
                    return false;
                }
            }
        }
        else if (lt == "cull")
        {
            std::string mode;
            if (iss >> mode)
            {
                const std::string lm = LowerAscii(mode);
                if (lm == "back") pass.cullMode = SurfaceCullMode::Back;
                else if (lm == "off" || lm == "none") pass.cullMode = SurfaceCullMode::Off;
                else if (lm == "front") pass.cullMode = SurfaceCullMode::Front;
                else
                {
                    errorMessage = "Unknown Cull mode '" + mode + "' (expected Back, Front, or Off).";
                    return false;
                }
            }
        }
        else if (lt == "doublesided")
        {
            std::string val;
            if (iss >> val)
            {
                const std::string lv = LowerAscii(val);
                pass.cullMode = (lv == "on" || lv == "true" || lv == "1") ? SurfaceCullMode::Off : SurfaceCullMode::Back;
            }
        }
        else if (lt == "zwrite")
        {
            std::string val;
            if (iss >> val)
            {
                const std::string lv = LowerAscii(val);
                pass.zWrite = (lv == "on" || lv == "true" || lv == "1");
            }
        }
        else if (lt == "ztest")
        {
            std::string val;
            if (iss >> val)
            {
                const std::string lv = LowerAscii(val);
                if (lv == "lequal" || lv == "less" || lv == "lessequal") pass.zTest = SurfaceDepthTest::LessEqual;
                else if (lv == "greater" || lv == "gequal" || lv == "greaterequal") pass.zTest = SurfaceDepthTest::Greater;
                else if (lv == "always" || lv == "off" || lv == "disabled") pass.zTest = SurfaceDepthTest::Always;
                else
                {
                    errorMessage = "Unknown ZTest mode '" + val + "' (expected LEqual, Greater, or Always).";
                    return false;
                }
            }
        }
    }
    return true;
}

void FinalizeMetadataPasses(SurfaceShaderMetadata& output)
{
    output.shadowPassIndex.reset();
    output.usesAlphaCoverage = false;
    bool allTransparent = !output.passes.empty();

    for (std::size_t i = 0; i < output.passes.size(); ++i)
    {
        const SurfaceShaderPass& p = output.passes[i];
        const bool isTransOrAdd = p.blendPreset == SurfaceBlendPreset::Transparent || p.blendPreset == SurfaceBlendPreset::Additive;
        if (!isTransOrAdd) allTransparent = false;
        if (!p.blendPreset.has_value() || p.blendPreset == SurfaceBlendPreset::Cutout || p.blendPreset == SurfaceBlendPreset::Transparent)
        {
            output.usesAlphaCoverage = true;
        }
        const bool isCullFront = (p.cullMode == SurfaceCullMode::Front);
        const bool writesDepth = p.zWrite.value_or(true) && (p.zTest == SurfaceDepthTest::LessEqual);
        if (!output.shadowPassIndex.has_value() && !isTransOrAdd && !isCullFront && writesDepth)
        {
            output.shadowPassIndex = i;
        }
    }

    if (!output.passes.empty())
    {
        const SurfaceShaderPass& first = output.passes.front();
        output.shadingModel = first.shadingModel;
        output.usesUnitySurfaceEntry = first.usesUnitySurfaceEntry;
        output.usesUnityVertexEntry = first.usesUnityVertexEntry;
        output.bodyHlsl = first.bodyHlsl;
        output.bodyStartLine = first.bodyStartLine;
        if (first.cullMode.has_value())
        {
            output.doubleSidedOverride = (*first.cullMode == SurfaceCullMode::Off);
        }
        if (output.shadowPassIndex.has_value())
        {
            const SurfaceShaderPass& sp = output.passes[*output.shadowPassIndex];
            output.hasVertexHook = sp.hasVertexHook;
            if (sp.blendPreset == SurfaceBlendPreset::Cutout) output.blendOverride = MaterialAlphaMode::Mask;
            else if (sp.blendPreset == SurfaceBlendPreset::Opaque) output.blendOverride = MaterialAlphaMode::Opaque;
            else output.blendOverride.reset();
        }
        else
        {
            output.hasVertexHook = first.hasVertexHook;
            if (allTransparent) output.blendOverride = MaterialAlphaMode::Blend;
            else if (first.blendPreset == SurfaceBlendPreset::Cutout) output.blendOverride = MaterialAlphaMode::Mask;
            else if (first.blendPreset == SurfaceBlendPreset::Opaque) output.blendOverride = MaterialAlphaMode::Opaque;
        }
    }
}

// 解析 Unity ShaderLab 风格的单 Pass / 多 Pass 结构：
// Shader "Name" { Properties { ... } SubShader { [HLSLINCLUDE...ENDHLSL] [Pass { ... } ...] | [HLSLPROGRAM...ENDHLSL] } }
bool ParseUnityStyleShaderBlock(std::string_view hlslText, std::string_view cleanText,
                                SurfaceShaderMetadata& output, std::string& errorMessage)
{
    std::array<std::uint32_t, MaxSurfaceShaderVec4Slots> usedComponents{};

    // 1. Shader "DisplayName" { ... }
    const std::size_t shaderKw = FindKeywordOutsideQuotes(cleanText, "Shader", 0);
    if (shaderKw == std::string_view::npos) return false;
    const std::size_t firstBrace = cleanText.find('{', shaderKw + 6);
    if (firstBrace == std::string_view::npos)
    {
        errorMessage = "Missing '{' after Shader declaration.";
        return false;
    }
    const std::size_t shaderClose = FindMatchingBrace(cleanText, firstBrace, '{', '}');
    if (shaderClose == std::string_view::npos)
    {
        errorMessage = "Unclosed '{' on Shader block.";
        return false;
    }
    {
        std::string_view titleRegion = Trim(hlslText.substr(shaderKw + 6, firstBrace - (shaderKw + 6)));
        if (titleRegion.size() >= 2 && titleRegion.front() == '"' && titleRegion.back() == '"')
            titleRegion = titleRegion.substr(1, titleRegion.size() - 2);
        output.displayName = std::string(titleRegion);
    }

    // 2. Parse optional Properties { ... } block
    std::size_t propsStart = FindKeywordOutsideQuotes(cleanText, "Properties", firstBrace + 1);
    std::size_t propsOpen = std::string_view::npos;
    std::size_t propsClose = std::string_view::npos;
    const std::size_t firstSubKw = FindKeywordOutsideQuotes(cleanText, "SubShader", firstBrace + 1);
    const std::size_t firstProgKw = std::min(FindKeywordOutsideQuotes(cleanText, "HLSLPROGRAM", firstBrace + 1),
                                             FindKeywordOutsideQuotes(cleanText, "CGPROGRAM", firstBrace + 1));
    const std::size_t propsLimit = std::min({shaderClose, firstSubKw, firstProgKw});
    if (propsStart != std::string_view::npos && propsStart < propsLimit)
    {
        propsOpen = cleanText.find('{', propsStart + 10);
        if (propsOpen == std::string_view::npos || propsOpen >= shaderClose)
        {
            errorMessage = "Line " + std::to_string(LineNumberAt(hlslText, propsStart)) + ": missing '{' for Properties block.";
            return false;
        }
        propsClose = FindMatchingBrace(cleanText, propsOpen, '{', '}');
        if (propsClose == std::string_view::npos || propsClose >= shaderClose)
        {
            errorMessage = "Line " + std::to_string(LineNumberAt(hlslText, propsOpen)) + ": unclosed Properties '{' block.";
            return false;
        }

        std::size_t cur = propsOpen + 1;
        while (cur < propsClose)
        {
            while (cur < propsClose && std::isspace(static_cast<unsigned char>(cleanText[cur]))) ++cur;
            if (cur >= propsClose) break;

            bool hasToggleAttr = false;
            while (cur < propsClose && cleanText[cur] == '[')
            {
                const std::size_t rb = FindMatchingBrace(cleanText, cur, '[', ']');
                if (rb == std::string_view::npos || rb >= propsClose)
                {
                    errorMessage = "Line " + std::to_string(LineNumberAt(hlslText, cur)) + ": unclosed '[' attribute in Properties.";
                    return false;
                }
                const std::string attrLower = LowerAscii(Trim(cleanText.substr(cur + 1, rb - cur - 1)));
                if (attrLower.starts_with("toggle")) hasToggleAttr = true;
                cur = rb + 1;
                while (cur < propsClose && std::isspace(static_cast<unsigned char>(cleanText[cur]))) ++cur;
            }
            if (cur >= propsClose) break;

            const std::uint32_t propLine = LineNumberAt(hlslText, cur);
            const std::size_t idStart = cur;
            while (cur < propsClose && IsIdentChar(cleanText[cur])) ++cur;
            const std::string_view propName = hlslText.substr(idStart, cur - idStart);
            if (propName.empty())
            {
                errorMessage = "Line " + std::to_string(propLine) + ": expected property identifier in Properties block.";
                return false;
            }
            while (cur < propsClose && std::isspace(static_cast<unsigned char>(cleanText[cur]))) ++cur;
            if (cur >= propsClose || cleanText[cur] != '(')
            {
                errorMessage = "Line " + std::to_string(propLine) + ": expected '(\"Label\", Type)' after property '" + std::string(propName) + "'.";
                return false;
            }
            const std::size_t closeParen = FindMatchingBrace(cleanText, cur, '(', ')');
            if (closeParen == std::string_view::npos || closeParen >= propsClose)
            {
                errorMessage = "Line " + std::to_string(propLine) + ": unclosed '(' in property '" + std::string(propName) + "'.";
                return false;
            }

            const std::string_view rawInside = hlslText.substr(cur + 1, closeParen - cur - 1);
            const std::string_view cleanInside = cleanText.substr(cur + 1, closeParen - cur - 1);
            std::size_t commaPos = std::string_view::npos;
            {
                bool q = false;
                for (std::size_t k = 0; k < cleanInside.size(); ++k)
                {
                    if (cleanInside[k] == '"' && (k == 0 || cleanInside[k - 1] != '\\')) q = !q;
                    else if (!q && cleanInside[k] == ',') { commaPos = k; break; }
                }
            }
            if (commaPos == std::string_view::npos)
            {
                errorMessage = "Line " + std::to_string(propLine) + ": property '" + std::string(propName) + "' requires (\"Label\", Type).";
                return false;
            }
            std::string_view labelPart = Trim(rawInside.substr(0, commaPos));
            if (labelPart.size() >= 2 && labelPart.front() == '"' && labelPart.back() == '"')
                labelPart = labelPart.substr(1, labelPart.size() - 2);
            const std::string_view typePart = Trim(cleanInside.substr(commaPos + 1));

            cur = closeParen + 1;
            while (cur < propsClose && std::isspace(static_cast<unsigned char>(cleanText[cur]))) ++cur;
            std::string_view defPart{};
            if (cur < propsClose && cleanText[cur] == '=')
            {
                ++cur;
                while (cur < propsClose && (cleanText[cur] == ' ' || cleanText[cur] == '\t')) ++cur;
                const std::size_t valStart = cur;
                if (cur < propsClose && cleanText[cur] == '(')
                {
                    const std::size_t tupEnd = FindMatchingBrace(cleanText, cur, '(', ')');
                    if (tupEnd == std::string_view::npos || tupEnd >= propsClose)
                    {
                        errorMessage = "Line " + std::to_string(propLine) + ": unclosed default tuple '(' in '" + std::string(propName) + "'.";
                        return false;
                    }
                    cur = tupEnd + 1;
                    defPart = Trim(cleanText.substr(valStart, cur - valStart));
                }
                else if (cur < propsClose && cleanText[cur] == '"')
                {
                    ++cur;
                    while (cur < propsClose && !(cleanText[cur] == '"' && cleanText[cur - 1] != '\\')) ++cur;
                    if (cur < propsClose) ++cur;
                    defPart = Trim(hlslText.substr(valStart, cur - valStart));
                    while (cur < propsClose && std::isspace(static_cast<unsigned char>(cleanText[cur]))) ++cur;
                    if (cur < propsClose && cleanText[cur] == '{')
                    {
                        const std::size_t tb = FindMatchingBrace(cleanText, cur, '{', '}');
                        cur = (tb == std::string_view::npos) ? propsClose : (tb + 1);
                    }
                }
                else
                {
                    while (cur < propsClose && cleanText[cur] != '\n' && cleanText[cur] != '\r' && cleanText[cur] != ';')
                        ++cur;
                    defPart = Trim(cleanText.substr(valStart, cur - valStart));
                }
                if (cur < propsClose && cleanText[cur] == ';') ++cur;
            }
            while (cur < propsClose && (cleanText[cur] == ';' || cleanText[cur] == ',')) ++cur;

            if (!ParseTypedParam(propName, std::string(labelPart), typePart, defPart, hasToggleAttr, propLine,
                                 usedComponents, output, errorMessage))
            {
                return false;
            }
        }
    }

    // 3. Locate SubShader { ... } (or treat remaining Shader body as SubShader)
    const std::size_t searchAfterProps = (propsClose != std::string_view::npos) ? (propsClose + 1) : (firstBrace + 1);
    std::size_t subOpen = firstBrace;
    std::size_t subClose = shaderClose;
    const std::size_t subKw = FindKeywordOutsideQuotes(cleanText, "SubShader", searchAfterProps);
    if (subKw != std::string_view::npos && subKw < shaderClose)
    {
        subOpen = cleanText.find('{', subKw + 9);
        if (subOpen == std::string_view::npos || subOpen >= shaderClose)
        {
            errorMessage = "Line " + std::to_string(LineNumberAt(hlslText, subKw)) + ": missing '{' after SubShader.";
            return false;
        }
        subClose = FindMatchingBrace(cleanText, subOpen, '{', '}');
        if (subClose == std::string_view::npos || subClose > shaderClose)
        {
            errorMessage = "Line " + std::to_string(LineNumberAt(hlslText, subOpen)) + ": unclosed SubShader '{' block.";
            return false;
        }
    }

    std::string subClean(cleanText.substr(subOpen + 1, subClose - (subOpen + 1)));
    const std::size_t subBase = subOpen + 1;
    if (subOpen == firstBrace && propsOpen != std::string_view::npos && propsClose != std::string_view::npos)
    {
        const std::size_t relStart = (propsStart < subBase) ? 0 : (propsStart - subBase);
        const std::size_t relEnd = std::min(subClean.size(), (propsClose + 1) - subBase);
        if (relStart < relEnd)
            std::fill(subClean.begin() + static_cast<std::ptrdiff_t>(relStart),
                      subClean.begin() + static_cast<std::ptrdiff_t>(relEnd), ' ');
    }

    // 4. Extract optional HLSLINCLUDE ... ENDHLSL (or CGINCLUDE ... ENDCG) shared across all Passes
    std::string cleanStateText(cleanText);
    std::string sharedIncludeHlsl;
    std::uint32_t sharedIncludeLine = 1;
    {
        for (;;)
        {
            std::size_t incKw = FindKeywordOutsideQuotes(subClean, "HLSLINCLUDE", 0);
            std::string_view endTok = "ENDHLSL";
            std::size_t kwLen = 11;
            if (incKw == std::string_view::npos)
            {
                incKw = FindKeywordOutsideQuotes(subClean, "CGINCLUDE", 0);
                endTok = "ENDCG";
                kwLen = 9;
            }
            if (incKw == std::string_view::npos) break;
            const std::size_t incEnd = FindKeywordOutsideQuotes(subClean, endTok, incKw + kwLen);
            if (incEnd == std::string_view::npos)
            {
                errorMessage = "Missing matching " + std::string(endTok) + " for HLSLINCLUDE/CGINCLUDE block.";
                return false;
            }
            std::size_t absBodyStart = subBase + incKw + kwLen;
            if (absBodyStart < hlslText.size() && hlslText[absBodyStart] == '\r') ++absBodyStart;
            if (absBodyStart < hlslText.size() && hlslText[absBodyStart] == '\n') ++absBodyStart;
            const std::size_t absBodyEnd = subBase + incEnd;
            if (sharedIncludeHlsl.empty()) sharedIncludeLine = LineNumberAt(hlslText, absBodyStart);
            else sharedIncludeHlsl += "\n";
            sharedIncludeHlsl += SanitizeUserHlslBody(hlslText.substr(absBodyStart, absBodyEnd - absBodyStart), output.parameters, output.textures, nullptr);
            std::fill(subClean.begin() + static_cast<std::ptrdiff_t>(incKw),
                      subClean.begin() + static_cast<std::ptrdiff_t>(incEnd + endTok.size()), ' ');
            std::fill(cleanStateText.begin() + static_cast<std::ptrdiff_t>(subBase + incKw),
                      cleanStateText.begin() + static_cast<std::ptrdiff_t>(subBase + incEnd + endTok.size()), ' ');
        }
    }

    // 5. Discover Pass { ... } blocks inside SubShader (skipping over HLSLPROGRAM...ENDHLSL / CGPROGRAM...ENDCG)
    struct PassSpan { std::size_t kwOffset; std::size_t openOffset; std::size_t closeOffset; };
    std::vector<PassSpan> passSpans;
    {
        bool inQuote = false;
        for (std::size_t i = 0; i < subClean.size(); ++i)
        {
            const char c = subClean[i];
            if (c == '\n') { inQuote = false; continue; }
            if (IsUnescapedQuote(subClean, i)) { inQuote = !inQuote; continue; }
            if (inQuote) continue;
            if (FindKeywordOutsideQuotes(subClean, "HLSLPROGRAM", i) == i)
            {
                const std::size_t e = FindKeywordOutsideQuotes(subClean, "ENDHLSL", i + 11);
                if (e == std::string_view::npos) break;
                i = e + 6;
                continue;
            }
            if (FindKeywordOutsideQuotes(subClean, "CGPROGRAM", i) == i)
            {
                const std::size_t e = FindKeywordOutsideQuotes(subClean, "ENDCG", i + 9);
                if (e == std::string_view::npos) break;
                i = e + 4;
                continue;
            }
            if (FindKeywordOutsideQuotes(subClean, "Pass", i) == i)
            {
                std::size_t pOpen = i + 4;
                while (pOpen < subClean.size() && std::isspace(static_cast<unsigned char>(subClean[pOpen]))) ++pOpen;
                if (pOpen < subClean.size() && subClean[pOpen] == '{')
                {
                    const std::size_t pClose = FindMatchingBrace(subClean, pOpen, '{', '}');
                    if (pClose == std::string_view::npos)
                    {
                        errorMessage = "Line " + std::to_string(LineNumberAt(hlslText, subBase + i)) + ": unclosed Pass '{' block.";
                        return false;
                    }
                    passSpans.push_back({subBase + i, subBase + pOpen, subBase + pClose});
                    i = pClose;
                }
            }
        }
    }

    const auto compilePassSlice = [&](std::size_t sliceStart, std::size_t sliceEnd,
                                      const SurfaceShaderPass& baseState,
                                      bool baseExplicitShading,
                                      bool& outSkipped, SurfaceShaderPass& outPass) -> bool {
        outSkipped = false;
        outPass = baseState;
        bool hasExplicitShading = baseExplicitShading;
        const std::string_view rawSlice = hlslText.substr(sliceStart, sliceEnd - sliceStart);
        const std::string_view cleanSlice = std::string_view(cleanStateText).substr(sliceStart, sliceEnd - sliceStart);

        std::size_t pKw = FindKeywordOutsideQuotes(cleanSlice, "HLSLPROGRAM", 0);
        std::string_view endTok = "ENDHLSL";
        std::size_t kwLen = 11;
        if (pKw == std::string_view::npos)
        {
            pKw = FindKeywordOutsideQuotes(cleanSlice, "CGPROGRAM", 0);
            endTok = "ENDCG";
            kwLen = 9;
        }
        if (pKw == std::string_view::npos)
        {
            // Check if this is a Unity utility Pass (e.g. UsePass or ShadowCaster without HLSLPROGRAM)
            bool skipHdr = false;
            std::string tmpErr;
            (void)ParsePassRenderStates(rawSlice, cleanSlice, outPass, skipHdr, tmpErr);
            if (skipHdr || FindKeywordOutsideQuotes(cleanSlice, "UsePass", 0) != std::string_view::npos)
            {
                outSkipped = true;
                return true;
            }
            errorMessage = "Line " + std::to_string(LineNumberAt(hlslText, sliceStart)) + ": missing HLSLPROGRAM ... ENDHLSL block inside Pass.";
            return false;
        }
        const std::size_t pEnd = FindKeywordOutsideQuotes(cleanSlice, endTok, pKw + kwLen);
        if (pEnd == std::string_view::npos || pEnd <= pKw + kwLen)
        {
            errorMessage = "Line " + std::to_string(LineNumberAt(hlslText, sliceStart + pKw)) + ": missing matching " + std::string(endTok) + ".";
            return false;
        }

        if (!ParsePassRenderStates(rawSlice.substr(0, pKw), cleanSlice.substr(0, pKw), outPass, outSkipped, errorMessage, &hasExplicitShading))
            return false;
        if (outSkipped) return true;

        std::size_t absBody = sliceStart + pKw + kwLen;
        if (absBody < hlslText.size() && hlslText[absBody] == '\r') ++absBody;
        if (absBody < hlslText.size() && hlslText[absBody] == '\n') ++absBody;
        const std::size_t absEnd = sliceStart + pEnd;

        outPass.includeHlsl = sharedIncludeHlsl;
        outPass.includeStartLine = sharedIncludeLine;
        outPass.bodyStartLine = LineNumberAt(hlslText, absBody);
        outPass.bodyHlsl = SanitizeUserHlslBody(
            hlslText.substr(absBody, absEnd - absBody), output.parameters, output.textures, hasExplicitShading ? nullptr : &outPass.shadingModel);

        const std::string combinedClean = StripCommentsPreserveLines(outPass.includeHlsl + "\n" + outPass.bodyHlsl);
        outPass.usesUnitySurfaceEntry = HasFunctionDefinition(combinedClean, "surf");
        const bool hasFlareSurface = HasFunctionDefinition(combinedClean, "FlareSurface");
        if (!outPass.usesUnitySurfaceEntry && !hasFlareSurface)
        {
            errorMessage = "Line " + std::to_string(outPass.bodyStartLine) +
                           ": missing surface function 'void surf(Input IN, inout SurfaceOutput o)'.";
            return false;
        }
        outPass.usesUnityVertexEntry = HasFunctionDefinition(combinedClean, "vert", &outPass.vertHasOutInput);
        outPass.hasVertexHook = outPass.usesUnityVertexEntry || HasFunctionDefinition(combinedClean, "FlareVertex");
        return true;
    };

    if (passSpans.empty())
    {
        // Single-pass SubShader
        SurfaceShaderPass defaultState{};
        bool skipped = false;
        SurfaceShaderPass singlePass;
        // Mask out Properties if SubShader was implicit
        std::string singleRaw(hlslText.substr(subBase, subClose - subBase));
        if (subOpen == firstBrace && propsOpen != std::string_view::npos && propsClose != std::string_view::npos)
        {
            // Compile directly from subBase..subClose after propsClose+1
            const std::size_t afterProps = propsClose + 1;
            if (!compilePassSlice(afterProps, subClose, defaultState, false, skipped, singlePass)) return false;
        }
        else
        {
            if (!compilePassSlice(subBase, subClose, defaultState, false, skipped, singlePass)) return false;
        }
        if (skipped)
        {
            errorMessage = "Shader contains no active surface Pass.";
            return false;
        }
        output.passes.push_back(std::move(singlePass));
    }
    else
    {
        // Parse SubShader-level shared render states (outside all Pass { ... } blocks)
        std::string subHeaderClean = subClean;
        std::string subHeaderRaw(hlslText.substr(subBase, subClose - subBase));
        for (const auto& sp : passSpans)
        {
            const std::size_t relStart = sp.kwOffset - subBase;
            const std::size_t relEnd = (sp.closeOffset + 1) - subBase;
            std::fill(subHeaderClean.begin() + static_cast<std::ptrdiff_t>(relStart),
                      subHeaderClean.begin() + static_cast<std::ptrdiff_t>(relEnd), ' ');
            std::fill(subHeaderRaw.begin() + static_cast<std::ptrdiff_t>(relStart),
                      subHeaderRaw.begin() + static_cast<std::ptrdiff_t>(relEnd), ' ');
        }
        SurfaceShaderPass sharedState{};
        bool dummySkip = false;
        bool sharedExplicitShading = false;
        if (!ParsePassRenderStates(subHeaderRaw, subHeaderClean, sharedState, dummySkip, errorMessage, &sharedExplicitShading))
            return false;
        sharedState.name.clear();

        for (const auto& sp : passSpans)
        {
            bool skipped = false;
            SurfaceShaderPass pass;
            if (!compilePassSlice(sp.openOffset + 1, sp.closeOffset, sharedState, sharedExplicitShading, skipped, pass))
                return false;
            if (skipped) continue;
            if (output.passes.size() >= MaxSurfaceShaderPasses)
            {
                errorMessage = "Line " + std::to_string(LineNumberAt(hlslText, sp.kwOffset)) +
                               ": exceeded maximum Pass count (" + std::to_string(MaxSurfaceShaderPasses) + ").";
                return false;
            }
            output.passes.push_back(std::move(pass));
        }
        if (output.passes.empty())
        {
            errorMessage = "SubShader contains no active surface Pass.";
            return false;
        }
    }

    FinalizeMetadataPasses(output);
    return true;
}
} // namespace

SurfaceShaderPass SurfaceShaderMetadata::EffectivePass(std::size_t passIndex) const
{
    if (!passes.empty())
    {
        return passes[std::min(passIndex, passes.size() - 1)];
    }
    SurfaceShaderPass synth;
    synth.shadingModel = shadingModel;
    if (blendOverride.has_value())
    {
        synth.blendPreset = (*blendOverride == MaterialAlphaMode::Blend)
            ? SurfaceBlendPreset::Transparent
            : (*blendOverride == MaterialAlphaMode::Mask ? SurfaceBlendPreset::Cutout : SurfaceBlendPreset::Opaque);
    }
    if (doubleSidedOverride.has_value())
    {
        synth.cullMode = *doubleSidedOverride ? SurfaceCullMode::Off : SurfaceCullMode::Back;
    }
    synth.hasVertexHook = hasVertexHook;
    synth.usesUnitySurfaceEntry = usesUnitySurfaceEntry;
    synth.usesUnityVertexEntry = usesUnityVertexEntry;
    synth.bodyHlsl = bodyHlsl;
    synth.bodyStartLine = bodyStartLine;
    return synth;
}

bool IsSurfaceShaderHlslPath(const std::filesystem::path& path)
{
    const std::string lower = LowerAscii(asset_io::Utf8(path.filename()));
    return lower.ends_with(".fshader.hlsl");
}

bool IsShaderGraphPath(const std::filesystem::path& path)
{
    const std::string lower = LowerAscii(asset_io::Utf8(path.filename()));
    return lower.ends_with(".fshader.json");
}

bool IsUserSurfaceAssetPath(const std::filesystem::path& path)
{
    return IsSurfaceShaderHlslPath(path) || IsShaderGraphPath(path);
}

bool ParseSurfaceShaderSource(std::string_view hlslText, SurfaceShaderMetadata& output, std::string& errorMessage)
{
    output = {};
    errorMessage.clear();
    if (hlslText.empty() || hlslText.size() > 512 * 1024)
    {
        errorMessage = "Surface shader source is empty or exceeds 512 KB.";
        return false;
    }
    if (hlslText.size() >= 3 &&
        static_cast<unsigned char>(hlslText[0]) == 0xEF &&
        static_cast<unsigned char>(hlslText[1]) == 0xBB &&
        static_cast<unsigned char>(hlslText[2]) == 0xBF)
    {
        hlslText.remove_prefix(3);
    }

    // Prefer clean Unity-style Shader "Name" { Properties { ... } SubShader { ... HLSLPROGRAM ... ENDHLSL } }
    const std::string cleanWhole = StripCommentsPreserveLines(hlslText);
    {
        std::size_t firstNonWs = 0;
        while (firstNonWs < cleanWhole.size() && std::isspace(static_cast<unsigned char>(cleanWhole[firstNonWs])))
            ++firstNonWs;
        if (FindKeywordOutsideQuotes(cleanWhole, "Shader", firstNonWs) == firstNonWs)
        {
            return ParseUnityStyleShaderBlock(hlslText, cleanWhole, output, errorMessage);
        }
    }

    // Legacy // @FlareShader / // @Param comment-directive format (used by internal ShaderGraph codegen & existing assets)
    std::array<std::uint32_t, MaxSurfaceShaderVec4Slots> usedComponents{};
    std::size_t pos = 0;
    std::uint32_t lineNum = 0;
    while (pos <= hlslText.size())
    {
        ++lineNum;
        const std::size_t nl = hlslText.find('\n', pos);
        std::string_view line = (nl == std::string_view::npos) ? hlslText.substr(pos) : hlslText.substr(pos, nl - pos);
        std::string_view trimmed = Trim(line);
        if (trimmed.starts_with("//"))
        {
            std::string_view directive = Trim(trimmed.substr(2));
            if (directive.starts_with("@FlareShader"))
            {
                directive = Trim(directive.substr(12));
                std::string quoted = ExtractTrailingQuoted(directive);
                output.displayName = quoted.empty() ? std::string(directive) : std::move(quoted);
            }
            else if (directive.starts_with("@Shading"))
            {
                const std::string mode = LowerAscii(Trim(directive.substr(8)));
                if (mode == "lit") output.shadingModel = SurfaceShadingModel::Lit;
                else if (mode == "unlit") output.shadingModel = SurfaceShadingModel::Unlit;
                else if (mode == "toon") output.shadingModel = SurfaceShadingModel::Toon;
                else
                {
                    errorMessage = "Line " + std::to_string(lineNum) + ": unknown @Shading mode '" + mode + "'.";
                    return false;
                }
            }
            else if (directive.starts_with("@Blend"))
            {
                const std::string mode = LowerAscii(Trim(directive.substr(6)));
                if (mode == "opaque") output.blendOverride = MaterialAlphaMode::Opaque;
                else if (mode == "mask" || mode == "cutout") output.blendOverride = MaterialAlphaMode::Mask;
                else if (mode == "blend" || mode == "transparent") output.blendOverride = MaterialAlphaMode::Blend;
                else
                {
                    errorMessage = "Line " + std::to_string(lineNum) + ": unknown @Blend mode '" + mode + "'.";
                    return false;
                }
            }
            else if (directive.starts_with("@DoubleSided"))
            {
                const std::string val = LowerAscii(Trim(directive.substr(12)));
                output.doubleSidedOverride = (val == "true" || val == "1" || val == "on");
            }
            else if (directive.starts_with("@Param"))
            {
                std::string_view rest = Trim(directive.substr(6));
                std::string label = ExtractTrailingQuoted(rest);
                const std::size_t space = rest.find_first_of(" \t");
                if (space == std::string_view::npos)
                {
                    errorMessage = "Line " + std::to_string(lineNum) + ": malformed @Param declaration.";
                    return false;
                }
                const std::string_view nameView = rest.substr(0, space);
                rest = Trim(rest.substr(space + 1));
                const std::size_t eq = rest.find('=');
                const std::string_view typeSpec = Trim(eq == std::string_view::npos ? rest : rest.substr(0, eq));
                const std::string_view defSpec = Trim(eq == std::string_view::npos ? std::string_view{} : rest.substr(eq + 1));
                if (!ParseTypedParam(nameView, std::move(label), typeSpec, defSpec, false, lineNum,
                                     usedComponents, output, errorMessage))
                {
                    return false;
                }
            }
        }
        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }

    output.bodyHlsl = std::string(hlslText);
    output.bodyStartLine = 1;
    output.usesUnitySurfaceEntry = HasFunctionDefinition(cleanWhole, "surf");
    const bool hasFlareSurf = HasFunctionDefinition(cleanWhole, "FlareSurface");
    if (!output.usesUnitySurfaceEntry && !hasFlareSurf)
    {
        errorMessage = "Missing surface function: void surf(Input IN, inout SurfaceOutput o)";
        return false;
    }
    output.usesUnityVertexEntry = HasFunctionDefinition(cleanWhole, "vert");
    output.hasVertexHook = output.usesUnityVertexEntry || HasFunctionDefinition(cleanWhole, "FlareVertex");

    SurfaceShaderPass legacyPass;
    legacyPass.shadingModel = output.shadingModel;
    if (output.blendOverride.has_value())
    {
        legacyPass.blendPreset = (*output.blendOverride == MaterialAlphaMode::Blend)
            ? SurfaceBlendPreset::Transparent
            : (*output.blendOverride == MaterialAlphaMode::Mask ? SurfaceBlendPreset::Cutout : SurfaceBlendPreset::Opaque);
    }
    if (output.doubleSidedOverride.has_value())
    {
        legacyPass.cullMode = *output.doubleSidedOverride ? SurfaceCullMode::Off : SurfaceCullMode::Back;
    }
    legacyPass.hasVertexHook = output.hasVertexHook;
    legacyPass.usesUnitySurfaceEntry = output.usesUnitySurfaceEntry;
    legacyPass.usesUnityVertexEntry = output.usesUnityVertexEntry;
    legacyPass.bodyHlsl = output.bodyHlsl;
    legacyPass.bodyStartLine = output.bodyStartLine;
    output.passes.push_back(std::move(legacyPass));
    FinalizeMetadataPasses(output);
    return true;
}

std::array<Vec4, MaxSurfaceShaderVec4Slots> PackSurfaceShaderParameters(
    const SurfaceShaderMetadata& metadata,
    const std::map<std::string, Vec4>& overrides)
{
    std::array<Vec4, MaxSurfaceShaderVec4Slots> packed{};
    for (const SurfaceShaderParam& param : metadata.parameters)
    {
        if (param.slotIndex >= MaxSurfaceShaderVec4Slots) continue;
        const auto it = overrides.find(param.name);
        const Vec4 val = (it != overrides.end()) ? it->second : param.defaultValue;
        float* dst = &packed[param.slotIndex].x;
        const float src[4]{val.x, val.y, val.z, val.w};
        for (std::uint32_t c = 0; c < param.componentCount && (param.componentOffset + c) < 4; ++c)
        {
            float v = src[c];
            if (param.kind == SurfaceParamKind::Range)
                v = std::clamp(v, param.minValue, param.maxValue);
            dst[param.componentOffset + c] = v;
        }
    }
    return packed;
}

std::string MakeDefaultSurfaceShaderTemplate(std::string_view displayName)
{
    const std::string title = displayName.empty() ? "NewSurfaceShader" : std::string(displayName);
    std::ostringstream out;
    out << "Shader \"" << title << "\"\n"
        << "{\n"
        << "    Properties\n"
        << "    {\n"
        << "        _Color      (\"基础颜色\", Color)       = (1, 1, 1, 1)\n"
        << "        _Metallic   (\"金属度\",   Range(0, 1)) = 0.0\n"
        << "        _Smoothness (\"光滑度\",   Range(0, 1)) = 0.5\n"
        << "    }\n"
        << "\n"
        << "    SubShader\n"
        << "    {\n"
        << "        // 光照: Lit (写实PBR) | Toon (卡通) | Unlit (无光照)\n"
        << "        // 混合: Opaque (不透明) | Cutout (镂空) | Transparent (半透明)\n"
        << "        // 剔除: Back (单面) | Off (双面)\n"
        << "        Shading Lit\n"
        << "        Blend   Opaque\n"
        << "        Cull    Back\n"
        << "\n"
        << "        HLSLPROGRAM\n"
        << "\n"
        << "        void surf(Input IN, inout SurfaceOutput o)\n"
        << "        {\n"
        << "            float4 c = tex2D(_MainTex, IN.uv) * _Color;\n"
        << "            o.Albedo     = c.rgb;\n"
        << "            o.Metallic   = _Metallic;\n"
        << "            o.Smoothness = _Smoothness;\n"
        << "            o.Alpha      = c.a;\n"
        << "        }\n"
        << "\n"
        << "        ENDHLSL\n"
        << "    }\n"
        << "}\n";
    return out.str();
}

namespace
{
// 仅替换独立的属性标识符引用，跳过成员访问符 '.' 右侧（如 o.Metallic / IN.uv）与注释/字符串字面量，
// 既保留 1:1 源码行号，又彻底消除与 SurfaceOutput / Input 成员同名的宏展开冲突。
std::string ExpandUserParamReferences(std::string_view code, const std::vector<SurfaceShaderParam>& parameters)
{
    if (parameters.empty() || code.empty()) return std::string(code);
    std::string out;
    out.reserve(code.size() + 128);
    bool inQuote = false;
    bool inLineComment = false;
    bool inBlockComment = false;
    bool inDirectiveLine = false;
    bool atLineStart = true;
    char prevNonSpace = '\0';

    for (std::size_t i = 0; i < code.size();)
    {
        const char c = code[i];
        if (c == '\n')
        {
            out.push_back(c);
            inLineComment = false;
            inQuote = false;
            inDirectiveLine = false;
            atLineStart = true;
            ++i;
            continue;
        }
        if (inLineComment)
        {
            out.push_back(c);
            ++i;
            continue;
        }
        if (inBlockComment)
        {
            if (c == '*' && i + 1 < code.size() && code[i + 1] == '/')
            {
                out.append("*/");
                inBlockComment = false;
                i += 2;
            }
            else
            {
                out.push_back(c);
                ++i;
            }
            continue;
        }
        if (!inQuote && c == '/' && i + 1 < code.size() && code[i + 1] == '/')
        {
            out.append("//");
            inLineComment = true;
            i += 2;
            continue;
        }
        if (!inQuote && c == '/' && i + 1 < code.size() && code[i + 1] == '*')
        {
            out.append("/*");
            inBlockComment = true;
            i += 2;
            continue;
        }
        if (IsUnescapedQuote(code, i))
        {
            inQuote = !inQuote;
            out.push_back(c);
            atLineStart = false;
            prevNonSpace = '"';
            ++i;
            continue;
        }
        if (inQuote || inDirectiveLine)
        {
            out.push_back(c);
            ++i;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r')
        {
            out.push_back(c);
            ++i;
            continue;
        }
        if (atLineStart && c == '#')
        {
            inDirectiveLine = true;
            atLineStart = false;
            out.push_back(c);
            ++i;
            continue;
        }
        atLineStart = false;

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')
        {
            std::size_t end = i + 1;
            while (end < code.size() && IsIdentChar(code[end])) ++end;
            const std::string_view ident = code.substr(i, end - i);
            std::size_t nextNonSpace = end;
            while (nextNonSpace < code.size() && (code[nextNonSpace] == ' ' || code[nextNonSpace] == '\t')) ++nextNonSpace;
            const bool isMemberAccess = (prevNonSpace == '.');
            const bool isFuncCall = (nextNonSpace < code.size() && code[nextNonSpace] == '(');
            const SurfaceShaderParam* matched = nullptr;
            if (!isMemberAccess && !isFuncCall)
            {
                for (const auto& p : parameters)
                {
                    if (p.name == ident) { matched = &p; break; }
                }
            }
            if (matched)
            {
                out += "(flareUserParams[" + std::to_string(matched->slotIndex) + "]." +
                       std::string(SwizzleFor(matched->componentOffset, matched->componentCount)) + ")";
                prevNonSpace = ')';
            }
            else
            {
                out.append(ident);
                prevNonSpace = ident.back();
            }
            i = end;
            continue;
        }

        out.push_back(c);
        prevNonSpace = c;
        ++i;
    }
    return out;
}
} // namespace

std::string BuildSurfaceShaderUserInclude(const SurfaceShaderMetadata& metadata,
                                          std::string_view sourceDebugName,
                                          std::size_t passIndex)
{
    const SurfaceShaderPass pass = metadata.EffectivePass(passIndex);
    std::ostringstream out;
    out << "// Auto-generated Flare Surface Shader bridge\n";
    out << "#define FLARE_PASS_INDEX " << passIndex << "\n";
    out << "#define FLARE_SHADING_MODE " << static_cast<int>(pass.shadingModel) << "\n";
    if (pass.hasVertexHook)
    {
        out << "#define FLARE_HAS_VERTEX 1\n";
    }
    out << "#define Input FlareSurfInput\n";
    out << "#define SurfaceOutput FlareSurfOutput\n";
    out << "#define SurfaceOutputStandard FlareSurfOutput\n";
    out << "#define VertexInput FlareVertexData\n";
    out << "#define fixed float\n";
    out << "#define fixed2 float2\n";
    out << "#define fixed3 float3\n";
    out << "#define fixed4 float4\n";
    out << "#define tex2D(slot, uv) FlareSampleTex2D((slot), (uv))\n";
    out << "#define tex2Dlod(slot, uvLod) FlareSampleTex2DLod((slot), (uvLod))\n";
    out << "#define tex2Dgrad(slot, uv, dx, dy) FlareSampleTex2DGrad((slot), (uv), (dx), (dy))\n";
    for (const auto& tex : metadata.textures)
    {
        out << "static const uint " << tex.name << " = " << (3u + tex.slotIndex) << "u;\n";
    }
    std::string sanitizedName(sourceDebugName);
    for (char& ch : sanitizedName)
    {
        if (ch == '\\') ch = '/';
        if (ch == '"') ch = '\'';
    }
    if (!pass.includeHlsl.empty())
    {
        out << "#line " << pass.includeStartLine << " \"" << sanitizedName << "\"\n";
        out << ExpandUserParamReferences(pass.includeHlsl, metadata.parameters) << "\n";
    }
    out << "#line " << pass.bodyStartLine << " \"" << sanitizedName << "\"\n";
    out << ExpandUserParamReferences(pass.bodyHlsl, metadata.parameters) << "\n";
    out << "#line 1 \"FlareGeneratedBridge\"\n";

    if (pass.usesUnityVertexEntry)
    {
        out << "\nvoid FlareVertex(inout FlareVertexInput V)\n"
            << "{\n"
            << "    FlareVertexData v = (FlareVertexData)0;\n"
            << "    v.vertex   = V.localPos;\n"
            << "    v.normal   = V.localNormal;\n"
            << "    v.uv       = V.uv;\n"
            << "    v.texcoord = V.uv;\n";
        if (pass.vertHasOutInput)
        {
            out << "    FlareSurfInput dummyOut = (FlareSurfInput)0;\n"
                << "    vert(v, dummyOut);\n";
        }
        else
        {
            out << "    vert(v);\n";
        }
        out << "    V.localPos    = v.vertex;\n"
            << "    V.localNormal = v.normal;\n"
            << "}\n";
    }
    if (pass.usesUnitySurfaceEntry)
    {
        const bool isCutout = (pass.blendPreset == SurfaceBlendPreset::Cutout);
        const bool forceSolidAlpha = (pass.blendPreset == SurfaceBlendPreset::Opaque) ||
                                     isCutout ||
                                     (!pass.blendPreset.has_value() &&
                                      ((pass.zWrite.has_value() && !*pass.zWrite) ||
                                       pass.zTest != SurfaceDepthTest::LessEqual));
        out << "\nvoid FlareSurface(in FlareSurfaceInput srcIn, inout FlareSurfaceOutput dstOut)\n"
            << "{\n"
            << "    FlareSurfInput IN = (FlareSurfInput)0;\n"
            << "    IN.uv             = srcIn.uv;\n"
            << "    IN.uv_MainTex     = srcIn.uv;\n"
            << "    IN.uv_BumpMap     = srcIn.uv;\n"
            << "    IN.worldPos       = srcIn.worldPos;\n"
            << "    IN.localPos       = srcIn.localPos;\n"
            << "    IN.objectPos      = srcIn.objectPos;\n"
            << "    IN.cameraPos      = srcIn.cameraPos;\n"
            << "    IN.worldNormal    = srcIn.worldNormal;\n"
            << "    IN.worldTangent   = srcIn.worldTangent;\n"
            << "    IN.worldBitangent = srcIn.worldBitangent;\n"
            << "    IN.viewDir        = srcIn.viewDir;\n"
            << "    IN.lightDir       = srcIn.lightDir;\n"
            << "    IN.lightColor     = srcIn.lightColor;\n"
            << "    IN.screenUV       = srcIn.screenUv;\n"
            << "    IN.screenPos      = srcIn.screenPos;\n"
            << "    IN.pixelPos       = srcIn.pixelPos;\n"
            << "    IN.color          = srcIn.vertexColor;\n"
            << "    IN.facing         = srcIn.facing;\n"
            << "    IN.time           = srcIn.time;\n"
            << "    float initRough  = dstOut.roughness;\n"
            << "    float initSmooth = 1.0 - initRough;\n"
            << "    FlareSurfOutput o = (FlareSurfOutput)0;\n"
            << "    o.Albedo      = dstOut.baseColor;\n"
            << "    o.Normal      = dstOut.normalTS;\n"
            << "    o.Emission    = dstOut.emissive;\n"
            << "    o.Metallic    = dstOut.metallic;\n"
            << "    o.Smoothness  = initSmooth;\n"
            << "    o.Roughness   = initRough;\n"
            << "    o.Occlusion   = dstOut.occlusion;\n"
            << "    o.Specular    = dstOut.specular;\n"
            << "    o.Translucency= dstOut.translucency;\n"
            << "    o.Alpha       = dstOut.opacity;\n"
            << "    o.AlphaCutoff = dstOut.alphaCutoff;\n"
            << "    surf(IN, o);\n";
        if (isCutout)
        {
            out << "    clip(o.Alpha - o.AlphaCutoff);\n";
        }
        out << "    dstOut.baseColor   = o.Albedo;\n"
            << "    dstOut.normalTS    = o.Normal;\n"
            << "    dstOut.emissive    = o.Emission;\n"
            << "    dstOut.metallic    = o.Metallic;\n"
            << "    dstOut.roughness   = (abs(o.Roughness - initRough) > 1e-5) ? o.Roughness : (1.0 - saturate(o.Smoothness));\n"
            << "    dstOut.occlusion   = o.Occlusion;\n"
            << "    dstOut.specular    = o.Specular;\n"
            << "    dstOut.translucency= o.Translucency;\n"
            << "    dstOut.opacity     = " << (forceSolidAlpha ? "1.0" : "o.Alpha") << ";\n"
            << "    dstOut.alphaCutoff = o.AlphaCutoff;\n"
            << "}\n";
    }
    out << "#undef tex2Dgrad\n#undef tex2Dlod\n#undef tex2D\n";
    out << "#undef fixed4\n#undef fixed3\n#undef fixed2\n#undef fixed\n";
    out << "#undef VertexInput\n#undef SurfaceOutputStandard\n#undef SurfaceOutput\n#undef Input\n";
    return out.str();
}

bool StageSurfaceShaderVariant(const std::filesystem::path& builtinShadersDir,
                               const std::filesystem::path& cacheRootDir,
                               const std::filesystem::path& userSourcePath,
                               const SurfaceShaderMetadata& metadata,
                               std::string_view baseBuiltinVariantFile,
                               std::filesystem::path& outVariantHlslPath,
                               std::string& outContentKey,
                               std::string& errorMessage,
                               std::size_t passIndex)
{
    errorMessage.clear();
    if (builtinShadersDir.empty() || cacheRootDir.empty() || baseBuiltinVariantFile.empty())
    {
        errorMessage = "Invalid shader staging directory.";
        return false;
    }

    const std::string canonicalInclude = BuildSurfaceShaderUserInclude(metadata, "UserSurface.fshader.hlsl", passIndex);
    outContentKey = Sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(canonicalInclude.data()), canonicalInclude.size()));
    if (outContentKey.empty())
    {
        errorMessage = "Failed to hash user surface shader.";
        return false;
    }
    const std::string debugName = userSourcePath.empty() ? "UserSurface.fshader.hlsl"
                                                         : asset_io::Utf8(userSourcePath);
    const std::string userIncludeText = BuildSurfaceShaderUserInclude(metadata, debugName, passIndex);

    const auto tryStageInto = [&](const std::filesystem::path& rootCand) -> bool {
        const std::filesystem::path stageDir = rootCand / ("S_" + outContentKey.substr(0, 16));
        std::error_code ec;
        std::filesystem::create_directories(stageDir, ec);
        if (ec) return false;

        for (const auto& entry : std::filesystem::directory_iterator(builtinShadersDir, ec))
        {
            if (ec || !entry.is_regular_file()) continue;
            const auto ext = LowerAscii(asset_io::Utf8(entry.path().extension()));
            if (ext == ".hlsl" || ext == ".hlsli")
            {
                if (!CopyIfDifferent(entry.path(), stageDir / entry.path().filename())) return false;
            }
        }

        const std::filesystem::path userHeaderPath = stageDir / "FlareUserSurfaceBody.hlsli";
        const std::span<const std::uint8_t> userBytes(
            reinterpret_cast<const std::uint8_t*>(userIncludeText.data()), userIncludeText.size());
        std::vector<std::uint8_t> existingUser;
        if (!std::filesystem::exists(userHeaderPath) || !asset_io::Read(userHeaderPath, existingUser) ||
            existingUser.size() != userBytes.size() ||
            std::memcmp(existingUser.data(), userBytes.data(), userBytes.size()) != 0)
        {
            if (!asset_io::WriteAtomic(userHeaderPath, userBytes)) return false;
        }

        const std::string wrapperName = "User_" + std::string(baseBuiltinVariantFile);
        const std::filesystem::path candidateVariant = stageDir / wrapperName;
        const std::string wrapperText =
            "#define SE_USER_SURFACE 1\n"
            "#include \"" + std::string(baseBuiltinVariantFile) + "\"\n";
        const std::span<const std::uint8_t> wrapBytes(
            reinterpret_cast<const std::uint8_t*>(wrapperText.data()), wrapperText.size());
        std::vector<std::uint8_t> existingWrap;
        if (!std::filesystem::exists(candidateVariant) || !asset_io::Read(candidateVariant, existingWrap) ||
            existingWrap.size() != wrapBytes.size() ||
            std::memcmp(existingWrap.data(), wrapBytes.data(), wrapBytes.size()) != 0)
        {
            if (!asset_io::WriteAtomic(candidateVariant, wrapBytes)) return false;
        }
        outVariantHlslPath = candidateVariant;
        return true;
    };

    if (tryStageInto(cacheRootDir)) return true;

    std::error_code tempEc;
    const std::filesystem::path tempRoot = std::filesystem::temp_directory_path(tempEc) / "FlareEngine" / "user-shaders";
    if (!tempEc && tryStageInto(tempRoot)) return true;

    errorMessage = "Failed to stage user shader variant into cache or temp directory.";
    return false;
}
} // namespace flare
