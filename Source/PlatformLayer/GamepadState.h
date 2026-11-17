#pragma once

#include "CoreLayer/Math/Math.h"
#include <cstdint>

// XInput 标准按键位掩码定义，纯值常量，零平台头依赖。
namespace GamepadButton
{
inline constexpr std::uint32_t DpadUp        = 0x0001u;
inline constexpr std::uint32_t DpadDown      = 0x0002u;
inline constexpr std::uint32_t DpadLeft      = 0x0004u;
inline constexpr std::uint32_t DpadRight     = 0x0008u;
inline constexpr std::uint32_t Start         = 0x0010u;
inline constexpr std::uint32_t Back          = 0x0020u;
inline constexpr std::uint32_t LeftThumb     = 0x0040u;
inline constexpr std::uint32_t RightThumb    = 0x0080u;
inline constexpr std::uint32_t LeftShoulder  = 0x0100u;
inline constexpr std::uint32_t RightShoulder = 0x0200u;
inline constexpr std::uint32_t A             = 0x1000u;
inline constexpr std::uint32_t B             = 0x2000u;
inline constexpr std::uint32_t X             = 0x4000u;
inline constexpr std::uint32_t Y             = 0x8000u;
} // namespace GamepadButton

// 单个游戏手柄帧快照（纯 POD 值类型，供 FunctionLayer 安全消费）。
struct GamepadState
{
    std::uint32_t buttons = 0;
    se::Vec2 leftStick{};
    se::Vec2 rightStick{};
    float leftTrigger = 0.0f;
    float rightTrigger = 0.0f;
    bool connected = false;
};
