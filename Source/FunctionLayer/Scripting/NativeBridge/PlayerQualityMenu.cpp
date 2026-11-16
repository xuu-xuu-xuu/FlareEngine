#include "PlayerQualityMenu.h"
#include "FunctionLayer/GameUI/UiEventSystem.h"
#include "FunctionLayer/Application/FrameData.h"
#include "CoreLayer/Localization.h"
#include <windowsx.h>
#include <algorithm>

namespace se
{
namespace
{
constexpr const char* names[]{"PlayerQuality.Project","PlayerQuality.Performance","PlayerQuality.Balanced","PlayerQuality.High"};
constexpr const char* hints[]{"PlayerQuality.ProjectHint","PlayerQuality.PerformanceHint","PlayerQuality.BalancedHint","PlayerQuality.HighHint"};
}
PlayerQualityMenu::PlayerQualityMenu(PlayerQualityPreference& pref,const Localization& text,const QualityProfiles& profiles):preference_(pref),text_(text),profiles_(profiles)
{
    const auto node=[&](UiKind kind,UiId parent,UiStyle style,const char* key){
        const auto id=ui_.Create(kind,parent);ui_.SetStyle(id,style);if(key)ui_.SetText(id,text_.GetText(key));return id;};
    const auto box=[](float x,float y,float w,float h){UiStyle s;s.offset={x,y};s.size={w,h};return s;};
    auto launcher=box(-8,8,118,32);launcher.anchorMin=launcher.anchorMax=launcher.pivot={1,0};
    launch_=node(UiKind::Button,0,launcher,"PlayerQuality.Open");
    UiStyle fill;fill.anchorMax={1,1};fill.size={};shade_=node(UiKind::Panel,0,fill,nullptr);
    ui_.SetColor(shade_,{.005f,.009f,.018f,.68f});ui_.SetBlocksPointer(shade_,true);
    auto center=box(0,0,320,316);center.anchorMin=center.anchorMax=center.pivot={.5f,.5f};
    panel_=node(UiKind::Panel,shade_,center,nullptr);ui_.SetColor(panel_,{.07f,.09f,.14f,1});
    node(UiKind::Label,panel_,box(16,8,288,26),"PlayerQuality.Title");
    for(size_t i=0;i<levels_.size();++i)levels_[i]=node(UiKind::Button,panel_,box(16,43+36.f*i,288,32),names[i]);
    detail_=node(UiKind::Label,panel_,box(16,190,288,64),nullptr);
    apply_=node(UiKind::Button,panel_,box(16,264,138,36),"PlayerQuality.Apply");
    back_=node(UiKind::Button,panel_,box(166,264,138,36),"PlayerQuality.Cancel");
    Toggle(false);
}
void PlayerQualityMenu::Layout(HWND window)
{
    RECT r{};GetClientRect(window,&r);
    const float w=float(r.right),h=float(r.bottom);
    if(w<=0||h<=0)return;
    const float scale=std::min({1.f,w/344.f,h/332.f});
    ui_.SetCanvasReference(w/scale,h/scale);ui_.Layout(w,h);
}
void PlayerQualityMenu::Toggle(bool open)
{
    open_=open;selected_=static_cast<int>(preference_.Level());ui_.CancelInput();
    ui_.SetVisible(launch_,!open);ui_.SetVisible(shade_,open);Refresh();
}
void PlayerQualityMenu::Refresh()
{
    for(size_t i=0;i<levels_.size();++i){
        ui_.SetText(levels_[i],(static_cast<int>(i)==selected_?"●  ":"")+text_.GetText(names[i]));
        ui_.SetEnabled(levels_[i],available_||i==0);}
    auto hint=text_.GetText(available_?hints[selected_]:"PlayerQuality.Unavailable");
    if(available_&&selected_>0&&profiles_[selected_-1]!=DefaultQualityProfiles[selected_-1]){
        const auto& profile=profiles_[selected_-1];
        hint=text_.GetText("PlayerQuality.AuthoredHint")+" "+text_.GetText(std::string("PlayerQuality.Sampling.")+QualitySamplingIds[static_cast<size_t>(profile.sampling)]);
    }
    ui_.SetText(detail_,hint);
    ui_.SetEnabled(apply_,available_||selected_==0);
}
void PlayerQualityMenu::Commit()
{
    if(!available_&&selected_!=0)return;
    if(preference_.Select(static_cast<PlayerQuality>(selected_)))Toggle(false);
    else ui_.SetText(detail_,text_.GetText("PlayerQuality.SaveFailed"));
}
void PlayerQualityMenu::Append(HWND window,FrameData& frame)
{
    const bool available=SupportsPlayerQuality(frame);
    if(available_!=available){available_=available;Refresh();}
    Layout(window);auto overlay=ui_.Visuals();
    frame.ui.insert(frame.ui.end(),overlay.begin(),overlay.end());
}
std::optional<LRESULT> PlayerQualityMenu::Message(HWND window,UINT msg,WPARAM w,LPARAM l)
{
    if(msg==WM_KILLFOCUS||msg==WM_CANCELMODE||msg==WM_CAPTURECHANGED){
        ui_.CancelInput();if(msg!=WM_CAPTURECHANGED)heldByMenu_.reset();return {};}
    const bool keyboard=msg==WM_KEYDOWN||msg==WM_KEYUP||msg==WM_SYSKEYDOWN||msg==WM_SYSKEYUP;
    if(keyboard&&(w==VK_F10||open_||(w<heldByMenu_.size()&&heldByMenu_[w])))
    {
        if(msg==WM_SYSKEYDOWN&&w==VK_F4)return {};
        if(w<heldByMenu_.size())heldByMenu_.set(w,msg==WM_KEYDOWN||msg==WM_SYSKEYDOWN);
        keysHandled_=true;
        if((msg==WM_KEYDOWN||msg==WM_SYSKEYDOWN)&&!(l&(1L<<30))){
            if(w==VK_F10||w==VK_ESCAPE)Toggle(!open_);
            else if(w==VK_UP||w==VK_DOWN){selected_=available_?(selected_+(w==VK_UP?3:1))%4:0;Refresh();}
            else if(w==VK_RETURN)Commit();}
        return 0;
    }
    if(msg==WM_CHAR&&open_)return 0;
    if(msg!=WM_LBUTTONDOWN&&msg!=WM_LBUTTONUP&&msg!=WM_MOUSEMOVE){
        if(open_&&msg>=WM_MOUSEFIRST&&msg<=WM_MOUSELAST){pointerHandled_=true;return 0;}return {};}
    Layout(window);const UiPoint point{float(GET_X_LPARAM(l)),float(GET_Y_LPARAM(l))};
    const bool ownsPress=ui_.Captured()!=0;
    if(!open_&&!ownsPress&&(msg==WM_LBUTTONUP||GetCapture()==window))return {};
    const bool hit=UiEventSystem::HitTarget(ui_,point)!=0;
    if(!open_&&!hit&&!ui_.Captured()){ui_.PointerLeave();ui_.TakeEvents();return {};}
    pointerHandled_=true;
    if(msg==WM_MOUSEMOVE){ui_.PointerMove(point);ui_.TakeEvents();}
    else if(msg==WM_LBUTTONDOWN){ui_.PointerDown(point);if(ui_.Captured())SetCapture(window);}
    else{
        ui_.PointerUp(point);
        for(const auto& event:ui_.TakeEvents())if(event.kind==UiEventKind::Click){
            if(event.target==launch_)Toggle(true);
            else if(event.target==back_)Toggle(false);
            else if(event.target==apply_)Commit();
            else for(size_t i=0;i<levels_.size();++i)if(event.target==levels_[i]){selected_=static_cast<int>(i);Refresh();}}
        if(ownsPress&&GetCapture()==window)ReleaseCapture();
    }
    return 0;
}
}
