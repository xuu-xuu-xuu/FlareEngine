#pragma once

#include "FunctionLayer/UI/Core/UiStyle.h"
#include "ToolLayer/Editor/FlarePalette.h"
#include <cmath>

namespace flare::ui::editor
{
// 工具包画笔使用线性 RGB。色表及交互设计使用 sRGB。
inline UiColor UiInk(flare::palette::Color color, float alpha = 1.0f)
{
    const auto linear = [](std::uint8_t channel) {
        const float value = channel / 255.0f;
        return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
    };
    return {linear(color.r), linear(color.g), linear(color.b), alpha};
}
}
