#pragma once

#include "PlatformLayer/GamepadState.h"
#include <array>
#include <cstdint>

namespace flare
{
// 将 XInput 有符号 16 位轴量 [-32768, 32767] 归一化到 [-1.0, 1.0]。
[[nodiscard]] float NormalizeXInputThumbAxis(std::int16_t raw) noexcept;

// 将 XInput 原始字段解码为平台无关的 GamepadState。
[[nodiscard]] GamepadState DecodeXInputGamepad(
    std::uint16_t buttons,
    std::uint8_t leftTrigger,
    std::uint8_t rightTrigger,
    std::int16_t thumbLX,
    std::int16_t thumbLY,
    std::int16_t thumbRX,
    std::int16_t thumbRY) noexcept;

// 动态探测并采集 0..3 号手柄状态（无 xinput1_4.dll 或未插入手柄时安全清零返回）。
void PollWindowsGamepads(std::array<GamepadState, 4>& outStates) noexcept;
} // namespace flare
