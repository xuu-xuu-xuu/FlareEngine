#pragma once
#include "EditorUi/EditorShell.h"
#include "ResourceLayer/Assets/AssetStore.h"
#include <future>
#include <atomic>

namespace se
{
class EditorSession;
struct FeatureToolResult
{
    bool success=false,cancelled=false;
    std::string error,detail;
    std::filesystem::path directory;
    std::vector<std::filesystem::path> artifacts;
    std::shared_ptr<ImageData> preview;
};
struct FeatureToolInput
{
    std::string operation;
    float amount=4;
    std::filesystem::path source,stage,destination,shaderBuilder;
    std::shared_ptr<const StaticModel> model;
    std::shared_ptr<const ImageData> image;
    struct Instance {std::shared_ptr<const StaticModel> model;Mat4 world;std::vector<AssetId> materials;};
    std::vector<Instance> instances;
};
// 算法工作可独立验证；只读快照跨线程，UI/AssetStore 的修改仅在 Poll 中执行。
FeatureToolResult ExecuteFeatureTool(const FeatureToolInput&,const std::atomic<bool>& cancel);
class EditorFeatureTools
{
public:
    EditorFeatureTools(std::filesystem::path,AssetStore&,EditorSession&,const Localization&);
    ~EditorFeatureTools();
    void Attach(ui::editor::EditorShell&);
    void Poll();
    void SelectSource(std::string source){if(!job_.valid()){source_=std::move(source);refreshPending_=true;}}
    const std::shared_ptr<ImageData>& Preview()const{return preview_;}
    std::uint64_t PreviewToken()const{return previewToken_;}
private:
    void Refresh();bool Start();bool Commit(std::string,std::string);
    std::filesystem::path root_,output_;
    AssetStore& assets_;EditorSession& session_;const Localization& text_;
    ui::editor::EditorShell* shell_=nullptr;
    std::string family_="geometry",operation_="lod",source_,status_;
    float amount_=4;
    std::future<FeatureToolResult> job_;
    std::atomic<bool> cancel_=false;
    bool refreshPending_=false;
    std::shared_ptr<ImageData> preview_;
    std::uint64_t previewToken_=1ull<<60;
};
}
