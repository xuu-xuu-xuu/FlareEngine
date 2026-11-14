#include "TODPreview.h"
namespace se
{
bool TODPreview::Open(AssetStore& assets,const AssetId& id)
{
    std::vector<AssetDiagnostic> diagnostics;const auto candidate=assets.LoadTODProfile(id,diagnostics);
    if(!candidate){error_=diagnostics.empty()?"TOD.ReadFailed":diagnostics.back().key;return false;}
    std::string encoded;if(!SerializeTODProfile(*candidate,encoded,error_))return false;
    if(!environment_.TimeOfDay().SelectProfile(*candidate)){error_=environment_.TimeOfDay().LastError();return false;}
    profile_=*candidate;id_=id;path_=assets.Resolve(id);saved_=std::move(encoded);environment_.SetPaused(true);undo_.clear();redo_.clear();edit_.reset();error_.clear();return true;
}
bool TODPreview::Apply(const TODProfile& candidate)
{
    if(!Ready()){error_="TOD.NoProfile";return false;}
    if(!environment_.TimeOfDay().SelectProfile(candidate,true)){error_=environment_.TimeOfDay().LastError();return false;}
    if(!edit_){undo_.push_back(profile_);redo_.clear();if(undo_.size()>128)undo_.erase(undo_.begin());}
    profile_=candidate;error_.clear();return true;
}
bool TODPreview::Save()
{
    if(!Ready()){error_="TOD.NoProfile";return false;}
    std::string encoded;if(!SerializeTODProfile(profile_,encoded,error_)||!SaveTODProfile(path_,profile_,error_))return false;saved_=std::move(encoded);return true;
}
bool TODPreview::Dirty()const{std::string text,error;return Ready()&&SerializeTODProfile(profile_,text,error)&&text!=saved_;}
bool TODPreview::EndEdit(bool commit)
{
    if(!edit_)return false;auto before=std::move(*edit_);edit_.reset();std::string a,b,error;SerializeTODProfile(before,a,error);SerializeTODProfile(profile_,b,error);if(a==b)return false;
    if(commit){undo_.push_back(std::move(before));redo_.clear();if(undo_.size()>128)undo_.erase(undo_.begin());}
    else {environment_.TimeOfDay().SelectProfile(before,true);profile_=std::move(before);}return true;
}
bool TODPreview::Undo(){if(undo_.empty()||edit_)return false;auto previous=undo_.back();if(!environment_.TimeOfDay().SelectProfile(previous,true))return false;redo_.push_back(profile_);profile_=std::move(previous);undo_.pop_back();return true;}
bool TODPreview::Redo(){if(redo_.empty()||edit_)return false;auto next=redo_.back();if(!environment_.TimeOfDay().SelectProfile(next,true))return false;undo_.push_back(profile_);profile_=std::move(next);redo_.pop_back();return true;}
}
