#include "FunctionLayer/Input/InputActionMap.h"

#include <algorithm>
#include <cmath>

namespace se
{
namespace
{
float ApplyScalarDeadZone(float value, float deadZone) noexcept
{
    const float mag = std::abs(value);
    if (mag <= deadZone || deadZone >= 0.999f) return 0.0f;
    const float scaled = std::clamp((mag - deadZone) / (1.0f - deadZone), 0.0f, 1.0f);
    return std::copysign(scaled, value);
}

Vec2 ApplyRadialDeadZone(Vec2 stick, float deadZone) noexcept
{
    const float len = std::hypot(stick.x, stick.y);
    if (len <= deadZone || len <= 1e-6f || deadZone >= 0.999f) return {};
    const float clamped = std::min(len, 1.0f);
    const float scaled = std::clamp((clamped - deadZone) / (1.0f - deadZone), 0.0f, 1.0f);
    const float factor = scaled / len;
    return {stick.x * factor, stick.y * factor};
}

std::uint32_t GamepadMaskForControl(GamepadControl control) noexcept
{
    switch (control)
    {
    case GamepadControl::DpadUp: return GamepadButton::DpadUp;
    case GamepadControl::DpadDown: return GamepadButton::DpadDown;
    case GamepadControl::DpadLeft: return GamepadButton::DpadLeft;
    case GamepadControl::DpadRight: return GamepadButton::DpadRight;
    case GamepadControl::Start: return GamepadButton::Start;
    case GamepadControl::Back: return GamepadButton::Back;
    case GamepadControl::LeftThumb: return GamepadButton::LeftThumb;
    case GamepadControl::RightThumb: return GamepadButton::RightThumb;
    case GamepadControl::LeftShoulder: return GamepadButton::LeftShoulder;
    case GamepadControl::RightShoulder: return GamepadButton::RightShoulder;
    case GamepadControl::A: return GamepadButton::A;
    case GamepadControl::B: return GamepadButton::B;
    case GamepadControl::X: return GamepadButton::X;
    case GamepadControl::Y: return GamepadButton::Y;
    default: return 0;
    }
}

bool KeyboardResetInFrame(const InputState& state) noexcept
{
    for (const KeyEvent& ev : state.keyEvents)
    {
        if (ev.type == KeyEventType::Reset) return true;
    }
    return false;
}

bool PointerCancelledInFrame(const InputState& state) noexcept
{
    for (const PointerEvent& ev : state.pointerEvents)
    {
        if (ev.type == PointerEventType::Cancel) return true;
    }
    return false;
}

bool MouseControlDown(const InputState& state, MouseControl control) noexcept
{
    if (PointerCancelledInFrame(state)) return false;
    switch (control)
    {
    case MouseControl::Left: return state.mouseButtons[0];
    case MouseControl::Right: return state.mouseButtons[1];
    case MouseControl::Middle: return state.mouseButtons[2];
    case MouseControl::Wheel:
        for (const PointerEvent& ev : state.pointerEvents)
        {
            if (ev.type == PointerEventType::Wheel && std::abs(ev.wheel) > 1e-6f) return true;
        }
        return false;
    case MouseControl::None: break;
    }
    return false;
}

bool MouseControlPressed(const InputState& state, MouseControl control) noexcept
{
    if (PointerCancelledInFrame(state)) return false;
    if (control == MouseControl::Wheel)
    {
        return MouseControlDown(state, control);
    }
    const unsigned targetBtn = control == MouseControl::Left ? 0u
        : (control == MouseControl::Right ? 1u : (control == MouseControl::Middle ? 2u : 99u));
    if (targetBtn >= 3u) return false;
    for (const PointerEvent& ev : state.pointerEvents)
    {
        if (ev.type == PointerEventType::Down && ev.button == targetBtn) return true;
    }
    return false;
}

bool GamepadControlDown(const GamepadState& gp, GamepadControl control, float deadZone) noexcept
{
    if (!gp.connected) return false;
    if (const std::uint32_t mask = GamepadMaskForControl(control); mask != 0)
    {
        return (gp.buttons & mask) != 0;
    }
    const float threshold = std::max(0.5f, deadZone);
    switch (control)
    {
    case GamepadControl::LeftTrigger: return gp.leftTrigger >= threshold;
    case GamepadControl::RightTrigger: return gp.rightTrigger >= threshold;
    case GamepadControl::LeftStickX: return std::abs(gp.leftStick.x) >= threshold;
    case GamepadControl::LeftStickY: return std::abs(gp.leftStick.y) >= threshold;
    case GamepadControl::RightStickX: return std::abs(gp.rightStick.x) >= threshold;
    case GamepadControl::RightStickY: return std::abs(gp.rightStick.y) >= threshold;
    case GamepadControl::LeftStick: return std::hypot(gp.leftStick.x, gp.leftStick.y) >= threshold;
    case GamepadControl::RightStick: return std::hypot(gp.rightStick.x, gp.rightStick.y) >= threshold;
    default: break;
    }
    return false;
}

bool IsBindingBlocked(const InputBinding& binding, bool keyboardConsumed, bool pointerConsumed) noexcept
{
    switch (binding.device)
    {
    case InputDeviceKind::Keyboard: return keyboardConsumed;
    case InputDeviceKind::Mouse: return pointerConsumed;
    case InputDeviceKind::Gamepad: return keyboardConsumed && pointerConsumed;
    }
    return false;
}
} // namespace

InputActionMap::InputActionMap(InputActionDocument document)
{
    SetDocument(std::move(document));
}

void InputActionMap::SetDocument(InputActionDocument document)
{
    document_ = std::move(document);
    nameIndex_.clear();
    nameIndex_.reserve(document_.actions.size());
    for (std::size_t i = 0; i < document_.actions.size(); ++i)
    {
        nameIndex_.emplace(document_.actions[i].name, i);
    }
}

void InputActionMap::Reset() noexcept
{
    currentState_ = InputState{};
    previousGamepads_.fill(GamepadState{});
    hasPreviousGamepad_ = false;
    keyboardConsumed_ = false;
    pointerConsumed_ = false;
}

void InputActionMap::Update(const InputState& current, bool keyboardConsumed, bool pointerConsumed) noexcept
{
    previousGamepads_ = hasPreviousGamepad_ ? currentState_.gamepads : std::array<GamepadState, 4>{};
    hasPreviousGamepad_ = true;
    currentState_ = current;
    keyboardConsumed_ = keyboardConsumed;
    pointerConsumed_ = pointerConsumed;
}

const InputActionDefinition* InputActionMap::FindAction(std::string_view name) const noexcept
{
    const auto it = nameIndex_.find(name);
    if (it == nameIndex_.end() || it->second >= document_.actions.size()) return nullptr;
    return &document_.actions[it->second];
}

bool InputActionMap::IsActionPressed(std::string_view name) const noexcept
{
    return IsActionPressed(name, currentState_, hasPreviousGamepad_ ? &previousGamepads_ : nullptr,
                           keyboardConsumed_, pointerConsumed_);
}

bool InputActionMap::IsActionDown(std::string_view name) const noexcept
{
    return IsActionDown(name, currentState_, keyboardConsumed_, pointerConsumed_);
}

float InputActionMap::GetAxis1D(std::string_view name) const noexcept
{
    return GetAxis1D(name, currentState_, keyboardConsumed_, pointerConsumed_);
}

Vec2 InputActionMap::GetAxis2D(std::string_view name) const noexcept
{
    return GetAxis2D(name, currentState_, keyboardConsumed_, pointerConsumed_);
}

bool InputActionMap::IsActionPressed(
    std::string_view name,
    const InputState& current,
    const std::array<GamepadState, 4>* previousGamepads,
    bool keyboardConsumed,
    bool pointerConsumed) const noexcept
{
    const InputActionDefinition* action = FindAction(name);
    if (!action) return false;
    const bool keyReset = KeyboardResetInFrame(current);

    for (const InputBinding& binding : action->bindings)
    {
        if (IsBindingBlocked(binding, keyboardConsumed, pointerConsumed)) continue;
        if (binding.device == InputDeviceKind::Keyboard)
        {
            if (keyReset || binding.keyCode >= current.keysDown.size()) continue;
            for (const KeyEvent& ev : current.keyEvents)
            {
                if (ev.type == KeyEventType::Pressed && ev.keyCode == binding.keyCode) return true;
            }
        }
        else if (binding.device == InputDeviceKind::Mouse)
        {
            if (MouseControlPressed(current, binding.mouseControl)) return true;
        }
        else
        {
            if (binding.gamepadIndex >= current.gamepads.size()) continue;
            const bool nowDown = GamepadControlDown(current.gamepads[binding.gamepadIndex],
                                                    binding.gamepadControl, action->deadZone);
            const bool wasDown = previousGamepads
                ? GamepadControlDown((*previousGamepads)[binding.gamepadIndex], binding.gamepadControl, action->deadZone)
                : false;
            if (nowDown && !wasDown) return true;
        }
    }
    return false;
}

bool InputActionMap::IsActionDown(
    std::string_view name,
    const InputState& current,
    bool keyboardConsumed,
    bool pointerConsumed) const noexcept
{
    const InputActionDefinition* action = FindAction(name);
    if (!action) return false;
    const bool keyReset = KeyboardResetInFrame(current);

    for (const InputBinding& binding : action->bindings)
    {
        if (IsBindingBlocked(binding, keyboardConsumed, pointerConsumed)) continue;
        if (binding.device == InputDeviceKind::Keyboard)
        {
            if (!keyReset && binding.keyCode < current.keysDown.size() && current.keysDown[binding.keyCode])
            {
                return true;
            }
        }
        else if (binding.device == InputDeviceKind::Mouse)
        {
            if (MouseControlDown(current, binding.mouseControl)) return true;
        }
        else
        {
            if (binding.gamepadIndex < current.gamepads.size() &&
                GamepadControlDown(current.gamepads[binding.gamepadIndex], binding.gamepadControl, action->deadZone))
            {
                return true;
            }
        }
    }
    return false;
}

float InputActionMap::GetAxis1D(
    std::string_view name,
    const InputState& current,
    bool keyboardConsumed,
    bool pointerConsumed) const noexcept
{
    const InputActionDefinition* action = FindAction(name);
    if (!action) return 0.0f;
    const bool keyReset = KeyboardResetInFrame(current);

    float sum = 0.0f;
    float maxScale = 1.0f;
    for (const InputBinding& binding : action->bindings)
    {
        maxScale = std::max(maxScale, std::abs(binding.scale));
        if (IsBindingBlocked(binding, keyboardConsumed, pointerConsumed)) continue;
        const float dirSign = (binding.axisX != 0.0f) ? binding.axisX : 1.0f;

        if (binding.device == InputDeviceKind::Keyboard)
        {
            if (!keyReset && binding.keyCode < current.keysDown.size() && current.keysDown[binding.keyCode])
            {
                sum += dirSign * binding.scale;
            }
        }
        else if (binding.device == InputDeviceKind::Mouse)
        {
            if (binding.mouseControl == MouseControl::Wheel && !PointerCancelledInFrame(current))
            {
                float wheelSum = 0.0f;
                for (const PointerEvent& ev : current.pointerEvents)
                {
                    if (ev.type == PointerEventType::Wheel) wheelSum += ev.wheel;
                }
                sum += wheelSum * dirSign * binding.scale;
            }
            else if (MouseControlDown(current, binding.mouseControl))
            {
                sum += dirSign * binding.scale;
            }
        }
        else
        {
            if (binding.gamepadIndex >= current.gamepads.size()) continue;
            const GamepadState& gp = current.gamepads[binding.gamepadIndex];
            if (!gp.connected) continue;
            switch (binding.gamepadControl)
            {
            case GamepadControl::LeftTrigger:
                sum += ApplyScalarDeadZone(gp.leftTrigger, action->deadZone) * dirSign * binding.scale;
                break;
            case GamepadControl::RightTrigger:
                sum += ApplyScalarDeadZone(gp.rightTrigger, action->deadZone) * dirSign * binding.scale;
                break;
            case GamepadControl::LeftStickX:
                sum += ApplyScalarDeadZone(gp.leftStick.x, action->deadZone) * binding.scale;
                break;
            case GamepadControl::LeftStickY:
                sum += ApplyScalarDeadZone(gp.leftStick.y, action->deadZone) * binding.scale;
                break;
            case GamepadControl::RightStickX:
                sum += ApplyScalarDeadZone(gp.rightStick.x, action->deadZone) * binding.scale;
                break;
            case GamepadControl::RightStickY:
                sum += ApplyScalarDeadZone(gp.rightStick.y, action->deadZone) * binding.scale;
                break;
            case GamepadControl::LeftStick:
                sum += ApplyRadialDeadZone(gp.leftStick, action->deadZone).x * binding.scale;
                break;
            case GamepadControl::RightStick:
                sum += ApplyRadialDeadZone(gp.rightStick, action->deadZone).x * binding.scale;
                break;
            default:
                if (GamepadControlDown(gp, binding.gamepadControl, action->deadZone))
                {
                    sum += dirSign * binding.scale;
                }
                break;
            }
        }
    }
    return std::clamp(sum, -maxScale, maxScale);
}

Vec2 InputActionMap::GetAxis2D(
    std::string_view name,
    const InputState& current,
    bool keyboardConsumed,
    bool pointerConsumed) const noexcept
{
    const InputActionDefinition* action = FindAction(name);
    if (!action) return {};
    const bool keyReset = KeyboardResetInFrame(current);

    Vec2 sum{};
    float maxScale = 1.0f;
    for (const InputBinding& binding : action->bindings)
    {
        maxScale = std::max(maxScale, std::abs(binding.scale));
        if (IsBindingBlocked(binding, keyboardConsumed, pointerConsumed)) continue;

        if (binding.device == InputDeviceKind::Keyboard)
        {
            if (!keyReset && binding.keyCode < current.keysDown.size() && current.keysDown[binding.keyCode])
            {
                sum.x += binding.axisX * binding.scale;
                sum.y += binding.axisY * binding.scale;
            }
        }
        else if (binding.device == InputDeviceKind::Mouse)
        {
            if (binding.mouseControl == MouseControl::Wheel && !PointerCancelledInFrame(current))
            {
                float wheelSum = 0.0f;
                for (const PointerEvent& ev : current.pointerEvents)
                {
                    if (ev.type == PointerEventType::Wheel) wheelSum += ev.wheel;
                }
                sum.x += wheelSum * binding.axisX * binding.scale;
                sum.y += wheelSum * binding.axisY * binding.scale;
            }
            else if (MouseControlDown(current, binding.mouseControl))
            {
                sum.x += binding.axisX * binding.scale;
                sum.y += binding.axisY * binding.scale;
            }
        }
        else
        {
            if (binding.gamepadIndex >= current.gamepads.size()) continue;
            const GamepadState& gp = current.gamepads[binding.gamepadIndex];
            if (!gp.connected) continue;
            switch (binding.gamepadControl)
            {
            case GamepadControl::LeftStick:
            {
                const Vec2 s = ApplyRadialDeadZone(gp.leftStick, action->deadZone);
                sum.x += s.x * binding.scale;
                sum.y += s.y * binding.scale;
                break;
            }
            case GamepadControl::RightStick:
            {
                const Vec2 s = ApplyRadialDeadZone(gp.rightStick, action->deadZone);
                sum.x += s.x * binding.scale;
                sum.y += s.y * binding.scale;
                break;
            }
            case GamepadControl::LeftStickX:
                sum.x += ApplyScalarDeadZone(gp.leftStick.x, action->deadZone) * binding.scale;
                break;
            case GamepadControl::LeftStickY:
                sum.y += ApplyScalarDeadZone(gp.leftStick.y, action->deadZone) * binding.scale;
                break;
            case GamepadControl::RightStickX:
                sum.x += ApplyScalarDeadZone(gp.rightStick.x, action->deadZone) * binding.scale;
                break;
            case GamepadControl::RightStickY:
                sum.y += ApplyScalarDeadZone(gp.rightStick.y, action->deadZone) * binding.scale;
                break;
            case GamepadControl::LeftTrigger:
            {
                const float v = ApplyScalarDeadZone(gp.leftTrigger, action->deadZone);
                sum.x += v * binding.axisX * binding.scale;
                sum.y += v * binding.axisY * binding.scale;
                break;
            }
            case GamepadControl::RightTrigger:
            {
                const float v = ApplyScalarDeadZone(gp.rightTrigger, action->deadZone);
                sum.x += v * binding.axisX * binding.scale;
                sum.y += v * binding.axisY * binding.scale;
                break;
            }
            default:
                if (GamepadControlDown(gp, binding.gamepadControl, action->deadZone))
                {
                    sum.x += binding.axisX * binding.scale;
                    sum.y += binding.axisY * binding.scale;
                }
                break;
            }
        }
    }

    const float len = std::hypot(sum.x, sum.y);
    if (len > maxScale && len > 1e-6f)
    {
        const float factor = maxScale / len;
        sum.x *= factor;
        sum.y *= factor;
    }
    return sum;
}
} // namespace se
