#include "EditorPlayInput.h"

#include <windowsx.h>
#include <algorithm>
#include <cmath>

namespace se
{
namespace
{
bool Keyboard(UINT message)
{
    return (message >= WM_KEYFIRST && message <= WM_KEYLAST) || message == WM_UNICHAR ||
           message == WM_IME_SETCONTEXT || message == WM_IME_STARTCOMPOSITION ||
           message == WM_IME_ENDCOMPOSITION || message == WM_IME_COMPOSITION ||
           message == WM_IME_CHAR || message == WM_IME_KEYDOWN || message == WM_IME_KEYUP;
}

int Button(UINT message)
{
    if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_LBUTTONDBLCLK)
    {
        return 0;
    }
    if (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP || message == WM_RBUTTONDBLCLK)
    {
        return 1;
    }
    if (message == WM_MBUTTONDOWN || message == WM_MBUTTONUP || message == WM_MBUTTONDBLCLK)
    {
        return 2;
    }
    return -1;
}

bool Down(UINT message)
{
    return message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK ||
           message == WM_RBUTTONDOWN || message == WM_RBUTTONDBLCLK ||
           message == WM_MBUTTONDOWN || message == WM_MBUTTONDBLCLK;
}
} // namespace

bool EditorPlayInput::Held() const
{
    return std::any_of(held_.begin(), held_.end(), [](bool value) { return value; });
}

void EditorPlayInput::Attach(UiTree* ui)
{
    if (ui == ui_)
    {
        return;
    }
    Deactivate(window_);
    input_.reset();
    ui_ = ui;
    if (ui_)
    {
        input_ = std::make_unique<WindowsUiInput>(*ui_);
        input_->SetViewport(viewport_);
    }
}

void EditorPlayInput::Activate()
{
    if (!ui_ || !input_ || (blocked_ && blocked_()))
    {
        return;
    }
    if (!active_)
    {
        active_ = true;
        if (onActivated_)
        {
            onActivated_();
        }
    }
}

void EditorPlayInput::Deactivate(HWND window)
{
    if (input_ && ui_)
    {
        if (window && IsWindow(window))
        {
            input_->Process(window, WM_KILLFOCUS, 0, 0);
        }
        else
        {
            ui_->CancelInput();
        }
    }
    active_ = false;
    over_ = false;
    held_.fill(false);
}

bool EditorPlayInput::OwnsPoint(UiPoint point) const
{
    if (!ui_ || !viewport_.Contains(point))
    {
        return false;
    }
    if (blocked_ && blocked_())
    {
        return false;
    }
    return !ownsPoint_ || ownsPoint_(point);
}

void EditorPlayInput::SetViewport(HWND window, UiRect viewport)
{
    window_ = window;
    viewport_ = viewport;
    if (input_)
    {
        input_->SetViewport(viewport);
        input_->SynchronizeCapture(window);
    }
    over_ = OwnsPoint(pointer_);
    if (active_)
    {
        if (viewport_.width <= 0 || viewport_.height <= 0 || (blocked_ && blocked_()))
        {
            Deactivate(window);
        }
        else if (input_)
        {
            input_->PositionIme(window);
        }
    }
}

EditorInputRoute EditorPlayInput::Process(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    EditorInputRoute result;
    if (!ui_ || !input_)
    {
        return result;
    }
    window_ = window;
    if (message == WM_KILLFOCUS)
    {
        Deactivate(window);
        return result;
    }
    if (message == WM_CAPTURECHANGED)
    {
        held_.fill(false);
        input_->Process(window, message, wParam, lParam);
        return result;
    }
    if (Keyboard(message) && active_)
    {
        if ((message == WM_KEYDOWN || message == WM_KEYUP || message == WM_SYSKEYDOWN || message == WM_SYSKEYUP) &&
            (wParam == VK_F5 || wParam == VK_F6 || wParam == VK_F7 || wParam == VK_F8 || wParam == VK_F11))
        {
            return result;
        }
        result.forwardEditor = false;
        result.handled = input_->Process(window, message, wParam, lParam);
        return result;
    }
    const int button = Button(message);
    const bool pointerMessage =
        button >= 0 || message == WM_MOUSEMOVE || message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL;
    if (!pointerMessage)
    {
        return result;
    }
    POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL)
    {
        ScreenToClient(window, &point);
    }
    pointer_ = {static_cast<float>(point.x), static_cast<float>(point.y)};
    over_ = OwnsPoint(pointer_);
    if (button >= 0 && Down(message))
    {
        if (!over_)
        {
            Deactivate(window);
            return result;
        }
        if (!active_)
        {
            active_ = true;
            if (onActivated_)
            {
                onActivated_();
            }
        }
        held_[static_cast<std::size_t>(button)] = true;
    }
    if (active_ && (over_ || Held()))
    {
        const UINT routedMessage = message == WM_LBUTTONDBLCLK ? WM_LBUTTONDOWN : message;
        result.forwardEditor = false;
        result.handled = input_->Process(window, routedMessage, wParam, lParam);
    }
    else if (active_ && message == WM_MOUSEMOVE)
    {
        ui_->PointerMove({-100000, -100000});
    }
    if (button >= 0 && !Down(message))
    {
        held_[static_cast<std::size_t>(button)] = false;
    }
    return result;
}

bool EditorPlayInput::KeyboardConsumed() const
{
    return !active_ || !input_ || input_->KeyboardConsumed();
}

bool EditorPlayInput::PointerConsumed() const
{
    return !active_ || !input_ || (!over_ && !Held() && !cursorCaptured_) || input_->PointerConsumed();
}

InputState EditorPlayInput::GameplayInput(const InputState& source) const
{
    InputState result = source;
    const int x = static_cast<int>(std::lround(viewport_.x));
    const int y = static_cast<int>(std::lround(viewport_.y));
    result.mouseX -= x;
    result.mouseY -= y;
    for (auto& event : result.pointerEvents)
    {
        event.x -= x;
        event.y -= y;
    }
    if (KeyboardConsumed())
    {
        result.keysDown.fill(false);
        result.keyEvents.clear();
        result.keyEvents.push_back({KeyEventType::Reset, 0});
        result.textEvents.clear();
    }
    if (PointerConsumed())
    {
        result.isLeftDown = false;
        result.mouseButtons.fill(false);
        result.mouseDeltaX = 0;
        result.mouseDeltaY = 0;
        result.pointerEvents.clear();
    }
    return result;
}

void EditorPlayInput::EndFrame()
{
    if (input_)
    {
        input_->EndFrame();
    }
}

} // namespace se
