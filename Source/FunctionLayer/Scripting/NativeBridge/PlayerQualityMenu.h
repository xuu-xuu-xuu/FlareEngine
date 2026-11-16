#pragma once
#include "FunctionLayer/Application/PlayerQuality.h"
#include "FunctionLayer/GameUI/UiTree.h"
#include <Windows.h>
#include <optional>
#include <bitset>

class Localization;
namespace se
{
// 独立 UI 树，玩家菜单不污染项目 UI 的节点 ID、焦点、序列化或脚本事件。
class PlayerQualityMenu
{
public:
    PlayerQualityMenu(PlayerQualityPreference& preference,const Localization& text,const QualityProfiles& profiles);
    void Append(HWND window,FrameData& frame);
    std::optional<LRESULT> Message(HWND,UINT,WPARAM,LPARAM);
    bool Open()const{return open_;}
    bool ConsumePointer()const{return open_||pointerHandled_;}
    bool ConsumeKeys()const{return open_||keysHandled_;}
    void EndInput(){keysHandled_=pointerHandled_=false;}
private:
    void Layout(HWND window);
    void Toggle(bool open);
    void Refresh();
    void Commit();
    UiTree ui_;
    PlayerQualityPreference& preference_;
    const Localization& text_;
    const QualityProfiles& profiles_;
    UiId launch_,shade_,panel_,detail_,apply_,back_;
    std::array<UiId,4> levels_{};
    std::bitset<256> heldByMenu_;
    int selected_=0;bool open_=false,pointerHandled_=false,keysHandled_=false,available_=true;
};
}
