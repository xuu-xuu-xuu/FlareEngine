#include "PlatformLayer/Windows/WindowsGamepad.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>

namespace se
{
namespace
{
#pragma pack(push, 1)
struct RawXInputGamepad
{
    WORD buttons;
    BYTE leftTrigger;
    BYTE rightTrigger;
    SHORT thumbLX;
    SHORT thumbLY;
    SHORT thumbRX;
    SHORT thumbRY;
};

struct RawXInputState
{
    DWORD packetNumber;
    RawXInputGamepad gamepad;
};
#pragma pack(pop)

static_assert(sizeof(RawXInputGamepad) == 12);
static_assert(sizeof(RawXInputState) == 16);

using XInputGetStateFn = DWORD (WINAPI*)(DWORD userIndex, RawXInputState* state);

XInputGetStateFn ResolveXInputGetState() noexcept
{
    static const XInputGetStateFn cachedFn = []() noexcept -> XInputGetStateFn {
        HMODULE module = LoadLibraryW(L"xinput1_4.dll");
        if (!module)
        {
            module = LoadLibraryW(L"xinput9_1_0.dll");
        }
        return module ? reinterpret_cast<XInputGetStateFn>(GetProcAddress(module, "XInputGetState")) : nullptr;
    }();
    return cachedFn;
}
} // namespace

float NormalizeXInputThumbAxis(std::int16_t raw) noexcept
{
    if (raw == -32768) return -1.0f;
    return std::clamp(static_cast<float>(raw) / 32767.0f, -1.0f, 1.0f);
}

GamepadState DecodeXInputGamepad(
    std::uint16_t buttons,
    std::uint8_t leftTrigger,
    std::uint8_t rightTrigger,
    std::int16_t thumbLX,
    std::int16_t thumbLY,
    std::int16_t thumbRX,
    std::int16_t thumbRY) noexcept
{
    GamepadState out{};
    out.connected = true;
    out.buttons = static_cast<std::uint32_t>(buttons);
    out.leftTrigger = std::clamp(static_cast<float>(leftTrigger) / 255.0f, 0.0f, 1.0f);
    out.rightTrigger = std::clamp(static_cast<float>(rightTrigger) / 255.0f, 0.0f, 1.0f);
    out.leftStick = {NormalizeXInputThumbAxis(thumbLX), NormalizeXInputThumbAxis(thumbLY)};
    out.rightStick = {NormalizeXInputThumbAxis(thumbRX), NormalizeXInputThumbAxis(thumbRY)};
    return out;
}

void PollWindowsGamepads(std::array<GamepadState, 4>& outStates) noexcept
{
    const XInputGetStateFn getState = ResolveXInputGetState();
    if (!getState)
    {
        outStates.fill(GamepadState{});
        return;
    }

    static std::array<ULONGLONG, 4> nextDisconnectedRetryTick{};
    const ULONGLONG nowTick = GetTickCount64();

    for (DWORD i = 0; i < static_cast<DWORD>(outStates.size()); ++i)
    {
        if (!outStates[i].connected && nowTick < nextDisconnectedRetryTick[i])
        {
            continue;
        }
        RawXInputState raw{};
        if (getState(i, &raw) == ERROR_SUCCESS)
        {
            nextDisconnectedRetryTick[i] = 0;
            outStates[i] = DecodeXInputGamepad(
                raw.gamepad.buttons,
                raw.gamepad.leftTrigger,
                raw.gamepad.rightTrigger,
                raw.gamepad.thumbLX,
                raw.gamepad.thumbLY,
                raw.gamepad.thumbRX,
                raw.gamepad.thumbRY);
        }
        else
        {
            nextDisconnectedRetryTick[i] = nowTick + 500;
            outStates[i] = GamepadState{};
        }
    }
}
} // namespace se
