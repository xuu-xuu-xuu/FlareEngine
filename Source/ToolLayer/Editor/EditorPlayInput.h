#pragma once

#include "PlatformLayer/InputState.h"
#include "PlatformLayer/Windows/WindowsUiInput.h"

#include <array>
#include <functional>
#include <memory>
#include <optional>

namespace flare
{
struct EditorInputRoute
{
    bool forwardEditor = true;
    std::optional<LRESULT> handled;
};

// 将 Play 视口窗口内的输入安全转交给游戏 UI 与 Gameplay，并尊重遮挡判断。
class EditorPlayInput
{
public:
    using OverlayHitPredicate = std::function<bool(UiPoint)>;

    explicit EditorPlayInput(OverlayHitPredicate viewportOwnsPoint = {},
                             std::function<void()> onViewportActivated = {},
                             std::function<bool()> viewportBlocked = {})
        : ownsPoint_(std::move(viewportOwnsPoint))
        , onActivated_(std::move(onViewportActivated))
        , blocked_(std::move(viewportBlocked))
    {
    }

    void Attach(UiTree* ui);
    void SetViewport(HWND window, UiRect viewport);
    void Activate();
    EditorInputRoute Process(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    [[nodiscard]] bool Active() const noexcept { return active_; }
    void SetCursorCaptured(bool captured) noexcept { cursorCaptured_ = captured; }
    [[nodiscard]] bool CursorCaptured() const noexcept { return cursorCaptured_; }
    [[nodiscard]] HWND Window() const noexcept { return window_; }
    [[nodiscard]] UiRect Viewport() const noexcept { return viewport_; }
    [[nodiscard]] bool KeyboardConsumed() const;
    [[nodiscard]] bool PointerConsumed() const;
    [[nodiscard]] InputState GameplayInput(const InputState& source) const;
    void EndFrame();
    void Deactivate(HWND window = nullptr);

private:
    bool OwnsPoint(UiPoint point) const;
    bool Held() const;

    OverlayHitPredicate ownsPoint_;
    std::function<void()> onActivated_;
    std::function<bool()> blocked_;
    UiTree* ui_ = nullptr;
    std::unique_ptr<WindowsUiInput> input_;
    UiRect viewport_{};
    UiPoint pointer_{};
    std::array<bool, 3> held_{};
    bool active_ = false;
    bool over_ = false;
    bool cursorCaptured_ = false;
    HWND window_ = nullptr;
};

} // namespace flare
