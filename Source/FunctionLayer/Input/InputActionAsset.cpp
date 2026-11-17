#include "FunctionLayer/Input/InputActionAsset.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <set>
#include <unordered_set>

namespace se
{
namespace
{
using Json = nlohmann::json;

constexpr std::size_t MaxInputFileBytes = 1024 * 1024;
constexpr std::size_t MaxActions = 1024;
constexpr std::size_t MaxBindingsPerAction = 128;

std::string LowerAscii(std::string_view text)
{
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

bool ValidActionName(std::string_view name) noexcept
{
    if (name.empty() || name.size() > 128) return false;
    return std::none_of(name.begin(), name.end(), [](unsigned char ch) { return ch < 32; });
}

const char* KindName(InputActionKind kind) noexcept
{
    switch (kind)
    {
    case InputActionKind::Button: return "button";
    case InputActionKind::Axis1D: return "axis1d";
    case InputActionKind::Axis2D: return "axis2d";
    }
    return "button";
}

bool ParseActionKind(std::string_view text, InputActionKind& out) noexcept
{
    const std::string lower = LowerAscii(text);
    if (lower == "button") { out = InputActionKind::Button; return true; }
    if (lower == "axis1d") { out = InputActionKind::Axis1D; return true; }
    if (lower == "axis2d") { out = InputActionKind::Axis2D; return true; }
    return false;
}

const char* DeviceName(InputDeviceKind device) noexcept
{
    switch (device)
    {
    case InputDeviceKind::Keyboard: return "keyboard";
    case InputDeviceKind::Mouse: return "mouse";
    case InputDeviceKind::Gamepad: return "gamepad";
    }
    return "keyboard";
}

bool ParseDeviceKind(std::string_view text, InputDeviceKind& out) noexcept
{
    const std::string lower = LowerAscii(text);
    if (lower == "keyboard") { out = InputDeviceKind::Keyboard; return true; }
    if (lower == "mouse") { out = InputDeviceKind::Mouse; return true; }
    if (lower == "gamepad") { out = InputDeviceKind::Gamepad; return true; }
    return false;
}

const char* MouseControlName(MouseControl control) noexcept
{
    switch (control)
    {
    case MouseControl::Left: return "left";
    case MouseControl::Right: return "right";
    case MouseControl::Middle: return "middle";
    case MouseControl::Wheel: return "wheel";
    case MouseControl::None: break;
    }
    return "left";
}

bool ParseMouseControl(const Json& node, MouseControl& out) noexcept
{
    if (node.is_number_integer())
    {
        const auto idx = node.get<std::int64_t>();
        if (idx == 0) { out = MouseControl::Left; return true; }
        if (idx == 1) { out = MouseControl::Right; return true; }
        if (idx == 2) { out = MouseControl::Middle; return true; }
        return false;
    }
    if (!node.is_string()) return false;
    const std::string lower = LowerAscii(node.get<std::string>());
    if (lower == "left" || lower == "lmb" || lower == "0") { out = MouseControl::Left; return true; }
    if (lower == "right" || lower == "rmb" || lower == "1") { out = MouseControl::Right; return true; }
    if (lower == "middle" || lower == "mmb" || lower == "2") { out = MouseControl::Middle; return true; }
    if (lower == "wheel") { out = MouseControl::Wheel; return true; }
    return false;
}

const char* GamepadControlName(GamepadControl control) noexcept
{
    switch (control)
    {
    case GamepadControl::DpadUp: return "DpadUp";
    case GamepadControl::DpadDown: return "DpadDown";
    case GamepadControl::DpadLeft: return "DpadLeft";
    case GamepadControl::DpadRight: return "DpadRight";
    case GamepadControl::Start: return "Start";
    case GamepadControl::Back: return "Back";
    case GamepadControl::LeftThumb: return "LeftThumb";
    case GamepadControl::RightThumb: return "RightThumb";
    case GamepadControl::LeftShoulder: return "LeftShoulder";
    case GamepadControl::RightShoulder: return "RightShoulder";
    case GamepadControl::A: return "A";
    case GamepadControl::B: return "B";
    case GamepadControl::X: return "X";
    case GamepadControl::Y: return "Y";
    case GamepadControl::LeftTrigger: return "LeftTrigger";
    case GamepadControl::RightTrigger: return "RightTrigger";
    case GamepadControl::LeftStick: return "LeftStick";
    case GamepadControl::RightStick: return "RightStick";
    case GamepadControl::LeftStickX: return "LeftStickX";
    case GamepadControl::LeftStickY: return "LeftStickY";
    case GamepadControl::RightStickX: return "RightStickX";
    case GamepadControl::RightStickY: return "RightStickY";
    case GamepadControl::None: break;
    }
    return "A";
}

bool ParseGamepadControl(std::string_view text, GamepadControl& out) noexcept
{
    const std::string lower = LowerAscii(text);
    if (lower == "dpadup") { out = GamepadControl::DpadUp; return true; }
    if (lower == "dpaddown") { out = GamepadControl::DpadDown; return true; }
    if (lower == "dpadleft") { out = GamepadControl::DpadLeft; return true; }
    if (lower == "dpadright") { out = GamepadControl::DpadRight; return true; }
    if (lower == "start") { out = GamepadControl::Start; return true; }
    if (lower == "back") { out = GamepadControl::Back; return true; }
    if (lower == "leftthumb" || lower == "ls") { out = GamepadControl::LeftThumb; return true; }
    if (lower == "rightthumb" || lower == "rs") { out = GamepadControl::RightThumb; return true; }
    if (lower == "leftshoulder" || lower == "lb") { out = GamepadControl::LeftShoulder; return true; }
    if (lower == "rightshoulder" || lower == "rb") { out = GamepadControl::RightShoulder; return true; }
    if (lower == "a") { out = GamepadControl::A; return true; }
    if (lower == "b") { out = GamepadControl::B; return true; }
    if (lower == "x") { out = GamepadControl::X; return true; }
    if (lower == "y") { out = GamepadControl::Y; return true; }
    if (lower == "lefttrigger" || lower == "lt") { out = GamepadControl::LeftTrigger; return true; }
    if (lower == "righttrigger" || lower == "rt") { out = GamepadControl::RightTrigger; return true; }
    if (lower == "leftstick") { out = GamepadControl::LeftStick; return true; }
    if (lower == "rightstick") { out = GamepadControl::RightStick; return true; }
    if (lower == "leftstickx") { out = GamepadControl::LeftStickX; return true; }
    if (lower == "leftsticky") { out = GamepadControl::LeftStickY; return true; }
    if (lower == "rightstickx") { out = GamepadControl::RightStickX; return true; }
    if (lower == "rightsticky") { out = GamepadControl::RightStickY; return true; }
    return false;
}

bool ValidateBinding(const InputBinding& b) noexcept
{
    if (!std::isfinite(b.axisX) || !std::isfinite(b.axisY) || !std::isfinite(b.scale)) return false;
    switch (b.device)
    {
    case InputDeviceKind::Keyboard:
        return b.keyCode < 256u;
    case InputDeviceKind::Mouse:
        return b.mouseControl != MouseControl::None;
    case InputDeviceKind::Gamepad:
        return b.gamepadControl != GamepadControl::None && b.gamepadIndex < 4u;
    }
    return false;
}

InputBinding KeyboardDir(std::uint32_t key, float x, float y) noexcept
{
    InputBinding b;
    b.device = InputDeviceKind::Keyboard;
    b.keyCode = key;
    b.axisX = x;
    b.axisY = y;
    b.scale = 1.0f;
    return b;
}
} // namespace

InputActionDocument DefaultInputActionDocument()
{
    InputActionDocument doc;
    doc.version = InputActionDocumentVersion;

    InputActionDefinition move;
    move.name = "Move";
    move.kind = InputActionKind::Axis2D;
    move.deadZone = 0.15f;
    move.bindings = {
        KeyboardDir('A', -1.0f, 0.0f),
        KeyboardDir('D', 1.0f, 0.0f),
        KeyboardDir('S', 0.0f, -1.0f),
        KeyboardDir('W', 0.0f, 1.0f),
        KeyboardDir(0x25, -1.0f, 0.0f), // VK_LEFT
        KeyboardDir(0x27, 1.0f, 0.0f),  // VK_RIGHT
        KeyboardDir(0x28, 0.0f, -1.0f), // VK_DOWN
        KeyboardDir(0x26, 0.0f, 1.0f),  // VK_UP
        InputBinding{InputDeviceKind::Gamepad, 0, MouseControl::None, GamepadControl::LeftStick, 0, 1.0f, 1.0f, 1.0f}
    };

    InputActionDefinition look;
    look.name = "Look";
    look.kind = InputActionKind::Axis2D;
    look.deadZone = 0.15f;
    look.bindings = {
        InputBinding{InputDeviceKind::Gamepad, 0, MouseControl::None, GamepadControl::RightStick, 0, 1.0f, 1.0f, 1.0f}
    };

    InputActionDefinition jump;
    jump.name = "Jump";
    jump.kind = InputActionKind::Button;
    jump.deadZone = 0.15f;
    jump.bindings = {
        KeyboardDir(0x20, 1.0f, 0.0f), // VK_SPACE
        InputBinding{InputDeviceKind::Gamepad, 0, MouseControl::None, GamepadControl::A, 0, 1.0f, 0.0f, 1.0f}
    };

    InputActionDefinition fire;
    fire.name = "Fire";
    fire.kind = InputActionKind::Button;
    fire.deadZone = 0.20f;
    fire.bindings = {
        InputBinding{InputDeviceKind::Mouse, 0, MouseControl::Left, GamepadControl::None, 0, 1.0f, 0.0f, 1.0f},
        InputBinding{InputDeviceKind::Gamepad, 0, MouseControl::None, GamepadControl::RightTrigger, 0, 1.0f, 0.0f, 1.0f}
    };

    InputActionDefinition interact;
    interact.name = "Interact";
    interact.kind = InputActionKind::Button;
    interact.deadZone = 0.15f;
    interact.bindings = {
        KeyboardDir('E', 1.0f, 0.0f),
        InputBinding{InputDeviceKind::Gamepad, 0, MouseControl::None, GamepadControl::X, 0, 1.0f, 0.0f, 1.0f}
    };

    doc.actions = {std::move(move), std::move(look), std::move(jump), std::move(fire), std::move(interact)};
    return doc;
}

bool ValidateInputActionDocument(const InputActionDocument& document, std::string& errorKey)
{
    if (document.version != InputActionDocumentVersion)
    {
        errorKey = "Input.UnsupportedVersion";
        return false;
    }
    if (document.actions.size() > MaxActions)
    {
        errorKey = "Input.InvalidDocument";
        return false;
    }

    std::unordered_set<std::string> seenNames;
    seenNames.reserve(document.actions.size());
    for (const InputActionDefinition& action : document.actions)
    {
        if (!ValidActionName(action.name) || !seenNames.emplace(action.name).second ||
            !std::isfinite(action.deadZone) || action.deadZone < 0.0f || action.deadZone >= 1.0f ||
            action.bindings.size() > MaxBindingsPerAction)
        {
            errorKey = "Input.InvalidDocument";
            return false;
        }
        for (const InputBinding& binding : action.bindings)
        {
            if (!ValidateBinding(binding))
            {
                errorKey = "Input.InvalidBinding";
                return false;
            }
        }
    }

    errorKey.clear();
    return true;
}

bool ParseInputActionDocument(std::string_view jsonText, InputActionDocument& outDocument, std::string& errorKey)
{
    errorKey.clear();
    if (jsonText.empty() || jsonText.size() > MaxInputFileBytes)
    {
        errorKey = "Input.InvalidDocument";
        return false;
    }

    try
    {
        bool duplicate = false;
        std::vector<std::set<std::string>> keyStack;
        const Json root = Json::parse(jsonText.begin(), jsonText.end(), [&](int depth, Json::parse_event_t event, Json& val) {
            if (depth > 32) throw std::runtime_error("Input.InvalidDocument");
            if (event == Json::parse_event_t::object_start) keyStack.emplace_back();
            else if (event == Json::parse_event_t::key && !keyStack.empty())
            {
                if (!keyStack.back().insert(val.get<std::string>()).second) duplicate = true;
            }
            else if (event == Json::parse_event_t::object_end && !keyStack.empty()) keyStack.pop_back();
            return true;
        });

        if (duplicate || !root.is_object() ||
            root.value("format", std::string{}) != "SelfEngine.InputActions" ||
            !root.contains("version") || !root["version"].is_number_integer())
        {
            errorKey = "Input.InvalidDocument";
            return false;
        }
        const auto version = root["version"].get<std::int64_t>();
        if (version != InputActionDocumentVersion)
        {
            errorKey = "Input.UnsupportedVersion";
            return false;
        }
        if (!root.contains("actions") || !root["actions"].is_array())
        {
            errorKey = "Input.InvalidDocument";
            return false;
        }

        InputActionDocument doc;
        doc.version = static_cast<std::uint32_t>(version);
        for (const Json& actionJson : root["actions"])
        {
            if (!actionJson.is_object() || !actionJson.contains("name") || !actionJson["name"].is_string())
            {
                errorKey = "Input.InvalidDocument";
                return false;
            }
            InputActionDefinition action;
            action.name = actionJson["name"].get<std::string>();
            if (actionJson.contains("kind"))
            {
                if (!actionJson["kind"].is_string() ||
                    !ParseActionKind(actionJson["kind"].get<std::string>(), action.kind))
                {
                    errorKey = "Input.InvalidDocument";
                    return false;
                }
            }
            if (actionJson.contains("deadZone"))
            {
                if (!actionJson["deadZone"].is_number())
                {
                    errorKey = "Input.InvalidDocument";
                    return false;
                }
                action.deadZone = actionJson["deadZone"].get<float>();
            }
            if (actionJson.contains("bindings"))
            {
                if (!actionJson["bindings"].is_array())
                {
                    errorKey = "Input.InvalidDocument";
                    return false;
                }
                for (const Json& bindingJson : actionJson["bindings"])
                {
                    if (!bindingJson.is_object() || !bindingJson.contains("device") || !bindingJson["device"].is_string())
                    {
                        errorKey = "Input.InvalidBinding";
                        return false;
                    }
                    InputBinding binding;
                    if (!ParseDeviceKind(bindingJson["device"].get<std::string>(), binding.device))
                    {
                        errorKey = "Input.InvalidBinding";
                        return false;
                    }
                    if (binding.device == InputDeviceKind::Keyboard)
                    {
                        if (!bindingJson.contains("key") || !bindingJson["key"].is_number_integer())
                        {
                            errorKey = "Input.InvalidBinding";
                            return false;
                        }
                        const auto code = bindingJson["key"].get<std::int64_t>();
                        if (code < 0 || code >= 256)
                        {
                            errorKey = "Input.InvalidBinding";
                            return false;
                        }
                        binding.keyCode = static_cast<std::uint32_t>(code);
                    }
                    else if (binding.device == InputDeviceKind::Mouse)
                    {
                        const Json* ctrlNode = bindingJson.contains("control") ? &bindingJson["control"]
                            : (bindingJson.contains("button") ? &bindingJson["button"] : nullptr);
                        if (!ctrlNode || !ParseMouseControl(*ctrlNode, binding.mouseControl))
                        {
                            errorKey = "Input.InvalidBinding";
                            return false;
                        }
                    }
                    else
                    {
                        if (!bindingJson.contains("control") || !bindingJson["control"].is_string() ||
                            !ParseGamepadControl(bindingJson["control"].get<std::string>(), binding.gamepadControl))
                        {
                            errorKey = "Input.InvalidBinding";
                            return false;
                        }
                        if (bindingJson.contains("gamepadIndex"))
                        {
                            if (!bindingJson["gamepadIndex"].is_number_integer())
                            {
                                errorKey = "Input.InvalidBinding";
                                return false;
                            }
                            const auto idx = bindingJson["gamepadIndex"].get<std::int64_t>();
                            if (idx < 0 || idx >= 4)
                            {
                                errorKey = "Input.InvalidBinding";
                                return false;
                            }
                            binding.gamepadIndex = static_cast<std::uint32_t>(idx);
                        }
                    }

                    if (bindingJson.contains("axisX"))
                    {
                        if (!bindingJson["axisX"].is_number()) { errorKey = "Input.InvalidBinding"; return false; }
                        binding.axisX = bindingJson["axisX"].get<float>();
                    }
                    if (bindingJson.contains("axisY"))
                    {
                        if (!bindingJson["axisY"].is_number()) { errorKey = "Input.InvalidBinding"; return false; }
                        binding.axisY = bindingJson["axisY"].get<float>();
                    }
                    if (bindingJson.contains("scale"))
                    {
                        if (!bindingJson["scale"].is_number()) { errorKey = "Input.InvalidBinding"; return false; }
                        binding.scale = bindingJson["scale"].get<float>();
                    }
                    action.bindings.push_back(binding);
                }
            }
            doc.actions.push_back(std::move(action));
        }

        if (!ValidateInputActionDocument(doc, errorKey)) return false;
        outDocument = std::move(doc);
        errorKey.clear();
        return true;
    }
    catch (const Json::exception&)
    {
        errorKey = "Input.InvalidDocument";
        return false;
    }
    catch (const std::exception&)
    {
        errorKey = "Input.InvalidDocument";
        return false;
    }
}

bool SerializeInputActionDocument(const InputActionDocument& document, std::string& outJson, std::string& errorKey)
{
    if (!ValidateInputActionDocument(document, errorKey)) return false;

    Json actionsJson = Json::array();
    for (const InputActionDefinition& action : document.actions)
    {
        Json bindingsJson = Json::array();
        for (const InputBinding& b : action.bindings)
        {
            Json item{
                {"device", DeviceName(b.device)},
                {"axisX", b.axisX},
                {"axisY", b.axisY},
                {"scale", b.scale}
            };
            if (b.device == InputDeviceKind::Keyboard)
            {
                item["key"] = b.keyCode;
            }
            else if (b.device == InputDeviceKind::Mouse)
            {
                item["control"] = MouseControlName(b.mouseControl);
            }
            else
            {
                item["control"] = GamepadControlName(b.gamepadControl);
                item["gamepadIndex"] = b.gamepadIndex;
            }
            bindingsJson.push_back(std::move(item));
        }
        actionsJson.push_back({
            {"name", action.name},
            {"kind", KindName(action.kind)},
            {"deadZone", action.deadZone},
            {"bindings", std::move(bindingsJson)}
        });
    }

    const Json docJson{
        {"format", "SelfEngine.InputActions"},
        {"version", document.version},
        {"actions", std::move(actionsJson)}
    };
    outJson = docJson.dump(2) + "\n";
    errorKey.clear();
    return true;
}

bool ReadInputActionDocument(const std::filesystem::path& path, InputActionDocument& outDocument, std::string& errorKey)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec)
    {
        errorKey = "Input.ReadFailed";
        return false;
    }
    const auto bytesSize = std::filesystem::file_size(path, ec);
    if (ec || bytesSize > MaxInputFileBytes)
    {
        errorKey = "Input.ReadFailed";
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        errorKey = "Input.ReadFailed";
        return false;
    }
    const std::string content{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    if (file.bad())
    {
        errorKey = "Input.ReadFailed";
        return false;
    }
    return ParseInputActionDocument(content, outDocument, errorKey);
}

bool WriteInputActionDocument(const std::filesystem::path& path, const InputActionDocument& document, std::string& errorKey)
{
    std::string text;
    if (!SerializeInputActionDocument(document, text, errorKey)) return false;

    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty())
    {
        std::filesystem::create_directories(parent, ec);
        if (ec) { errorKey = "Input.WriteFailed"; return false; }
    }

    std::filesystem::path tempPath = path;
    tempPath += ".tmp";
    {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) { errorKey = "Input.WriteFailed"; return false; }
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out.good())
        {
            out.close();
            std::filesystem::remove(tempPath, ec);
            errorKey = "Input.WriteFailed";
            return false;
        }
    }

    std::filesystem::rename(tempPath, path, ec);
    if (ec)
    {
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(tempPath, path, ec);
    }
    if (ec)
    {
        std::filesystem::remove(tempPath, ec);
        errorKey = "Input.WriteFailed";
        return false;
    }

    errorKey.clear();
    return true;
}
} // namespace se
