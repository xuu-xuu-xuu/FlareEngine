#include "TodInspector.h"
#include "TODPreview.h"
#include "EditorSession.h"
#include "CoreLayer/Localization.h"
#include <charconv>
#include <algorithm>
#include <cmath>
namespace se
{
namespace
{
using namespace ui::editor;
std::string Number(double n){char data[64];const auto [end,ec]=std::to_chars(data,data+64,n);return ec==std::errc{}?std::string(data,end):"0";}
}
void AppendTodInspector(TODPreview& preview,const Localization& text,bool locked,std::vector<EditorProperty>& rows)
{
    const auto add=[&](std::string key,const char* label,EditorPropertyType type,std::string value,std::string section="")->EditorProperty&{rows.push_back({std::move(key),text.GetText(label),type,std::move(value),locked,{},section.empty()?text.GetText("TOD.Editor"):std::move(section),true});return rows.back();};
    add("tod.open","TOD.OpenPreview",EditorPropertyType::Action,{});
    if(!preview.Ready())return;
    add("tod.scope","Feature.Tools.Scope",EditorPropertyType::String,text.GetText("TOD.GpuLinkedPreview")).readOnly=true;
    const auto number=[&](std::string key,const char* label,double value,std::string section=""){add(std::move(key),label,EditorPropertyType::Number,Number(value),std::move(section)).dragStep=.001f;};
    const auto& p=preview.Profile();number("tod.dayLength","TOD.DayLength",p.dayLengthSeconds);number("tod.initialTime","TOD.InitialTime",p.initialTime);number("tod.rate","TOD.Rate",p.rate);number("tod.sunrise","TOD.Sunrise",p.sunrise);number("tod.sunset","TOD.Sunset",p.sunset);number("tod.azimuth","TOD.Azimuth",p.azimuthRadians);number("tod.elevation","TOD.Elevation",p.maximumElevationRadians);
    for(std::size_t i=0;i<p.keys.size();++i)
    {
        const auto key="tod.key."+std::to_string(i)+".";const auto section=text.GetText("TOD.CurveKey")+" "+std::to_string(i+1);const auto& k=p.keys[i];number(key+"time","TOD.Time",k.time,section);number(key+"intensity","TOD.Intensity",k.value.sunIntensity,section);number(key+"density","TOD.FogDensity",k.value.fogDensity,section);
        const auto color=[&](const char* name,const char* label,Vec3 c){const float values[]{c.x,c.y,c.z};for(int axis=0;axis<3;++axis){number(key+name+"."+"xyz"[axis],label,values[axis],section);rows.back().axis="RGB"[axis];}};
        color("sun","TOD.SunColor",k.value.sunColor);color("sky","TOD.SkyColor",k.value.skyColor);color("fog","TOD.FogColor",k.value.fogColor);
        add(key+"remove","Feature.Remove",EditorPropertyType::Action,{},section).readOnly=locked||p.keys.size()==1;
    }
    add("tod.add","Feature.Tod.Add",EditorPropertyType::Action,{});add("tod.paused","TOD.PreviewPaused",EditorPropertyType::Boolean,preview.Paused()?"true":"false");
    number("tod.time","TOD.PreviewTime",preview.Snapshot().time);add("tod.save","TOD.SaveProfile",EditorPropertyType::Action,{});
    const auto snapshot=preview.Snapshot();const auto sample=[&](const char* key,const char* label,Vec3 color){auto& row=add(key,label,EditorPropertyType::Color,Number(color.x)+" "+Number(color.y)+" "+Number(color.z),text.GetText("Feature.Tod.Sample"));row.readOnly=true;};
    sample("tod.sample.sun","TOD.SunColor",snapshot.parameters.sunColor);sample("tod.sample.sky","TOD.SkyColor",snapshot.parameters.skyColor);sample("tod.sample.fog","TOD.FogColor",snapshot.parameters.fogColor);
}
bool ApplyTodInspector(TODPreview& preview,AssetStore& assets,EditorSession& session,const std::string& key,const std::string& raw)
{
    if(session.IsPlaying())return false;
    if(key=="tod.open"){if(preview.Dirty())return false;return preview.Open(assets,session.EditWorld().EnvironmentSettings().todProfile);}
    if(key=="tod.save")return preview.Save();if(!preview.Ready())return false;
    if(key=="tod.paused"){if(raw!="true"&&raw!="false")return false;preview.SetPaused(raw=="true");return true;}
    auto p=preview.Profile();
    if(key=="tod.add")
    {if(p.keys.size()>=256)return false;double gap=0,start=0;for(std::size_t i=0;i<p.keys.size();++i){const auto end=i+1<p.keys.size()?p.keys[i+1].time:1.;if(end-p.keys[i].time>gap){gap=end-p.keys[i].time;start=p.keys[i].time;}}const auto time=start+gap*.5;p.keys.push_back({time,SampleTODProfile(p,time)});std::sort(p.keys.begin(),p.keys.end(),[](const auto& a,const auto& b){return a.time<b.time;});return preview.Apply(p);}
    std::size_t index=0;std::string field;
    if(key.starts_with("tod.key.")){const auto dot=key.find('.',8);if(dot==std::string::npos)return false;const auto [end,ec]=std::from_chars(key.data()+8,key.data()+dot,index);if(ec!=std::errc{}||end!=key.data()+dot||index>=p.keys.size())return false;field=key.substr(dot+1);if(field=="remove"){if(p.keys.size()==1)return false;p.keys.erase(p.keys.begin()+index);return preview.Apply(p);}}
    double n;const auto [end,ec]=std::from_chars(raw.data(),raw.data()+raw.size(),n);if(ec!=std::errc{}||end!=raw.data()+raw.size()||!std::isfinite(n))return false;
    if(key=="tod.time"){n-=std::floor(n);return preview.SetTime(n);}
    if(key=="tod.dayLength")p.dayLengthSeconds=n;else if(key=="tod.initialTime")p.initialTime=n;else if(key=="tod.rate")p.rate=n;else if(key=="tod.sunrise")p.sunrise=n;else if(key=="tod.sunset")p.sunset=n;else if(key=="tod.azimuth")p.azimuthRadians=static_cast<float>(n);else if(key=="tod.elevation")p.maximumElevationRadians=static_cast<float>(n);
    else if(!field.empty())
    {auto& k=p.keys[index];if(field=="time")k.time=n;else if(field=="intensity")k.value.sunIntensity=static_cast<float>(n);else if(field=="density")k.value.fogDensity=static_cast<float>(n);else{const auto dot=field.find('.');if(dot==std::string::npos)return false;auto* c=field.substr(0,dot)=="sun"?&k.value.sunColor:field.substr(0,dot)=="sky"?&k.value.skyColor:field.substr(0,dot)=="fog"?&k.value.fogColor:nullptr;if(!c)return false;if(field.substr(dot+1)=="x")c->x=static_cast<float>(n);else if(field.substr(dot+1)=="y")c->y=static_cast<float>(n);else if(field.substr(dot+1)=="z")c->z=static_cast<float>(n);else return false;}}
    else return false;return preview.Apply(p);
}
}
