#pragma once

#include "FunctionLayer/Input/InputActionAsset.h"
#include "PlatformLayer/InputState.h"
#include <array>
#include <string_view>
#include <unordered_map>

namespace se
{
// 动作输入求值器：只依赖 EngineCore 与 PlatformLayer/InputState.h 值头，支持无状态纯求值与帧更新状态快照。
class InputActionMap
{
public:
    InputActionMap() = default;
    explicit InputActionMap(InputActionDocument document);

    void SetDocument(InputActionDocument document);
    [[nodiscard]] const InputActionDocument& Document() const noexcept { return document_; }

    // 清除帧输入与手柄上一帧缓存（保留已加载的动作映射表）。
    void Reset() noexcept;

    // 推进一帧快照（保存上一帧手柄状态以精确判定手柄按下边沿）。
    void Update(const InputState& current, bool keyboardConsumed = false, bool pointerConsumed = false) noexcept;

    [[nodiscard]] bool IsActionPressed(std::string_view name) const noexcept;
    [[nodiscard]] bool IsActionDown(std::string_view name) const noexcept;
    [[nodiscard]] float GetAxis1D(std::string_view name) const noexcept;
    [[nodiscard]] Vec2 GetAxis2D(std::string_view name) const noexcept;

    // 纯函数式求值重载：直接输入当前帧 InputState 与 UI 屏蔽状态，不依赖类内可变缓冲。
    [[nodiscard]] bool IsActionPressed(
        std::string_view name,
        const InputState& current,
        const std::array<GamepadState, 4>* previousGamepads = nullptr,
        bool keyboardConsumed = false,
        bool pointerConsumed = false) const noexcept;
    [[nodiscard]] bool IsActionDown(
        std::string_view name,
        const InputState& current,
        bool keyboardConsumed = false,
        bool pointerConsumed = false) const noexcept;
    [[nodiscard]] float GetAxis1D(
        std::string_view name,
        const InputState& current,
        bool keyboardConsumed = false,
        bool pointerConsumed = false) const noexcept;
    [[nodiscard]] Vec2 GetAxis2D(
        std::string_view name,
        const InputState& current,
        bool keyboardConsumed = false,
        bool pointerConsumed = false) const noexcept;

private:
    struct TransparentStringHash
    {
        using is_transparent = void;

        [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept
        {
            return std::hash<std::string_view>{}(value);
        }
    };

    struct TransparentStringEqual
    {
        using is_transparent = void;

        [[nodiscard]] bool operator()(std::string_view lhs, std::string_view rhs) const noexcept
        {
            return lhs == rhs;
        }
    };

    [[nodiscard]] const InputActionDefinition* FindAction(std::string_view name) const noexcept;

    InputActionDocument document_;
    std::unordered_map<std::string, std::size_t, TransparentStringHash, TransparentStringEqual> nameIndex_;
    InputState currentState_{};
    std::array<GamepadState, 4> previousGamepads_{};
    bool hasPreviousGamepad_ = false;
    bool keyboardConsumed_ = false;
    bool pointerConsumed_ = false;
};
} // namespace se
