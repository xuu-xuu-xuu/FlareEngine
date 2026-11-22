#pragma once
#include <Windows.h>

namespace flare
{
// 图标资源从本函数所在 EXE/DLL 读取，托管宿主也使用同一 Flare 图标。
HICON AppIcon(int pixels) noexcept;
void ApplyAppIcon(HWND window) noexcept;
}
