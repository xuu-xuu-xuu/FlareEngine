#pragma once
#include <cstdint>
#include <string_view>

namespace flare
{
// CPU 材质、导入与 RHI 共用的覆盖语义；数值固定，不按渲染 Pass 另建一套枚举。
enum class MaterialAlphaMode : std::uint8_t { Opaque=0,Mask=1,Blend=2 };
constexpr bool IsValidMaterialAlphaMode(MaterialAlphaMode mode) noexcept
{return mode==MaterialAlphaMode::Opaque||mode==MaterialAlphaMode::Mask||mode==MaterialAlphaMode::Blend;}
constexpr std::string_view MaterialAlphaModeName(MaterialAlphaMode mode) noexcept
{
    switch(mode)
    {
    case MaterialAlphaMode::Opaque:return "OPAQUE";
    case MaterialAlphaMode::Mask:return "MASK";
    case MaterialAlphaMode::Blend:return "BLEND";
    default:return {};
    }
}
constexpr bool ParseMaterialAlphaMode(std::string_view value,MaterialAlphaMode& output) noexcept
{
    if(value=="OPAQUE")output=MaterialAlphaMode::Opaque;
    else if(value=="MASK")output=MaterialAlphaMode::Mask;
    else if(value=="BLEND")output=MaterialAlphaMode::Blend;
    else return false;
    return true;
}
}
