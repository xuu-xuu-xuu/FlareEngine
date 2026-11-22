#include "EditorSession.h"
#include "FunctionLayer/Audio/SceneAudio.h"
#include "FunctionLayer/Navigation/SceneNavigation.h"
#include "FunctionLayer/Physics/ScenePhysics.h"
#include "FunctionLayer/Runtime/ComponentMetadata.h"
#include "CoreLayer/Math/TransformDecomposition.h"
#include "CoreLayer/Log.h"
#include "ResourceLayer/Scenes/SceneSerializer.h"
#include "SceneDefaults.h"
#include "PrefabWorkflow.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Assets/Primitives.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace flare
{
std::vector<EditorHierarchyItem> BuildEditorHierarchy(const World& world)
{
    std::unordered_map<StableEntityId,std::vector<StableEntityId>> children;
    for(const auto entity:world.Entities())
    {
        const auto* record=world.Get(entity);
        const auto parent=record->parent?world.Get(record->parent)->stableId:0;
        children[parent].push_back(record->stableId);
    }
    for(auto& [id,list]:children){(void)id;std::sort(list.begin(),list.end());}
    std::vector<EditorHierarchyItem> pending,result;result.reserve(world.Size());
    const auto push=[&](StableEntityId parent,std::size_t depth){
        if(const auto found=children.find(parent);found!=children.end())
            for(auto it=found->second.rbegin();it!=found->second.rend();++it)pending.push_back({*it,parent,depth,false});
    };
    push(0,0);
    while(!pending.empty())
    {
        auto item=pending.back();pending.pop_back();item.hasChildren=children.contains(item.id);
        result.push_back(item);push(item.id,item.depth+1);
    }
    return result;
}
bool EditorSession::Fail(const char* key) const { error_ = key; return false; }
bool EditorSession::IsSelected(StableEntityId id) const noexcept
{
    if (id == 0) return false;
    return std::find(selection_.begin(), selection_.end(), id) != selection_.end();
}
bool EditorSession::Select(StableEntityId id)
{
    const World& active = play_ ? play_->GetWorld() : edit_;
    if (id && !active.FindByStableId(id)) return Fail("Editor.InvalidSelection");
    selected_ = id;
    selection_.clear();
    if (id != 0) selection_.push_back(id);
    return true;
}
bool EditorSession::SetSelection(const std::vector<StableEntityId>& ids, StableEntityId primary)
{
    const World& active = play_ ? play_->GetWorld() : edit_;
    std::vector<StableEntityId> clean;
    clean.reserve(ids.size());
    for (const StableEntityId id : ids)
    {
        if (id == 0) continue;
        if (!active.FindByStableId(id)) return Fail("Editor.InvalidSelection");
        if (std::find(clean.begin(), clean.end(), id) == clean.end()) clean.push_back(id);
    }
    selection_ = std::move(clean);
    if (selection_.empty()) selected_ = 0;
    else if (primary != 0 && std::find(selection_.begin(), selection_.end(), primary) != selection_.end())
        selected_ = primary;
    else
        selected_ = selection_.back();
    return true;
}
bool EditorSession::ToggleSelection(StableEntityId id)
{
    const World& active = play_ ? play_->GetWorld() : edit_;
    if (id == 0 || !active.FindByStableId(id)) return Fail("Editor.InvalidSelection");
    const auto it = std::find(selection_.begin(), selection_.end(), id);
    if (it != selection_.end())
    {
        selection_.erase(it);
        if (selected_ == id) selected_ = selection_.empty() ? 0 : selection_.back();
    }
    else
    {
        selection_.push_back(id);
        selected_ = id;
    }
    return true;
}
bool EditorSession::Capture(Snapshot& snapshot) const
{
    snapshot.selected = selected_;
    snapshot.selection = selection_;
    snapshot.eulerCache = eulerCache_;
    return SerializeScene(edit_, snapshot.scene, error_);
}
bool EditorSession::Restore(const Snapshot& snapshot)
{
    World restored;if(!DeserializeScene(snapshot.scene,restored,error_))return false;
    // 撤销恢复逻辑对象；已发出的身份不随历史回滚，否则后来创建可能误接旧引用。
    for(const auto id:edit_.KnownStableIds())if(!restored.ReserveStableId(id))return Fail("Editor.OperationFailed");
    edit_=std::move(restored);
    if(!editScenes_.SynchronizeIdle()){error_=editScenes_.LastError();return false;}
    selection_.clear();
    for (const StableEntityId id : snapshot.selection)
    {
        if (id != 0 && edit_.FindByStableId(id) && std::find(selection_.begin(), selection_.end(), id) == selection_.end())
            selection_.push_back(id);
    }
    selected_ = edit_.FindByStableId(snapshot.selected) ? snapshot.selected : (selection_.empty() ? 0 : selection_.back());
    if (selected_ != 0 && std::find(selection_.begin(), selection_.end(), selected_) == selection_.end())
        selection_.push_back(selected_);
    eulerCache_ = snapshot.eulerCache;
    return true;
}
void EditorSession::Push(Command command)
{
    if (command.before.scene == command.after.scene &&
        command.before.selected == command.after.selected &&
        command.before.selection == command.after.selection) return;
    if (undo_.size() == 100) undo_.erase(undo_.begin());
    undo_.push_back(std::move(command)); redo_.clear();
}
bool EditorSession::Apply(std::string label, const std::function<bool(World&)>& operation)
{
    if (!operation || play_) return Fail("Editor.EditWhilePlaying");
    Command command; command.label = std::move(label);
    if (!Capture(command.before)) return false;
    World candidate(edit_);
    try
    {
        if (!operation(candidate) || !candidate.FlushCommands())
        { error_ = candidate.LastError().empty() ? "Editor.OperationFailed" : candidate.LastError(); return false; }
        if (!SerializeScene(candidate, command.after.scene, error_)) return false;
    }
    catch (...) { return Fail("Editor.OperationFailed"); }
    edit_ = std::move(candidate);
    if(!editScenes_.SynchronizeIdle()){error_=editScenes_.LastError();return false;}
    selection_.erase(std::remove_if(selection_.begin(), selection_.end(), [&](StableEntityId id) {
        return !edit_.FindByStableId(id);
    }), selection_.end());
    if (!edit_.FindByStableId(selected_)) selected_ = selection_.empty() ? 0 : selection_.back();
    command.after.selected = selected_;
    command.after.selection = selection_;
    command.after.eulerCache = eulerCache_;
    if (!transaction_) Push(std::move(command));
    error_.clear(); return true;
}
StableEntityId EditorSession::Create(std::string name, StableEntityId parent)
{
    StableEntityId created = 0;
    if (!Apply("Editor.Create", [&](World& world) {
        const auto p = parent ? world.FindByStableId(parent) : EntityHandle{};
        if (parent && !p) return false;
        const auto entity = world.Create(name);
        if (!entity || (parent && !world.SetParent(entity,p))) return false;
        created = world.Get(entity)->stableId; return true;
    })) return 0;
    selected_ = created;
    selection_ = {created};
    if (!transaction_ && !undo_.empty())
    {
        undo_.back().after.selected = created;
        undo_.back().after.selection = selection_;
    }
    return created;
}
bool EditorSession::Delete(StableEntityId id){
    const auto handle=edit_.FindByStableId(id);
    if(!handle)return Fail("Editor.InvalidSelection");
    bool savedBefore=false;
    if(!saved_.empty())
    {
        World base;std::string ignored;
        if(DeserializeScene(saved_,base,ignored))savedBefore=base.FindByStableId(id)!=EntityHandle{};
    }
    const bool result=Apply("Editor.Delete",[=](World& world){return world.Destroy(world.FindByStableId(id));});
    // 新建后删除、且从未保存的实体不应留下持久 retiredIds，否则一次无残留的编辑会永久变脏。
    if(result&&!savedBefore&&!edit_.FindByStableId(id)&&edit_.ForgetStableId(id))
    {
        if(transaction_)SerializeScene(edit_,transaction_->after.scene,error_);
        else if(!undo_.empty())SerializeScene(edit_,undo_.back().after.scene,error_);
    }
    return result;
}
bool EditorSession::Rename(StableEntityId id, std::string name)
{ return Apply("Editor.Rename", [&](World& world) { return world.Rename(world.FindByStableId(id),name); }); }
bool EditorSession::SetTransform(StableEntityId id, const Transform& transform)
{ return Apply("Editor.Transform", [&](World& world) { return world.SetTransform(world.FindByStableId(id),transform); }); }
bool EditorSession::SetParent(StableEntityId id,StableEntityId parent,ReparentMode mode)
{
    if(mode!=ReparentMode::KeepLocal&&mode!=ReparentMode::KeepWorld)return Fail("Editor.OperationFailed");
    bool unrepresentable=false;
    const bool result=Apply("Editor.Reparent", [&](World& world) {
        const auto entity=world.FindByStableId(id);if(!entity)return false;
        auto p = parent ? world.FindByStableId(parent) : EntityHandle{};
        if(parent&&!p)return false;
        const auto matrix=world.GetWorldMatrix(entity);
        if(!world.SetParent(entity,p))return false;
        if(mode==ReparentMode::KeepLocal)return true;
        const auto inverse=p?Inverse(world.GetWorldMatrix(p)):std::optional<Mat4>(Mat4{});Transform local;
        if(!inverse||!TryDecomposeTrs(*inverse*matrix,local)){unrepresentable=true;return false;}
        return world.SetTransform(entity,local);
    });
    if(!result&&unrepresentable)error_="Editor.TransformNotRepresentable";return result;
}
bool EditorSession::SetMesh(StableEntityId id, const MeshComponent& component)
{ return Apply("Editor.Mesh", [&](World& world) { return world.SetMesh(world.FindByStableId(id),component); }); }
bool EditorSession::SetProperty(StableEntityId id,std::string_view key,const PropertyValue& value)
{
    const auto* field=FindProperty(key);if(!field)return Fail("Metadata.UnknownProperty");
    std::string propertyError;
    const bool result=Apply(std::string(field->labelKey),[&](World& world){return WriteProperty(world,world.FindByStableId(id),key,value,propertyError);});
    if(!result&&!propertyError.empty())error_=propertyError;return result;
}
StableEntityId EditorSession::Duplicate(StableEntityId id, std::unordered_map<StableEntityId, StableEntityId>* outRemap)
{
    StableEntityId duplicate = 0;
    std::unordered_map<StableEntityId, StableEntityId> localRemap;
    const bool result = Apply("Editor.Duplicate", [&](World& world) {
        auto root = world.FindByStableId(id); if (!root) return false;
        const EntityRecord* rootRec = world.Get(root);
        const bool copyPrefabLinks = rootRec && rootRec->prefabLink.has_value() &&
            (!rootRec->parent || !world.GetPrefabLink(rootRec->parent) ||
             world.GetPrefabLink(rootRec->parent)->prefabId != rootRec->prefabLink->prefabId);
        const auto all = world.Entities();
        std::vector<EntityHandle> subtree;
        for (auto entity : all)
            for (auto cursor = entity; cursor; cursor = world.Get(cursor)->parent)
                if (cursor == root) { subtree.push_back(entity); break; }
        std::unordered_map<std::uint64_t, EntityHandle> replacements;
        for (auto source : subtree)
        {
            const auto record = *world.Get(source);
            const auto target = world.Create(record.name + " Copy");
            if (!target || !world.SetEnabled(target,record.enabled)) return false;
            if (record.tag != "Untagged" && !world.SetTag(target, record.tag)) return false;
            if (record.layer != 0 && !world.SetLayer(target, record.layer)) return false;
            if (record.staticFlags != 0 && !world.SetStaticFlags(target, record.staticFlags)) return false;
            replacements[source.Pack()] = target;
            if (source == root) duplicate = world.Get(target)->stableId;
        }
        for (auto source : subtree)
        {
            auto parent = world.Get(source)->parent;
            if (auto it = replacements.find(parent.Pack()); it != replacements.end()) parent = it->second;
            if (!world.SetParent(replacements.at(source.Pack()),parent)) return false;
        }
        std::unordered_map<StableEntityId,StableEntityId> references;
        for(auto source:subtree)references.emplace(world.Get(source)->stableId,world.Get(replacements.at(source.Pack()))->stableId);
        for(auto source:subtree)
        {
            const EntityHandle dst = replacements.at(source.Pack());
            std::string error;
            if(!CloneComponentProperties(world,source,world,dst,error,references))return false;
            if (copyPrefabLinks)
            {
                if (const PrefabLink* link = world.GetPrefabLink(source))
                {
                    PrefabLink copiedLink = *link;
                    for (auto& ov : copiedLink.overrides)
                    {
                        if (const auto* refVal = std::get_if<PropertyEntityReference>(&ov.value))
                        {
                            if (const auto mapIt = references.find(refVal->stableId); mapIt != references.end())
                                ov.value = PropertyEntityReference{mapIt->second};
                        }
                    }
                    (void)world.SetPrefabLink(dst, std::move(copiedLink));
                }
            }
        }
        localRemap = std::move(references);
        return true;
    });
    if (!result) return 0;
    if (outRemap)
    {
        for (const auto& [k, v] : localRemap) (*outRemap)[k] = v;
    }
    selected_ = duplicate;
    selection_ = {duplicate};
    if (!transaction_ && !undo_.empty())
    {
        undo_.back().after.selected = duplicate;
        undo_.back().after.selection = selection_;
    }
    return duplicate;
}
bool EditorSession::BeginEdit(std::string label)
{
    if (transaction_ || play_) return Fail("Editor.TransactionActive");
    auto command = std::make_unique<Command>(); command->label = std::move(label);
    if (!Capture(command->before)) return false;
    transaction_ = std::move(command); return true;
}
bool EditorSession::CommitEdit()
{
    if (!transaction_) return Fail("Editor.NoTransaction");
    if (!Capture(transaction_->after)) return false;
    auto command = std::move(*transaction_); transaction_.reset(); Push(std::move(command)); return true;
}
bool EditorSession::CancelEdit()
{
    if (!transaction_) return Fail("Editor.NoTransaction");
    if (!Restore(transaction_->before)) return false;
    transaction_.reset(); return true;
}
bool EditorSession::Undo()
{
    if (transaction_ || play_ || undo_.empty()) return Fail("Editor.CannotUndo");
    if (!Restore(undo_.back().before)) return false;
    redo_.push_back(std::move(undo_.back())); undo_.pop_back(); return true;
}
bool EditorSession::Redo()
{
    if (transaction_ || play_ || redo_.empty()) return Fail("Editor.CannotRedo");
    if (!Restore(redo_.back().after)) return false;
    undo_.push_back(std::move(redo_.back())); redo_.pop_back(); return true;
}
bool EditorSession::Dirty() const
{
    if (saved_.empty()) return edit_.Size() != 0;
    std::string current; return !SerializeScene(edit_,current,error_) || current != saved_;
}
bool EditorSession::RestoreUnsavedDocument(std::string_view content,const std::filesystem::path& path,const std::string& baseDigest)
{
    if(play_||transaction_)return Fail("Editor.TransactionActive");
    World restored;if(!DeserializeScene(content,restored,error_))return false;
    std::string saved,digest;World base;
    if(!path.empty()&&std::filesystem::is_regular_file(path))
    {
        std::vector<std::uint8_t> bytes;if(!asset_io::Read(path,bytes))return Fail("Scene.ReadFailed");
        digest=asset_io::Hash(bytes);if(digest!=baseDigest)return Fail("Editor.RecoverySourceChanged");
        if(!DeserializeScene({reinterpret_cast<const char*>(bytes.data()),bytes.size()},base,error_)||!SerializeScene(base,saved,error_))return false;
    }
    else if(!baseDigest.empty())return Fail("Editor.RecoverySourceChanged");
    edit_=std::move(restored);if(!editScenes_.SynchronizeIdle()){error_=editScenes_.LastError();return false;}
    path_=path;saved_=std::move(saved);sourceDigest_=std::move(digest);selected_=0;selection_.clear();eulerCache_.clear();undo_.clear();redo_.clear();error_.clear();return true;
}
bool EditorSession::NewDefaultScene(std::string& errorKey)
{
    errorKey.clear();
    if(play_||transaction_){errorKey="Editor.TransactionActive";return false;}
    World world;if(!CreateDefaultScene(world,errorKey))return false;
    edit_=std::move(world);if(!editScenes_.SynchronizeIdle()){errorKey=editScenes_.LastError();return false;}
    path_.clear();saved_.clear();sourceDigest_.clear();selected_=0;selection_.clear();eulerCache_.clear();undo_.clear();redo_.clear();error_.clear();
    return true;
}
bool EditorSession::Open(const std::filesystem::path& path, bool discardUnsaved)
{    if (transaction_ || play_) return Fail("Editor.TransactionActive");
    if (!discardUnsaved && Dirty()) return Fail("Editor.UnsavedChanges");
    std::vector<std::uint8_t> bytes;if(!asset_io::Read(path,bytes))return Fail("Scene.ReadFailed");
    SceneDocument document;if(!ParseSceneDocument({reinterpret_cast<const char*>(bytes.data()),bytes.size()},document,error_))return false;
    auto scene=editScenes_.ActiveScene();if(!scene)scene=editScenes_.Create("EditScene");
    if(!scene){error_=editScenes_.LastError();return false;}
    auto nextPath=path;const auto source=path.generic_u8string();std::string saved;
    if(!editScenes_.ActivateIdle(scene,document,SceneLoadMode::Replace,{},[&](const World& candidate,std::string& error){return SerializeScene(candidate,saved,error);},std::string(source.begin(),source.end())))
    {error_=editScenes_.LastError();return false;}
    saved_ = std::move(saved); path_ = std::move(nextPath);
    sourceDigest_=asset_io::Hash(bytes);
    selected_ = 0; selection_.clear(); eulerCache_.clear(); undo_.clear(); redo_.clear(); return true;
}
bool EditorSession::Save(const std::filesystem::path& path,bool replaceExisting)
{
    if (transaction_) return Fail("Editor.TransactionActive");
    if (!SaveScene(path,edit_,error_,replaceExisting)) return false;
    path_ = path;if(!SerializeScene(edit_,saved_,error_))return false;
    sourceDigest_=asset_io::Hash({reinterpret_cast<const std::uint8_t*>(saved_.data()),saved_.size()});return true;
}
bool EditorSession::ValidatePlay(std::string& reasonKey) const
{
    reasonKey.clear();
    if(!ValidateScenePhysics(edit_,reasonKey))return false;
    if(!ValidateSceneAudio(edit_,reasonKey))return false;
    if(!ValidateSceneNavigation(edit_,reasonKey))return false;
    if(play_||transaction_||startingPlay_||stoppingPlay_){reasonKey="Editor.TransactionActive";return false;}
    bool hasCamera=false;
    for(const auto entity:edit_.Entities())
    {
        if(!edit_.GetCamera(entity))continue;
        if(const auto camHandle=edit_.FindComponent(entity,ComponentTypes::Camera);
           camHandle&&(!edit_.GetComponent(camHandle)||!edit_.GetComponent(camHandle)->Enabled()))continue;
        bool enabled=true;
        for(auto current=entity;current;)
        {
            const auto* record=edit_.Get(current);
            if(!record||!record->enabled){enabled=false;break;}
            current=record->parent;
        }
        if(!enabled)continue;
        const auto matrix=edit_.GetWorldMatrix(entity);
        bool finite=true;
        for(const auto value:matrix.values)if(!std::isfinite(value)){finite=false;break;}
        if(!finite){reasonKey="Editor.PlayInvalidCamera";return false;}
        hasCamera=true;
    }
    if(!hasCamera){reasonKey="Editor.PlayNoCamera";return false;}
    return true;
}
bool EditorSession::StartPlay()
{
    LogCheckpoint("Play.Start.Begin");
    struct PlayResult{EditorSession& session;~PlayResult(){LogCheckpoint("Play.Start.End","playing="+std::to_string(session.IsPlaying())+" error="+session.LastError());}} result{*this};
    if(play_||transaction_||startingPlay_||stoppingPlay_)return Fail("Editor.TransactionActive");
    bool callbackEntered=false;
    try
    {
        auto candidate=std::make_unique<Environment>();candidate->SetTODResolver(todResolver_);candidate->GetWorld()=edit_;
        if(!candidate->Scenes().SynchronizeIdle()||!candidate->SynchronizeTOD())return Fail("Editor.PlayStartFailed");
        play_=std::move(candidate);startingPlay_=true;stopPlayRequested_=false;playStepRequested_=false;callbackEntered=true;
        const bool started=!playStarted_||playStarted_(play_->GetWorld());startingPlay_=false;
        if(!started||stopPlayRequested_){StopPlay();return Fail(started?"Editor.PlayStartCancelled":"Editor.PlayStartFailed");}
    }
    catch(...){startingPlay_=false;if(callbackEntered)StopPlay();else play_.reset();return Fail("Editor.PlayStartFailed");}
    error_.clear();return true;
}
bool EditorSession::SetPlayPaused(bool paused)
{
    if(!play_)return Fail("Editor.PlaybackInvalidState");
    play_->SetFramePaused(paused);
    error_.clear();return true;
}
bool EditorSession::StepPlayFrame()
{
    if(!play_||!play_->FramePaused())return Fail("Editor.PlaybackInvalidState");
    if(!play_->StepFrame()){error_=play_->LastError();return false;}
    error_.clear();return true;
}
double EditorSession::PlayFixedStep() const noexcept
{
    return play_?play_->Clock().Step():1.0/60.0;
}
bool EditorSession::PlayPaused() const noexcept
{
    return play_&&play_->FramePaused();
}
bool EditorSession::ConsumePlayStep() noexcept
{
    if(!playStepRequested_)return false;
    playStepRequested_=false;
    return play_&&play_->FramePaused();
}
bool EditorSession::SetTODResolver(Environment::TODResolver resolver)
{if(play_)return Fail("Editor.EditWhilePlaying");todResolver_=std::move(resolver);editEnvironment_.SetTODResolver(todResolver_);return true;}
void EditorSession::CompletePlayFrame()
{if(stopPlayRequested_&&play_&&!play_->IsRunningFrame())StopPlay();}
bool EditorSession::SetPlayCallbacks(std::function<bool(World&)> start,std::function<void(World&)> stopping)
{
    if(play_)return Fail("Editor.EditWhilePlaying");
    playStarted_=std::move(start);playStopping_=std::move(stopping);return true;
}
void EditorSession::StopPlay() noexcept
{
    if(!play_||stoppingPlay_)return;
    // 启动回调仍借用此 World，返回后才能执行停止回调与释放。
    if(startingPlay_||play_->IsRunningFrame()){stopPlayRequested_=true;return;}
    LogCheckpoint("Play.Stop.Begin");
    stopPlayRequested_=false;
    stoppingPlay_=true;
    play_->BeginShutdown();
    // 回调借用的 PlayWorld 必须保持存活直到 OnDestroy/订阅清理全部结束。
    try { if(playStopping_)playStopping_(play_->GetWorld()); } catch (...) { error_="Editor.PlayStopFailed"; }
    play_.reset();
    selection_.erase(std::remove_if(selection_.begin(), selection_.end(), [&](StableEntityId id) {
        return !edit_.FindByStableId(id);
    }), selection_.end());
    if (!edit_.FindByStableId(selected_)) selected_ = selection_.empty() ? 0 : selection_.back();
    stoppingPlay_=false;
    LogCheckpoint("Play.Stop.End");
}

namespace
{
Vec3 DecomposeEulerDegreesZXY(Quat orientation)
{
    const auto m = Rotation(Normalize(orientation));
    const float cosine = std::hypot(m(1,0), m(1,1));
    const float x = std::atan2(-m(1,2), cosine);
    const bool singular = cosine < 0.00001f;
    const float y = singular ? std::atan2(-m(2,0), m(0,0)) : std::atan2(m(0,2), m(2,2));
    const float z = singular ? 0.0f : std::atan2(m(1,0), m(1,1));
    return (180.0f / Pi) * Vec3{x, y, z};
}
}

Vec3 EditorSession::CachedEulerDegrees(StableEntityId id, Quat currentQuat, bool worldSpace) const
{
    const Quat q = Normalize(currentQuat);
    auto& entry = eulerCache_[id];
    bool& has = worldSpace ? entry.hasWorld : entry.hasLocal;
    Quat& cachedQ = worldSpace ? entry.worldQuat : entry.localQuat;
    Vec3& cachedE = worldSpace ? entry.worldEuler : entry.localEuler;
    const float dot = cachedQ.x * q.x + cachedQ.y * q.y + cachedQ.z * q.z + cachedQ.w * q.w;
    if (has && std::abs(dot) > 0.99995f)
    {
        return cachedE;
    }
    cachedQ = q;
    cachedE = DecomposeEulerDegreesZXY(q);
    has = true;
    return cachedE;
}

void EditorSession::UpdateCachedEulerDegrees(StableEntityId id, Quat syncedQuat, Vec3 eulerDegrees, bool worldSpace)
{
    auto& entry = eulerCache_[id];
    if (worldSpace)
    {
        entry.hasWorld = true;
        entry.worldQuat = Normalize(syncedQuat);
        entry.worldEuler = eulerDegrees;
    }
    else
    {
        entry.hasLocal = true;
        entry.localQuat = Normalize(syncedQuat);
        entry.localEuler = eulerDegrees;
    }
    if (!play_ && !transaction_ && !undo_.empty())
    {
        undo_.back().after.eulerCache = eulerCache_;
    }
}

bool EditorSession::CopyComponent(StableEntityId id, std::string_view componentKey)
{
    const ComponentDescriptor* desc = FindComponentDescriptor(componentKey);
    if (!desc) return Fail("Scene.UnknownComponent");
    const World& world = (IsPlaying() && PlayWorld()) ? *PlayWorld() : edit_;
    const EntityHandle entity = world.FindByStableId(id);
    if (!entity) return Fail("World.InvalidEntity");
    const ComponentHandle handle = world.FindComponent(entity, desc->type);
    const ComponentBase* base = world.GetComponent(handle);
    if (!base) return Fail("World.InvalidComponent");

    std::vector<ComponentProperty> props;
    props.reserve(desc->properties.size());
    std::string err;
    for (std::size_t i = 0; i < desc->properties.size(); ++i)
    {
        if (!desc->properties[i].serialized) continue;
        PropertyValue val;
        if (!desc->read(world, handle, i, val, err)) return Fail(err.empty() ? "Metadata.ReadFailed" : err.c_str());
        props.push_back({std::string(SerializedPropertyKey(desc->properties[i])), std::move(val)});
    }
    clipboard_.componentKey = std::string(desc->key);
    clipboard_.typeId = desc->type;
    clipboard_.version = desc->version;
    clipboard_.enabled = base->Enabled();
    clipboard_.properties = std::move(props);
    return true;
}

bool EditorSession::PasteComponentValues(StableEntityId id, std::string_view componentKey, AssetStore* assets)
{
    if (!clipboard_.Valid() || clipboard_.componentKey != componentKey) return Fail("Metadata.InvalidValue");
    const ComponentDescriptor* desc = FindComponentDescriptor(componentKey);
    if (!desc) return Fail("Scene.UnknownComponent");

    auto applyToWorld = [&](World& world, bool recordOverrides) -> bool {
        const EntityHandle entity = world.FindByStableId(id);
        if (!entity) return Fail("World.InvalidEntity");
        const ComponentHandle existing = world.FindComponent(entity, desc->type);
        if (!existing) return Fail("World.InvalidComponent");

        std::vector<ComponentProperty> sanitized = clipboard_.properties;
        for (auto& prop : sanitized)
        {
            if (auto* ref = std::get_if<PropertyEntityReference>(&prop.value))
            {
                if (ref->stableId != 0 && !world.CanResolveReference(ref->stableId)) ref->stableId = 0;
            }
        }
        std::string err;
        if (desc->type == ComponentTypes::Transform || desc->type == ComponentTypes::Camera ||
            desc->type == ComponentTypes::Mesh || desc->type == ComponentTypes::Light)
        {
            std::vector<PropertyValue> values;
            values.reserve(desc->properties.size());
            for (const auto& pd : desc->properties)
            {
                const auto sKey = SerializedPropertyKey(pd);
                const auto it = std::find_if(sanitized.begin(), sanitized.end(), [&](const auto& c) { return c.key == sKey; });
                values.push_back(it != sanitized.end() ? it->value : pd.defaultValue);
            }
            const bool en = (desc->type == ComponentTypes::Transform) ? true : clipboard_.enabled;
            if (!desc->create(world, entity, values, en, err)) return Fail(err.empty() ? "Metadata.WriteFailed" : err.c_str());
            if (desc->type != ComponentTypes::Transform)
            {
                if (const auto ch = world.FindComponent(entity, desc->type))
                    (void)world.SetComponentEnabled(ch, en);
            }
        }
        else
        {
            for (std::size_t i = 0; i < desc->properties.size(); ++i)
            {
                const auto& pd = desc->properties[i];
                if (!pd.editable) continue;
                const auto sKey = SerializedPropertyKey(pd);
                const auto it = std::find_if(sanitized.begin(), sanitized.end(), [&](const auto& c) { return c.key == sKey; });
                const PropertyValue& v = (it != sanitized.end()) ? it->value : pd.defaultValue;
                if (!desc->write(world, existing, i, v, err)) return Fail(err.empty() ? "Metadata.WriteFailed" : err.c_str());
            }
            (void)world.SetComponentEnabled(existing, clipboard_.enabled);
        }
        if (!world.FlushCommands()) return Fail(world.LastError().c_str());
        if (recordOverrides && world.GetPrefabLink(entity))
        {
            for (const auto& pd : desc->properties)
            {
                if (pd.editable) RecordOverrideAfterPropertyEdit(world, assets, entity, pd.key);
            }
            if (desc->type != ComponentTypes::Transform)
            {
                RecordOverrideAfterPropertyEdit(world, assets, entity, std::string(desc->key) + ".enabled");
            }
        }
        eulerCache_.erase(id);
        return true;
    };

    if (IsPlaying())
    {
        World* pw = PlayWorld();
        return pw && applyToWorld(*pw, false);
    }
    return Apply("Editor.ComponentPaste", [&](World& edit) { return applyToWorld(edit, true); });
}

bool EditorSession::PasteComponentAsNew(StableEntityId id)
{
    if (IsPlaying()) return Fail("Editor.EditWhilePlaying");
    if (!clipboard_.Valid() || clipboard_.typeId == ComponentTypes::Transform) return Fail("World.RequiredComponent");
    const ComponentDescriptor* desc = FindComponentDescriptor(clipboard_.typeId);
    if (!desc) return Fail("Scene.UnknownComponent");

    return Apply("EditorUi.AddComponent", [&](World& edit) {
        const EntityHandle entity = edit.FindByStableId(id);
        if (!entity || edit.FindComponent(entity, desc->type)) return Fail("Metadata.ComponentExists");
        std::vector<ComponentProperty> sanitized = clipboard_.properties;
        for (auto& prop : sanitized)
        {
            if (auto* ref = std::get_if<PropertyEntityReference>(&prop.value))
                if (ref->stableId != 0 && !edit.CanResolveReference(ref->stableId)) ref->stableId = 0;
        }
        std::string err;
        if (!CreateComponentFromProperties(edit, entity, desc->type, clipboard_.version, std::move(sanitized), clipboard_.enabled, err))
            return Fail(err.empty() ? "Metadata.WriteFailed" : err.c_str());
        if (!edit.FlushCommands()) return Fail(edit.LastError().c_str());
        return true;
    });
}

bool EditorSession::ResetComponent(StableEntityId id, std::string_view componentKey, AssetStore* assets)
{
    const ComponentDescriptor* desc = FindComponentDescriptor(componentKey);
    if (!desc) return Fail("Scene.UnknownComponent");

    auto resetInWorld = [&](World& world, bool recordOverrides) -> bool {
        const EntityHandle entity = world.FindByStableId(id);
        if (!entity) return Fail("World.InvalidEntity");
        const ComponentHandle existing = world.FindComponent(entity, desc->type);
        if (!existing) return Fail("World.InvalidComponent");

        if (desc->type == ComponentTypes::Transform)
        {
            if (!world.SetTransform(entity, Transform{})) return Fail(world.LastError().c_str());
        }
        else if (desc->type == ComponentTypes::Camera)
        {
            CameraComponent defCam{};
            if (const auto* cur = world.GetCamera(entity)) defCam.primary = cur->primary;
            if (!world.SetCamera(entity, defCam)) return Fail(world.LastError().c_str());
        }
        else if (desc->type == ComponentTypes::Light)
        {
            LightComponent defLight{};
            if (const auto* cur = world.GetLight(entity)) defLight.type = cur->type;
            if (!world.SetLight(entity, defLight)) return Fail(world.LastError().c_str());
        }
        else if (desc->type == ComponentTypes::Mesh)
        {
            MeshComponent defMesh{};
            if (const auto* cur = world.GetMesh(entity)) defMesh.meshAsset = cur->meshAsset.empty() ? std::string(BuiltinCubeAssetId) : cur->meshAsset;
            else defMesh.meshAsset = BuiltinCubeAssetId;
            if (!world.SetMesh(entity, defMesh)) return Fail(world.LastError().c_str());
        }
        else
        {
            std::string err;
            for (std::size_t i = 0; i < desc->properties.size(); ++i)
            {
                const auto& pd = desc->properties[i];
                if (!pd.editable) continue;
                if (!desc->write(world, existing, i, pd.defaultValue, err))
                    return Fail(err.empty() ? "Metadata.WriteFailed" : err.c_str());
            }
        }
        if (!world.FlushCommands()) return Fail(world.LastError().c_str());
        if (recordOverrides && world.GetPrefabLink(entity))
        {
            for (const auto& pd : desc->properties)
                if (pd.editable) RecordOverrideAfterPropertyEdit(world, assets, entity, pd.key);
        }
        eulerCache_.erase(id);
        return true;
    };

    if (IsPlaying())
    {
        World* pw = PlayWorld();
        return pw && resetInWorld(*pw, false);
    }
    return Apply("Editor.ComponentReset", [&](World& edit) { return resetInWorld(edit, true); });
}

bool EditorSession::MoveComponentOrder(StableEntityId id, std::string_view componentKey, int delta)
{
    if (IsPlaying()) return Fail("Editor.EditWhilePlaying");
    const ComponentDescriptor* desc = FindComponentDescriptor(componentKey);
    if (!desc || !desc->optional) return Fail("World.RequiredComponent");
    return Apply("Editor.ComponentMove", [&](World& edit) {
        const EntityHandle entity = edit.FindByStableId(id);
        return entity && edit.MoveComponent(entity, desc->type, delta);
    });
}

bool EditorSession::CaptureEntityPlayState(StableEntityId id, CapturedEntityPlayState& out) const
{
    const World* pw = PlayWorld();
    if (!pw || id == 0) return false;
    const EntityHandle entity = pw->FindByStableId(id);
    const EntityRecord* rec = pw->Get(entity);
    if (!rec) return false;
    out = {};
    out.id = id;
    out.name = rec->name;
    out.enabled = rec->enabled;
    out.tag = rec->tag;
    out.layer = rec->layer;
    out.staticFlags = rec->staticFlags;
    std::string err;
    for (const ComponentHandle ch : pw->Components(entity))
    {
        const ComponentBase* base = pw->GetComponent(ch);
        if (!base) continue;
        const ComponentDescriptor* desc = FindComponentDescriptor(base->Type());
        if (!desc) continue;
        EditorComponentClipboard snap;
        snap.componentKey = std::string(desc->key);
        snap.typeId = desc->type;
        snap.version = desc->version;
        snap.enabled = base->Enabled();
        snap.properties.reserve(desc->properties.size());
        bool ok = true;
        for (std::size_t i = 0; i < desc->properties.size(); ++i)
        {
            if (!desc->properties[i].serialized) continue;
            PropertyValue val;
            if (!desc->read(*pw, ch, i, val, err)) { ok = false; break; }
            snap.properties.push_back({std::string(SerializedPropertyKey(desc->properties[i])), std::move(val)});
        }
        if (ok) out.components.push_back(std::move(snap));
    }
    return true;
}

bool EditorSession::ApplyCapturedPlayStates(const std::vector<CapturedEntityPlayState>& states, AssetStore* assets)
{
    if (IsPlaying() || states.empty()) return false;
    return Apply("Editor.ApplyPlayChanges", [&](World& edit) {
        bool anyApplied = false;
        std::string err;
        for (const auto& st : states)
        {
            const EntityHandle entity = edit.FindByStableId(st.id);
            if (!entity) continue;
            (void)edit.Rename(entity, st.name);
            (void)edit.SetEnabled(entity, st.enabled);
            (void)edit.SetTag(entity, st.tag);
            (void)edit.SetLayer(entity, st.layer);
            (void)edit.SetStaticFlags(entity, st.staticFlags);
            for (const auto& comp : st.components)
            {
                const ComponentDescriptor* desc = FindComponentDescriptor(comp.typeId);
                if (!desc) continue;
                std::vector<ComponentProperty> sanitized = comp.properties;
                for (auto& prop : sanitized)
                {
                    if (auto* ref = std::get_if<PropertyEntityReference>(&prop.value))
                        if (ref->stableId != 0 && !edit.CanResolveReference(ref->stableId)) ref->stableId = 0;
                }
                const ComponentHandle existing = edit.FindComponent(entity, desc->type);
                if (existing)
                {
                    if (desc->type == ComponentTypes::Transform || desc->type == ComponentTypes::Camera ||
                        desc->type == ComponentTypes::Mesh || desc->type == ComponentTypes::Light)
                    {
                        std::vector<PropertyValue> values;
                        values.reserve(desc->properties.size());
                        for (const auto& pd : desc->properties)
                        {
                            const auto sKey = SerializedPropertyKey(pd);
                            const auto it = std::find_if(sanitized.begin(), sanitized.end(),
                                                         [&](const auto& c) { return c.key == sKey; });
                            values.push_back(it != sanitized.end() ? it->value : pd.defaultValue);
                        }
                        const bool en = (desc->type == ComponentTypes::Transform) ? true : comp.enabled;
                        (void)desc->create(edit, entity, values, en, err);
                        if (desc->type != ComponentTypes::Transform)
                        {
                            if (const auto ch = edit.FindComponent(entity, desc->type))
                                (void)edit.SetComponentEnabled(ch, en);
                        }
                    }
                    else
                    {
                        for (std::size_t i = 0; i < desc->properties.size(); ++i)
                        {
                            const auto& pd = desc->properties[i];
                            if (!pd.editable) continue;
                            const auto sKey = SerializedPropertyKey(pd);
                            const auto it = std::find_if(sanitized.begin(), sanitized.end(),
                                                         [&](const auto& c) { return c.key == sKey; });
                            const PropertyValue& v = (it != sanitized.end()) ? it->value : pd.defaultValue;
                            (void)desc->write(edit, existing, i, v, err);
                        }
                        (void)edit.SetComponentEnabled(existing, comp.enabled);
                    }
                }
                else if (desc->type != ComponentTypes::Transform)
                {
                    (void)CreateComponentFromProperties(edit, entity, desc->type, comp.version,
                                                        std::move(sanitized), comp.enabled, err);
                }
            }
            if (!edit.FlushCommands()) return Fail(edit.LastError().c_str());
            if (edit.GetPrefabLink(entity))
            {
                RecordOverrideAfterPropertyEdit(edit, assets, entity, "name");
                RecordOverrideAfterPropertyEdit(edit, assets, entity, "entity.enabled");
                RecordOverrideAfterPropertyEdit(edit, assets, entity, "entity.tag");
                RecordOverrideAfterPropertyEdit(edit, assets, entity, "entity.layer");
                RecordOverrideAfterPropertyEdit(edit, assets, entity, "entity.staticFlags");
                for (const auto& comp : st.components)
                {
                    if (const ComponentDescriptor* desc = FindComponentDescriptor(comp.typeId))
                    {
                        for (const auto& pd : desc->properties)
                            if (pd.editable) RecordOverrideAfterPropertyEdit(edit, assets, entity, pd.key);
                        if (desc->type != ComponentTypes::Transform)
                            RecordOverrideAfterPropertyEdit(edit, assets, entity, std::string(desc->key) + ".enabled");
                    }
                }
            }
            eulerCache_.erase(st.id);
            anyApplied = true;
        }
        return anyApplied;
    });
}
}
