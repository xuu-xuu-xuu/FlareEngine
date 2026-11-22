#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace flare
{
inline constexpr std::uint32_t InputActionDocumentVersion = 1;
inline constexpr std::string_view InputActionsRelativePath = "Settings/Input.input.json";

enum class InputActionKind
{
    Button,
    Axis1D,
    Axis2D
};

enum class InputDeviceKind
{
    Keyboard,
    Mouse,
    Gamepad
};

enum class MouseControl
{
    None,
    Left,
    Right,
    Middle,
    Wheel
};

enum class GamepadControl
{
    None,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    Start,
    Back,
    LeftThumb,
    RightThumb,
    LeftShoulder,
    RightShoulder,
    A,
    B,
    X,
    Y,
    LeftTrigger,
    RightTrigger,
    LeftStick,
    RightStick,
    LeftStickX,
    LeftStickY,
    RightStickX,
    RightStickY
};

struct InputBinding
{
    InputDeviceKind device = InputDeviceKind::Keyboard;
    std::uint32_t keyCode = 0; // Keyboard 虚拟键码 0..255。
    MouseControl mouseControl = MouseControl::None;
    GamepadControl gamepadControl = GamepadControl::None;
    std::uint32_t gamepadIndex = 0; // 手柄槽位 0..3。
    float axisX = 1.0f;
    float axisY = 0.0f;
    float scale = 1.0f;
};

struct InputActionDefinition
{
    std::string name;
    InputActionKind kind = InputActionKind::Button;
    float deadZone = 0.15f;
    std::vector<InputBinding> bindings;
};

struct InputActionDocument
{
    std::uint32_t version = InputActionDocumentVersion;
    std::vector<InputActionDefinition> actions;
};

[[nodiscard]] InputActionDocument DefaultInputActionDocument();
[[nodiscard]] bool ValidateInputActionDocument(const InputActionDocument& document, std::string& errorKey);
[[nodiscard]] bool ParseInputActionDocument(std::string_view jsonText, InputActionDocument& outDocument, std::string& errorKey);
[[nodiscard]] bool SerializeInputActionDocument(const InputActionDocument& document, std::string& outJson, std::string& errorKey);
[[nodiscard]] bool ReadInputActionDocument(const std::filesystem::path& path, InputActionDocument& outDocument, std::string& errorKey);
[[nodiscard]] bool WriteInputActionDocument(const std::filesystem::path& path, const InputActionDocument& document, std::string& errorKey);
} // namespace flare
