#include "EditorShell.h"
#include "EditorShellInternal.h"
#include "EditorInk.h"
#include "CoreLayer/Utf.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <sstream>

namespace se::ui::editor
{
namespace
{
constexpr int Columns=64, Rows=48, Hues=96;
UiStyle Box(float x,float y,float width,float height,UiColor background={0,0,0,0})
{
    UiStyle style;style.position=UiPosition::Absolute;
    style.left=UiLength::Px(x);style.top=UiLength::Px(y);style.width=UiLength::Px(width);style.height=UiLength::Px(height);
    style.background=background;style.color=UiInk(flare::Heading);style.fontSize=13;
    style.textFlow=UiTextFlow::SingleLineEllipsis;style.padding={4,0,4,0};return style;
}
float Linear(float v){return v<=.04045f?v/12.92f:std::pow((v+.055f)/1.055f,2.4f);}
float Srgb(float v){v=std::clamp(v,0.f,1.f);return v<=.0031308f?v*12.92f:1.055f*std::pow(v,1/2.4f)-.055f;}
UiColor FromHsv(float h,float s,float v)
{
    const float sector=(h-std::floor(h))*6;const int i=static_cast<int>(sector);const float f=sector-i;
    const float p=v*(1-s),q=v*(1-s*f),t=v*(1-s*(1-f));UiColor color;
    switch(i){case 0:color={v,t,p,1};break;case 1:color={q,v,p,1};break;case 2:color={p,v,t,1};break;
    case 3:color={p,q,v,1};break;case 4:color={t,p,v,1};break;default:color={v,p,q,1};break;}
    return {Linear(color.r),Linear(color.g),Linear(color.b),1};
}
void ToHsv(UiColor color,float& h,float& s,float& v)
{
    const float r=Srgb(color.r),g=Srgb(color.g),b=Srgb(color.b);
    v=std::max({r,g,b});const float delta=v-std::min({r,g,b});s=v>0?delta/v:0;
    if(delta>1e-6f){h=(v==r?(g-b)/delta:v==g?2+(b-r)/delta:4+(r-g)/delta)/6;h-=std::floor(h);}
}
std::string Number(float value)
{char buffer[40];const auto [end,error]=std::to_chars(buffer,buffer+sizeof(buffer),value);return error==std::errc{}?std::string(buffer,end):"0";}
std::string Value(UiColor color){return Number(color.r)+" "+Number(color.g)+" "+Number(color.b);}
bool Same(UiColor a,UiColor b){return a.r==b.r&&a.g==b.g&&a.b==b.b;}
}

UiElementHandle EditorShell::ColorPickerDialog()const noexcept{return inspectorColor_?inspectorColor_->panel:UiElementHandle{};}
UiElementHandle EditorShell::ColorPickerSquare()const noexcept{return inspectorColor_?inspectorColor_->square:UiElementHandle{};}
UiElementHandle EditorShell::ColorPickerHue()const noexcept{return inspectorColor_?inspectorColor_->hueBar:UiElementHandle{};}
UiElementHandle EditorShell::ColorPickerChannel(std::size_t index)const noexcept{return inspectorColor_&&index<3?inspectorColor_->channels[index]:UiElementHandle{};}
UiElementHandle EditorShell::ColorPickerApplyButton()const noexcept{return inspectorColor_?inspectorColor_->apply:UiElementHandle{};}
void EditorShell::CancelPropertyInteraction(){EndInspectorDrag(false);CloseInspectorColor(false);}

void EditorShell::OpenInspectorColor(std::size_t index)
{
    const auto field=inspectorModel_.Properties().at(index);
    if(field.readOnly||!propertyHandler_||!propertyEditHandler_)return;
    EndInspectorEdit(true);EndInspectorDrag(false);CloseInspectorColor(true);CloseMenu();
    UiColor color{0,0,0,1};
    std::string cleanVal(field.value);
    for(char& ch:cleanVal)if(ch==',')ch=' ';
    std::istringstream value(cleanVal);value.imbue(std::locale::classic());
    if(!(value>>color.r>>color.g>>color.b)||!std::isfinite(color.r)||!std::isfinite(color.g)||!std::isfinite(color.b))return;
    if(!propertyEditHandler_(inspectorModel_.Target(),EditorPropertyEditPhase::Begin))return;
    inspectorColor_=std::make_unique<InspectorColor>();auto& picker=*inspectorColor_;
    picker.target=inspectorModel_.Target();picker.surface=inputSurface_;picker.key=field.key;
    picker.original=picker.current=color;ToHsv(color,picker.hue,picker.saturation,picker.value);
    const auto content=layout_.Rect(InspectorContent());picker.anchor={content.x+content.width*.36f,content.y+80,0,0};
    for(std::size_t row=0;row<inspectorRows_.size();++row)
        if(std::find(inspectorRows_[row].fields.begin(),inspectorRows_[row].fields.end(),index)!=inspectorRows_[row].fields.end())
            picker.anchor.y=content.y+row*22-inspectorList_->Offset();
    const auto make=[&](UiElementHandle parent,UiElementKind kind=UiElementKind::Panel){
        const auto element=document_.CreateElement(kind);document_.SetParent(element,parent);return element;};
    picker.backdrop=make(SurfaceRoot(picker.surface));picker.panel=make(picker.backdrop);
    picker.title=make(picker.panel,UiElementKind::Label);document_.SetText(picker.title,field.label.empty()?Text("EditorUi.ColorPickerTitle","Color picker"):field.label);
    picker.square=make(picker.panel);picker.hueBar=make(picker.panel);
    for(int i=0;i<Columns*Rows;++i)picker.tiles.push_back(make(picker.square));
    for(int i=0;i<Hues;++i)picker.hues.push_back(make(picker.hueBar));
    picker.cursor=make(picker.square);picker.cursorInner=make(picker.cursor);picker.hueCursor=make(picker.hueBar);
    picker.previous=make(picker.panel);picker.preview=make(picker.panel);
    picker.previousLabel=make(picker.previous,UiElementKind::Label);picker.previewLabel=make(picker.preview,UiElementKind::Label);
    document_.SetText(picker.previousLabel,Text("EditorUi.ColorPrevious","Original"));document_.SetText(picker.previewLabel,Text("EditorUi.ColorCurrent","Current"));
    for(std::size_t i=0;i<3;++i)
    {
        picker.channelLabels[i]=make(picker.panel,UiElementKind::Label);document_.SetText(picker.channelLabels[i],std::string(1,"RGB"[i]));
        picker.channels[i]=make(picker.panel,UiElementKind::TextEdit);
        picker.channelSelections[i]=make(picker.channels[i]);
        picker.editors[i]=std::make_unique<UiTextEdit>(document_,picker.channels[i],text_);
        dispatcher_.AddHandler(picker.channels[i],UiEventType::PointerDown,[this,i](UiInputEvent& event){
            if(!inspectorColor_||event.button!=0)return;
            inspectorColor_->editors[i]->Select(0,std::numeric_limits<std::uint32_t>::max());event.handled=true;
        });
        dispatcher_.AddHandler(picker.channels[i],UiEventType::TextInput,[this,i](UiInputEvent& event){
            if(!inspectorColor_)return;std::string utf8;const char32_t codepoint=event.codepoint;
            if(codepoint>=32&&EncodeUtf8({&codepoint,1},utf8)&&inspectorColor_->editors[i]->InsertText(utf8))ApplyInspectorColorChannel(i);
            event.handled=true;
        });
        dispatcher_.AddHandler(picker.channels[i],UiEventType::KeyDown,[this,i](UiInputEvent& event){
            if(!inspectorColor_)return;
            if(event.key==UiKeyEnter){ApplyInspectorColorChannel(i);dispatcher_.ClearFocus();}
            else{inspectorColor_->editors[i]->HandleKey(event.key,event.shift,event.control);ApplyInspectorColorChannel(i);}
            event.handled=true;
        });
        dispatcher_.AddHandler(picker.channels[i],UiEventType::FocusLost,[this](UiInputEvent&){if(inspectorColor_)RefreshInspectorColor();});
    }
    picker.apply=make(picker.panel,UiElementKind::Button);picker.cancel=make(picker.panel,UiElementKind::Button);
    document_.SetText(picker.apply,Text("EditorUi.ColorApply","Apply"));document_.SetText(picker.cancel,Text("EditorUi.ColorCancel","Cancel"));
    dispatcher_.AddHandler(picker.apply,UiEventType::PointerDown,[this](UiInputEvent& event){if(event.button==0)CloseInspectorColor(true);event.handled=true;});
    dispatcher_.AddHandler(picker.cancel,UiEventType::PointerDown,[this](UiInputEvent& event){if(event.button==0)CloseInspectorColor(false);event.handled=true;});
    const auto bind=[&](UiElementHandle element,int part){dispatcher_.AddHandler(element,UiEventType::PointerDown,[this,part](UiInputEvent& event){
        if(!inspectorColor_||event.button!=0)return;
        dispatcher_.SetPointerCapture(SurfaceRoot(inspectorColor_->surface));inspectorColor_->dragging=part;
        MoveInspectorColor(event);event.handled=true;
    });};
    bind(picker.square,1);bind(picker.hueBar,2);
    LayoutInspectorColor();RefreshInspectorColor();
}

void EditorShell::LayoutInspectorColor()
{
    if(!inspectorColor_)return;auto& p=*inspectorColor_;
    const auto bounds=layout_.Rect(SurfaceRoot(p.surface));
    const float width=std::max(160.f,std::min(288.f,bounds.width-16)),height=std::max(208.f,std::min(336.f,bounds.height-16));
    const float left=std::clamp(p.anchor.x-width-8,bounds.x+8,std::max(bounds.x+8,bounds.x+bounds.width-width-8));
    const float top=std::clamp(p.anchor.y-24,bounds.y+8,std::max(bounds.y+8,bounds.y+bounds.height-height-8));
    document_.SetStyle(p.backdrop,Box(0,0,bounds.width,bounds.height));
    auto panel=Box(left-bounds.x,top-bounds.y,width,height,UiInk(flare::workspace::Panel));panel.borderWidth=1;panel.borderColor=UiInk(flare::workspace::Divider);
    document_.SetStyle(p.panel,panel);document_.SetStyle(p.title,Box(10,4,width-20,24));
    const float inner=width-24,svHeight=height-176;
    document_.SetStyle(p.square,Box(12,32,inner,svHeight));document_.SetStyle(p.hueBar,Box(12,svHeight+40,inner,16));
    for(int row=0;row<Rows;++row)for(int column=0;column<Columns;++column)
    {
        auto style=document_.Style(p.tiles[row*Columns+column]);style.position=UiPosition::Absolute;
        style.left=UiLength::Px(column*inner/Columns);style.top=UiLength::Px(row*svHeight/Rows);
        style.width=UiLength::Px(inner/Columns+.05f);style.height=UiLength::Px(svHeight/Rows+.05f);style.pointerEvents=UiPointerEvents::None;
        document_.SetStyle(p.tiles[row*Columns+column],style);
    }
    for(int i=0;i<Hues;++i){auto style=Box(i*inner/Hues,0,inner/Hues+.05f,16,FromHsv(float(i)/Hues,1,1));style.pointerEvents=UiPointerEvents::None;document_.SetStyle(p.hues[i],style);}
    document_.SetStyle(p.previous,Box(12,svHeight+64,inner*.5f,22,p.original));document_.SetStyle(p.preview,Box(12+inner*.5f,svHeight+64,inner*.5f,22,p.current));
    const auto caption=[&](UiElementHandle element,UiColor color){auto style=Box(0,0,inner*.5f,22);style.pointerEvents=UiPointerEvents::None;
        style.color=(color.r*.2126f+color.g*.7152f+color.b*.0722f)>.4f?UiColor{0,0,0,1}:UiColor{1,1,1,1};document_.SetStyle(element,style);};
    caption(p.previousLabel,p.original);caption(p.previewLabel,p.current);
    for(int i=0;i<3;++i){const float x=12+i*inner/3;auto label=Box(x,svHeight+94,14,22);label.padding={};document_.SetStyle(p.channelLabels[i],label);
        const bool focused=dispatcher_.Focus()==p.channels[i];
        auto style=Box(x+14,svHeight+94,inner/3-18,22,UiInk(flare::workspace::Field));style.overflow=UiOverflow::Hidden;
        style.borderWidth=1;style.borderColor=UiInk(focused?flare::workspace::Focus:flare::workspace::Divider);document_.SetStyle(p.channels[i],style);
        document_.SetVisible(p.channelSelections[i],focused);
        if(focused)
        {
            const auto raw=p.editors[i]->Text();const auto caret=p.editors[i]->Caret(),anchor=p.editors[i]->Anchor();
            const auto first=std::min(caret,anchor),last=std::max(caret,anchor);
            const float a=text_->CaretRect(raw,13,100000,false,first).x,b=text_->CaretRect(raw,13,100000,false,last).x;
            auto selection=Box(4+a,3,std::max(1.f,b-a),16,first==last?UiInk(flare::Heading):UiInk(flare::workspace::Selection));
            selection.pointerEvents=UiPointerEvents::None;document_.SetStyle(p.channelSelections[i],selection);
        }
    }
    document_.SetStyle(p.apply,Box(width-176,height-36,78,24,UiInk(flare::workspace::Selection)));
    document_.SetStyle(p.cancel,Box(width-90,height-36,78,24,UiInk(flare::workspace::Header)));
    auto cursor=Box(p.saturation*inner-4,(1-p.value)*svHeight-4,8,8);cursor.borderWidth=1;cursor.borderColor={0,0,0,1};cursor.pointerEvents=UiPointerEvents::None;document_.SetStyle(p.cursor,cursor);
    auto inside=Box(1,1,6,6);inside.borderWidth=1;inside.borderColor={1,1,1,1};inside.pointerEvents=UiPointerEvents::None;document_.SetStyle(p.cursorInner,inside);
    cursor=Box(p.hue*inner-2,-2,4,20);cursor.borderWidth=1;cursor.borderColor={1,1,1,1};cursor.pointerEvents=UiPointerEvents::None;document_.SetStyle(p.hueCursor,cursor);
}

void EditorShell::RefreshInspectorColor(int editingChannel)
{
    if(!inspectorColor_)return;auto& p=*inspectorColor_;
    if(p.paintedHue!=p.hue)
    {
        for(int row=0;row<Rows;++row)for(int column=0;column<Columns;++column)
        {auto style=document_.Style(p.tiles[row*Columns+column]);style.background=FromHsv(p.hue,float(column)/(Columns-1),1-float(row)/(Rows-1));document_.SetStyle(p.tiles[row*Columns+column],style);}
        p.paintedHue=p.hue;
    }
    const float channels[]{p.current.r,p.current.g,p.current.b};
    for(int i=0;i<3;++i)if(i!=editingChannel){const auto value=Number(channels[i]);p.editors[i]->SetText(value);document_.SetText(p.channels[i],value);}
    LayoutInspectorColor();
}

void EditorShell::MoveInspectorColor(const UiInputEvent& event)
{
    if(!inspectorColor_)return;auto& p=*inspectorColor_;
    if(p.dragging==1){const auto rect=layout_.Rect(p.square);p.saturation=std::clamp((event.x-rect.x)/std::max(1.f,rect.width),0.f,1.f);p.value=1-std::clamp((event.y-rect.y)/std::max(1.f,rect.height),0.f,1.f);}
    if(p.dragging==2){const auto rect=layout_.Rect(p.hueBar);p.hue=std::clamp((event.x-rect.x)/std::max(1.f,rect.width),0.f,.999999f);}
    const auto color=FromHsv(p.hue,p.saturation,p.value);
    if(!Same(color,p.current))
    {
        if(!propertyHandler_(p.target,p.key,Value(color))){ToHsv(p.current,p.hue,p.saturation,p.value);return;}
        // 与写入场景的浮点精度一致，预览及撤销判断不维护另一份颜色。
        std::istringstream accepted(Value(color));accepted>>p.current.r>>p.current.g>>p.current.b;
    }
    RefreshInspectorColor();
}

void EditorShell::ApplyInspectorColorChannel(std::size_t index)
{
    if(!inspectorColor_||index>=3)return;auto& p=*inspectorColor_;
    const auto raw=p.editors[index]->Text();document_.SetText(p.channels[index],raw);
    float number=0;const auto end=raw.data()+raw.size();const auto result=std::from_chars(raw.data(),end,number);
    if(result.ec!=std::errc{}||result.ptr!=end||!std::isfinite(number)||number<0||number>1)return;
    auto color=p.current;if(index==0)color.r=number;else if(index==1)color.g=number;else color.b=number;
    if(!Same(color,p.current)&&!propertyHandler_(p.target,p.key,Value(color)))return;
    p.current=color;ToHsv(color,p.hue,p.saturation,p.value);RefreshInspectorColor(static_cast<int>(index));
}

void EditorShell::CloseInspectorColor(bool commit)
{
    if(!inspectorColor_)return;auto picker=std::move(inspectorColor_);
    dispatcher_.ClearFocus();dispatcher_.ReleasePointerCapture();
    document_.SetVisible(picker->backdrop,false);pendingDestroy_.push_back(picker->backdrop);
    const auto phase=commit&&!Same(picker->original,picker->current)?EditorPropertyEditPhase::Commit:EditorPropertyEditPhase::Cancel;
    if(propertyEditHandler_&&!propertyEditHandler_(picker->target,phase)&&phase==EditorPropertyEditPhase::Commit)
        propertyEditHandler_(picker->target,EditorPropertyEditPhase::Cancel);
}
}
