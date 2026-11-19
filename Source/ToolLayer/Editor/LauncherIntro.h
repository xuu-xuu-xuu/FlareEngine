#pragma once

#include <Windows.h>
#include <vector>

namespace se
{
class LauncherIntro
{
public:
    static constexpr UINT_PTR Timer = 0xF1A;
    bool Active() const { return page_ != nullptr; }
    bool Fits(int width, int height) const { return size_.cx == width && size_.cy == height; }
    void Start(HWND window);
    void Tick(HWND window);
    void Paint(HDC target) const;
    void Finish(HWND window);
    ~LauncherIntro();

private:
    void Release();
    HBITMAP page_ = nullptr;
    HBITMAP frame_ = nullptr;
    ULONG_PTR graphics_ = 0;
    ULONGLONG start_ = 0;
    SIZE size_{};
    std::vector<HWND> hiddenControls_;
};
}
