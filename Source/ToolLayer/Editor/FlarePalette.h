#pragma once

#include <cstdint>

namespace se::flare
{
// sRGB 界面色，取自 FlareMark 图标：曜石黑底 + 香槟金。金色只做点睛（焦点、激活、主操作），大面积保持中性暗色。
struct Color { std::uint8_t r, g, b; };
inline constexpr Color Background{13,14,18}; // 图标底色，加载屏、根画布与停靠缝隙共用。
inline constexpr Color Surface{24,26,32};
inline constexpr Color Raised{33,36,44};
inline constexpr Color Border{44,47,56};
inline constexpr Color Hover{40,43,52};
inline constexpr Color Selected{66,53,31};
inline constexpr Color Heading{244,241,234};
inline constexpr Color Text{206,207,212};
inline constexpr Color Muted{128,132,144};
inline constexpr Color Disabled{84,88,98};
inline constexpr Color Silver{178,180,188};
inline constexpr Color Gold{254,206,123}; // B 款图标亮部的中位色。
inline constexpr Color Accent{232,184,98}; // 主按钮底色；其上文字用 Background。
inline constexpr Color AccentHover{254,206,123};
inline constexpr Color Danger{190,78,78};
inline constexpr Color Success{70,150,104};
inline constexpr Color Warning{214,152,64};

// 密集桌面工具的颜色：字段低于面板一层，焦点与对象选中分别绘制。
namespace workspace
{
inline constexpr Color Panel{22,24,30};
inline constexpr Color Field{17,18,23};
inline constexpr Color Header{28,30,37};
inline constexpr Color Divider{38,41,50};
inline constexpr Color Hover{44,46,56};
inline constexpr Color Pressed{92,72,38};
inline constexpr Color Focus{254,206,123};
inline constexpr Color Selection{78,62,34};
inline constexpr Color InactiveSelection{42,40,38};
inline constexpr Color AxisX{204,96,96};
inline constexpr Color AxisY{100,172,124};
inline constexpr Color AxisZ{98,142,208};
inline constexpr Color PlayTint{18,36,29};
inline constexpr Color PauseTint{46,36,19};
}

template<class T>
constexpr T Rgba(Color color, float alpha = 1.0f)
{
    return {color.r/255.0f, color.g/255.0f, color.b/255.0f, alpha};
}
}
