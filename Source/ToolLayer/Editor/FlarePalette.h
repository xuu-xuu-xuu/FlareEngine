#pragma once

#include <cstdint>

namespace se::flare
{
// sRGB 界面色，含义见《引擎自带UI风格》。在绘制边界转换为各后端需要的表示。
struct Color { std::uint8_t r, g, b; };
inline constexpr Color Background{22,24,28};
inline constexpr Color Surface{28,31,37};
inline constexpr Color Raised{38,42,50};
inline constexpr Color Border{52,58,70};
inline constexpr Color Hover{54,61,74};
inline constexpr Color Selected{42,92,168};
inline constexpr Color Heading{240,243,248};
inline constexpr Color Text{214,219,228};
inline constexpr Color Muted{148,158,176};
inline constexpr Color Disabled{104,112,126};
inline constexpr Color Silver{194,202,215};
inline constexpr Color Gold{254,206,123}; // B 款图标亮部的中位色。
inline constexpr Color Accent{59,138,245};
inline constexpr Color AccentHover{78,152,252};
inline constexpr Color Danger{176,56,62};
inline constexpr Color Success{46,148,88};
inline constexpr Color Warning{198,136,42};

// 密集桌面工具的颜色：字段低于面板一层，焦点与对象选中分别绘制。
namespace workspace
{
inline constexpr Color Panel{30,33,40};
inline constexpr Color Field{19,21,26};
inline constexpr Color Header{38,42,51};
inline constexpr Color Divider{52,58,70};
inline constexpr Color Hover{54,61,75};
inline constexpr Color Pressed{38,78,128};
inline constexpr Color Focus{59,138,245};
inline constexpr Color Selection{42,92,168};
inline constexpr Color InactiveSelection{44,50,62};
inline constexpr Color AxisX{196,58,58};
inline constexpr Color AxisY{48,150,74};
inline constexpr Color AxisZ{52,112,208};
inline constexpr Color PlayTint{26,48,36};
inline constexpr Color PauseTint{52,42,24};
}

template<class T>
constexpr T Rgba(Color color, float alpha = 1.0f)
{
    return {color.r/255.0f, color.g/255.0f, color.b/255.0f, alpha};
}
}
