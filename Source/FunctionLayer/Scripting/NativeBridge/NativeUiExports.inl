int32_t SE_CALL se_ui_create(SE_Session handle, int32_t kind, uint64_t parent, uint64_t* output) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!output||kind<0||kind>8||(parent&&!s->UiRef().Get(parent)))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");
        *output=s->UiRef().Create(static_cast<se::UiKind>(kind),parent);return *output?SE_OK:Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");});
}
int32_t SE_CALL se_ui_remove(SE_Session handle, uint64_t id) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();return s->UiRef().Remove(id)?SE_OK:Error(SE_INVALID_HANDLE,"Bridge.InvalidUi");});
}
int32_t SE_CALL se_ui_set_style(SE_Session handle, uint64_t id, const SE_UiStyle* style) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!style||style->layout<0||style->layout>2)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");
        se::UiStyle value;
        value.anchorMin={style->anchor_min_x,style->anchor_min_y};value.anchorMax={style->anchor_max_x,style->anchor_max_y};
        value.pivot={style->pivot_x,style->pivot_y};value.offset={style->offset_x,style->offset_y};value.size={style->width,style->height};
        value.padding=style->padding;value.spacing=style->spacing;value.layout=static_cast<se::UiLayout>(style->layout);value.clip=style->clip!=0;
        return s->UiRef().SetStyle(id,value)?SE_OK:Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");});
}
int32_t SE_CALL se_ui_set_text(SE_Session handle, uint64_t id, const char* text, uint32_t length) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(length>1024*1024||!ValidUtf8(text,length))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUtf8");
        return s->UiRef().SetText(id,std::string_view(text?text:"",length))?SE_OK:Error(SE_INVALID_HANDLE,"Bridge.InvalidUi");});
}
int32_t SE_CALL se_ui_set_property(SE_Session handle, uint64_t id, int32_t property, double value) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!std::isfinite(value))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");
        bool ok=false;
        switch(property){case 0:ok=s->UiRef().SetValue(id,value);break;case 1:ok=s->UiRef().SetVisible(id,value!=0);break;
        case 2:ok=s->UiRef().SetEnabled(id,value!=0);break;case 3:ok=s->UiRef().Focus(id);break;
        case 4:ok=(value==0||value==1)&&s->UiRef().SetBlocksPointer(id,value!=0);break;case 5:ok=s->UiRef().SetStep(id,value);break;
        case 6:ok=value==1&&s->UiRef().CancelTransitions(id);break;default:break;}
        return ok?SE_OK:Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");});
}
int32_t SE_CALL se_ui_set_range(SE_Session handle, uint64_t id, double minimum, double maximum) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();return s->UiRef().SetRange(id,minimum,maximum)?SE_OK:Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");});
}
int32_t SE_CALL se_ui_set_image(SE_Session handle, uint64_t id, const char* asset, uint32_t length) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!asset||length>4096||!ValidUtf8(asset,length))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUtf8");
        return s->UiRef().SetImage(id,std::string(asset,length))?SE_OK:Error(SE_INVALID_HANDLE,"Bridge.InvalidUi");});
}
int32_t SE_CALL se_ui_exists(SE_Session handle, uint64_t id, int32_t* exists) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();if(!exists)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        *exists=s->UiRef().Get(id)?1:0;return SE_OK;});
}
int32_t SE_CALL se_ui_bind_action(SE_Session handle,uint64_t id,int32_t eventKind,uint64_t entity,
    const char* script,uint32_t scriptLength,const char* action,uint32_t actionLength) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();const auto node=s->UiRef().Get(id);
        if(!node||eventKind<0||eventKind>5)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");
        const bool clear=entity==0&&scriptLength==0&&actionLength==0;
        if(!clear&&(!script||!action||scriptLength>128||actionLength>128||!ValidUtf8(script,scriptLength)||!ValidUtf8(action,actionLength)))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");
        auto bindings=node->bindings;const auto event=static_cast<se::UiEventKind>(eventKind);
        const auto old=std::find_if(bindings.begin(),bindings.end(),[&](const auto& binding){return binding.event==event;});
        if(clear){if(old!=bindings.end())bindings.erase(old);}
        else
        {
            se::UiEventBinding value{event,entity,std::string(script,scriptLength),std::string(action,actionLength)};
            if(old==bindings.end())bindings.push_back(std::move(value));else *old=std::move(value);
        }
        return s->UiRef().SetBindings(id,bindings)?SE_OK:Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");});
}
int32_t SE_CALL se_ui_read_bindings(SE_Session handle,uint64_t id,char* buffer,uint32_t capacity,uint32_t* required) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();const auto node=s->UiRef().Get(id);
        if(!node)return Error(SE_INVALID_HANDLE,"Bridge.InvalidUi");auto values=nlohmann::json::array();
        for(const auto& binding:node->bindings)values.push_back(BindingJson(binding));return CopyText(values.dump(),buffer,capacity,required);});
}
int32_t SE_CALL se_ui_fade(SE_Session handle, uint64_t id, float opacity, double seconds) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!s->UiRef().Get(id))return Error(SE_INVALID_HANDLE,"Bridge.InvalidUi");
        if(!std::isfinite(opacity)||opacity<0||opacity>1||!std::isfinite(seconds)||seconds<0)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");
        s->UiRef().FadeTo(id,opacity,seconds);return SE_OK;});
}
int32_t SE_CALL se_ui_move(SE_Session handle,uint64_t id,float x,float y,double seconds) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!s->UiRef().Get(id))return Error(SE_INVALID_HANDLE,"Bridge.InvalidUi");
        if(!std::isfinite(x)||!std::isfinite(y)||std::abs(x)>1000000||std::abs(y)>1000000||!std::isfinite(seconds)||seconds<0)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");
        s->UiRef().MoveTo(id,{x,y},seconds);return SE_OK;});
}
int32_t SE_CALL se_ui_color(SE_Session handle,uint64_t id,float r,float g,float b,float a,double seconds) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!s->UiRef().Get(id))return Error(SE_INVALID_HANDLE,"Bridge.InvalidUi");
        if(!std::isfinite(seconds)||seconds<0)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");
        for(float v:{r,g,b,a})if(!std::isfinite(v)||v<0||v>1)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");
        s->UiRef().ColorTo(id,{r,g,b,a},seconds);return SE_OK;});
}
int32_t SE_CALL se_ui_layout(SE_Session handle, float width, float height) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!std::isfinite(width)||!std::isfinite(height)||width<0||height<0)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");
        s->UiRef().Layout(width,height);return SE_OK;});
}
int32_t SE_CALL se_ui_pointer(SE_Session handle, int32_t operation, float x, float y, float wheel, int32_t* consumed) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!consumed||!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(wheel))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");
        bool result=false;switch(operation){case 0:result=s->UiRef().PointerMove({x,y});break;case 1:result=s->UiRef().PointerDown({x,y});break;
        case 2:result=s->UiRef().PointerUp({x,y});break;case 3:result=s->UiRef().Wheel({x,y},wheel);break;case 4:s->UiRef().CancelInput();break;default:return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");}
        *consumed=result?1:0;return SE_OK;});
}
int32_t SE_CALL se_ui_key(SE_Session handle, uint32_t key, int32_t shift, int32_t control, int32_t* consumed) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();if(!consumed||key>255)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");
        *consumed=s->UiRef().EditKey(key,shift!=0,control!=0)?1:0;return SE_OK;});
}
int32_t SE_CALL se_ui_input_text(SE_Session handle, const char* text, uint32_t length, int32_t composition, int32_t* consumed) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!consumed||length>1024*1024||!ValidUtf8(text,length))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUtf8");
        const std::string_view value(text?text:"",length);
        *consumed=(composition?s->UiRef().SetComposition(value):s->UiRef().InsertText(value))?1:0;return SE_OK;});
}
int32_t SE_CALL se_ui_consumes_keyboard(SE_Session handle, int32_t* consumed) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();if(!consumed)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        *consumed=s->UiRef().ConsumesKeyboard()?1:0;return SE_OK;});
}
int32_t SE_CALL se_ui_read_json(SE_Session handle, int32_t kind, char* buffer, uint32_t capacity, uint32_t* required) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        std::string text;
        if(kind==0)text=s->UiRef().SaveJson();
        else if(kind==1)
        {
            auto values=nlohmann::json::array();
            for(const auto& v:s->UiRef().Visuals())values.push_back({{"Id",v.id},{"Kind",static_cast<int>(v.kind)},
                {"X",v.rectangle.x},{"Y",v.rectangle.y},{"Width",v.rectangle.width},{"Height",v.rectangle.height},
                {"ClipX",v.clip.x},{"ClipY",v.clip.y},{"ClipWidth",v.clip.width},{"ClipHeight",v.clip.height},
                {"Text",v.text},{"ImageAsset",v.imageAsset},{"Composition",v.composition},{"Value",v.value},
                {"Opacity",v.opacity},{"Hovered",v.hovered},{"Pressed",v.pressed},{"Focused",v.focused},{"Enabled",v.enabled},
                {"Caret",v.caret},{"SelectionAnchor",v.selectionAnchor},{"CompositionCaret",v.compositionCaret==std::string::npos?0:v.compositionCaret}});
            text=values.dump();
        }
        else if(kind==2)
        {
            auto fresh=s->UiRef().TakeEvents();s->uiEvents.insert(s->uiEvents.end(),fresh.begin(),fresh.end());
            auto values=nlohmann::json::array();
            for(const auto& e:s->uiEvents)
            {
                const auto node=s->UiRef().Get(e.target);if(!node)continue;
                nlohmann::json value{{"Kind",static_cast<int>(e.kind)},{"Target",e.target},{"Value",e.value},{"Text",e.text}};
                for(const auto& binding:node->bindings)if(binding.event==e.kind){value["Binding"]=BindingJson(binding);break;}
                values.push_back(std::move(value));
            }
            text=values.dump();
        }
        else return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");
        const auto result=CopyText(text,buffer,capacity,required);
        if(result==SE_OK&&kind==2)s->uiEvents.clear();return result;});
}
int32_t SE_CALL se_ui_load_json(SE_Session handle, const char* text, uint32_t length) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!text||length>16*1024*1024||!ValidUtf8(text,length))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUtf8");
        std::string error;if(!s->UiRef().LoadJson(std::string_view(text,length),error))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");
        s->uiEvents.clear();return SE_OK;});
}
