#include "UiInspector.h"
#include "CoreLayer/Localization.h"
#include <charconv>
#include <sstream>
#include <cmath>
#include <nlohmann/json.hpp>
namespace se
{
namespace
{
using namespace ui::editor;
std::string Number(double number){char data[64];auto [end,error]=std::to_chars(data,data+64,number);return error==std::errc{}?std::string(data,end):"0";}
const char* kinds[]{"EditorUi.Panel","EditorUi.Label","EditorUi.Image","EditorUi.Button","EditorUi.Checkbox","EditorUi.Slider","EditorUi.Progress","EditorUi.Scroll","EditorUi.TextInput"};
}
std::vector<EditorProperty> UiInspectorRows(const UiTree& tree,UiId id,const Localization& text,bool locked)
{
    std::vector<EditorProperty> rows;const auto add=[&](std::string key,const char* label,EditorPropertyType type,std::string value,const char* section="EditorUi.UiDesigner")->EditorProperty&{
        rows.push_back({std::move(key),text.GetText(label),type,std::move(value),locked,{},text.GetText(section)});return rows.back();};
    if(const auto* node=tree.Get(id))
    {
        add("ui.text","EditorUi.Text",EditorPropertyType::String,node->text);
        add("ui.image","EditorUi.ImageAsset",EditorPropertyType::Asset,node->imageAsset);
        add("ui.parent","Feature.Ui.Parent",EditorPropertyType::String,std::to_string(node->parent));
        const auto point=[&](const char* key,const char* label,UiPoint value,float step){for(int axis=0;axis<2;++axis){auto& row=add(std::string(key)+'.'+"xy"[axis],label,EditorPropertyType::Number,Number(axis?value.y:value.x),"Feature.Ui.Rect");row.axis="XY"[axis];row.dragStep=step;}};
        point("ui.offset","EditorUi.Offset",node->style.offset,1);point("ui.size","EditorUi.Size",node->style.size,1);
        point("ui.anchorMin","EditorUi.AnchorMin",node->style.anchorMin,.01f);point("ui.anchorMax","EditorUi.AnchorMax",node->style.anchorMax,.01f);point("ui.pivot","EditorUi.Pivot",node->style.pivot,.01f);
        auto& layout=add("ui.layout","EditorUi.Layout",EditorPropertyType::Combo,std::to_string(static_cast<int>(node->style.layout)),"Feature.Ui.Rect");layout.options={"0","1","2"};layout.optionLabels={text.GetText("EditorUi.Absolute"),text.GetText("EditorUi.Horizontal"),text.GetText("EditorUi.Vertical")};layout.displayValue=layout.optionLabels[static_cast<int>(node->style.layout)];
        add("ui.padding","EditorUi.Padding",EditorPropertyType::Number,Number(node->style.padding),"Feature.Ui.Rect").dragStep=1;
        add("ui.spacing","EditorUi.Spacing",EditorPropertyType::Number,Number(node->style.spacing),"Feature.Ui.Rect").dragStep=1;
        add("ui.clip","EditorUi.Clip",EditorPropertyType::Boolean,node->style.clip?"true":"false","Feature.Ui.Rect");
        add("ui.minimum","EditorUi.Minimum",EditorPropertyType::Number,Number(node->minimum));add("ui.maximum","EditorUi.Maximum",EditorPropertyType::Number,Number(node->maximum));add("ui.value","EditorUi.Value",EditorPropertyType::Number,Number(node->value));add("ui.step","EditorUi.Step",EditorPropertyType::Number,Number(node->step));
        add("ui.color","EditorUi.UiColor",EditorPropertyType::Color,Number(node->color.r)+" "+Number(node->color.g)+" "+Number(node->color.b));
        auto& alpha=add("ui.alpha","Feature.Ui.Alpha",EditorPropertyType::Number,Number(node->color.a));alpha.dragStep=.01f;alpha.dragMinimum=0.f;alpha.dragMaximum=1.f;
        auto& opacity=add("ui.opacity","EditorUi.Opacity",EditorPropertyType::Number,Number(node->opacity));opacity.dragStep=.01f;opacity.dragMinimum=0.f;opacity.dragMaximum=1.f;
        add("ui.visible","EditorUi.Visible",EditorPropertyType::Boolean,node->visible?"true":"false");add("ui.enabled","Feature.Enabled",EditorPropertyType::Boolean,node->enabled?"true":"false");add("ui.blocks","Feature.Ui.Blocks",EditorPropertyType::Boolean,node->blocksPointer?"true":"false");
        nlohmann::json bindings=nlohmann::json::array();for(const auto& binding:node->bindings)bindings.push_back({{"event",static_cast<int>(binding.event)},{"entity",std::to_string(binding.entity)},{"script",binding.script},{"action",binding.action}});
        add("ui.bindings","Feature.Ui.Bindings",EditorPropertyType::String,bindings.dump(),"Feature.Ui.Events");
        add("ui.delete","EditorUi.DeleteUi",EditorPropertyType::Action,{});
    }
    for(int kind=0;kind<9;++kind){auto& row=add("ui.create."+std::to_string(kind),kinds[kind],EditorPropertyType::Action,{},"Feature.Ui.Create");row.sectionInitiallyClosed=true;}
    return rows;
}
bool ApplyUiInspector(UiTree& tree,UiId id,const std::string& key,const std::string& value,UiId& selected)
{
    try
    {
        if(key.starts_with("ui.create.")){unsigned kind=0;const auto raw=std::string_view(key).substr(10);auto [end,error]=std::from_chars(raw.data(),raw.data()+raw.size(),kind);if(error!=std::errc{}||end!=raw.data()+raw.size()||kind>8)return false;selected=tree.Create(static_cast<UiKind>(kind),id);return selected!=0;}
        const auto* current=tree.Get(id);if(!current)return false;const auto node=*current;
        if(key=="ui.delete"){if(!tree.Remove(id))return false;selected=0;return true;}
        if(key=="ui.text")return tree.SetText(id,value);if(key=="ui.image")return tree.SetImage(id,value);
        if(key=="ui.parent"){std::uint64_t parent=0;auto [end,error]=std::from_chars(value.data(),value.data()+value.size(),parent);return error==std::errc{}&&end==value.data()+value.size()&&tree.Reparent(id,parent);}
        if(key=="ui.bindings")
        {
            std::vector<UiEventBinding> bindings;const auto items=nlohmann::json::parse(value);if(!items.is_array()||items.size()>32)return false;
            for(const auto& item:items){const int event=item.at("event");if(event<0||event>5)return false;UiEventBinding binding;binding.event=static_cast<UiEventKind>(event);const auto raw=item.at("entity").is_string()?item.at("entity").get<std::string>():std::to_string(item.at("entity").get<std::uint64_t>());auto [end,error]=std::from_chars(raw.data(),raw.data()+raw.size(),binding.entity);if(error!=std::errc{}||end!=raw.data()+raw.size())return false;binding.script=item.at("script");binding.action=item.at("action");bindings.push_back(std::move(binding));}return tree.SetBindings(id,bindings);
        }
        if(key=="ui.color"){UiColor color=node.color;std::istringstream input(value);input.imbue(std::locale::classic());if(!(input>>color.r>>color.g>>color.b)||(input>>std::ws,!input.eof()))return false;return tree.SetColor(id,color);}
        if(key=="ui.enabled"||key=="ui.visible"||key=="ui.blocks"||key=="ui.clip")
        {if(value!="true"&&value!="false")return false;const bool flag=value=="true";if(key=="ui.enabled")return tree.SetEnabled(id,flag);if(key=="ui.visible")return tree.SetVisible(id,flag);if(key=="ui.blocks")return tree.SetBlocksPointer(id,flag);auto style=node.style;style.clip=flag;return tree.SetStyle(id,style);}
        double number=0;auto [end,error]=std::from_chars(value.data(),value.data()+value.size(),number);if(error!=std::errc{}||end!=value.data()+value.size()||!std::isfinite(number))return false;
        if(key=="ui.minimum")return tree.SetRange(id,number,node.maximum);if(key=="ui.maximum")return tree.SetRange(id,node.minimum,number);if(key=="ui.value")return tree.SetValue(id,number,false);if(key=="ui.step")return tree.SetStep(id,number);
        if(key=="ui.alpha"){auto color=node.color;color.a=static_cast<float>(number);return tree.SetColor(id,color);}if(key=="ui.opacity"){if(number<0||number>1)return false;tree.FadeTo(id,static_cast<float>(number),0);tree.Update(0);return true;}
        if(number<-100000||number>100000)return false;auto style=node.style;
        if(key=="ui.layout"){if(number<0||number>2||std::floor(number)!=number)return false;style.layout=static_cast<UiLayout>(static_cast<int>(number));}
        else if(key=="ui.padding")style.padding=static_cast<float>(number);else if(key=="ui.spacing")style.spacing=static_cast<float>(number);
        else
        {
            if(key.size()<3||key[key.size()-2]!='.')return false;const auto group=key.substr(0,key.size()-2);UiPoint* point=group=="ui.offset"?&style.offset:group=="ui.size"?&style.size:group=="ui.anchorMin"?&style.anchorMin:group=="ui.anchorMax"?&style.anchorMax:group=="ui.pivot"?&style.pivot:nullptr;if(!point)return false;
            if(key.back()=='x')point->x=static_cast<float>(number);else if(key.back()=='y')point->y=static_cast<float>(number);else return false;
        }
        return tree.SetStyle(id,style);
    }
    catch(const nlohmann::json::exception&){return false;}
}
}
