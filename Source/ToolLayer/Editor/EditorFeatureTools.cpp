#include "EditorFeatureTools.h"
#include "EditorSession.h"
#include "EditorAssetActions.h"
#include "EditorProcessRunner.h"
#include "CoreLayer/Localization.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Assets/LodBake.h"
#include "ResourceLayer/Assets/HlodBake.h"
#include "ResourceLayer/Assets/MeshletBake.h"
#include "ResourceLayer/Assets/ImpostorBake.h"
#include "ResourceLayer/Assets/TextureBake.h"
#include "ResourceLayer/Assets/UserShaderVariants.h"
#include "ResourceLayer/Assets/VertexCompression.h"
#include <nlohmann/json.hpp>
#include "ResourceLayer/Assets/Environment.h"
#include "ResourceLayer/Assets/ImageWriter.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "ResourceLayer/Assets/StaticModelPreparation.h"
#include "FunctionLayer/Rendering/Atmosphere.h"
#include "FunctionLayer/Rendering/Reflection.h"
#include "FunctionLayer/Navigation/NavMeshBaker.h"
#include "FunctionLayer/Navigation/SceneNavigation.h"
#include <Windows.h>
#include <shellapi.h>
#include <charconv>
#include <chrono>
#include <fstream>
#include <algorithm>
#include <cmath>

namespace se
{
namespace
{
using namespace ui::editor;
std::vector<std::string> Operations(std::string_view family)
{return family=="geometry"?std::vector<std::string>{"lod","hlod","meshlets","impostor","vertices","navmesh"}:family=="images"?std::vector<std::string>{"environment","bc1","bc4","bc5","shader"}:std::vector<std::string>{"atmosphere","fog","probes","ssr","occlusion"};}
std::shared_ptr<const StaticModel> Model(AssetStore& assets,const AssetId& id,std::vector<AssetDiagnostic>& diagnostics)
{
    if(!IsBuiltinMesh(id))return assets.LoadModel(id,diagnostics);
    auto model=std::make_shared<StaticModel>();model->meshes.push_back(id==BuiltinCubeAssetId?MakeUnitCube():MakeUnitSphere());ModelNode node;node.meshes={0};model->nodes.push_back(std::move(node));return model;
}
void Pixel(ImageData& image,std::size_t index,Vec3 color)
{
    const float values[]{color.x,color.y,color.z};for(int c=0;c<3;++c){const auto v=std::max(0.f,values[c]);image.rgba8[index*4+c]=static_cast<unsigned char>(255*std::pow(v/(1+v),1/2.2f));}image.rgba8[index*4+3]=255;
}
std::shared_ptr<ImageData> ReferencePreview(const FeatureToolInput& input,const std::atomic<bool>& cancel)
{
    auto image=std::make_shared<ImageData>();image->width=256;image->height=128;image->rgba8.resize(image->width*image->height*4);
    Atmosphere atmosphere;AtmosphereSettings settings;settings.turbidity=std::clamp(input.amount,.1f,10.f);if(!atmosphere.Rebuild(settings))return {};
    FogSettings fog;fog.density=std::clamp(input.amount,.0001f,1.f);
    ScreenSpaceFrame screen;screen.width=image->width;screen.height=image->height;screen.aspect=2;screen.viewDepth.resize(image->width*image->height);screen.viewNormal.resize(screen.viewDepth.size(),{0,0,-1});
    for(unsigned y=0;y<image->height;++y)for(unsigned x=0;x<image->width;++x)screen.viewDepth[y*image->width+x]=(x>90&&x<165&&y>35&&y<95)?2.f:4.f;
    std::vector<float> ao,contact;
    if(input.operation=="occlusion")
    {GtaoSettings a;a.radius=std::clamp(input.amount,.01f,10.f);if(!ComputeGtao(screen,a,ao)||!ComputeContactShadows(screen,{},Normalize(Vec3{1,1,-1}),contact))return {};}
    FloatImage environment;environment.width=64;environment.height=32;environment.pixels.resize(64*32);
    for(unsigned y=0;y<32;++y)for(unsigned x=0;x<64;++x)environment.pixels[y*64+x]={.1f+x/64.f,.2f+y/32.f,.6f};
    if(!input.source.empty()&&input.operation=="probes")
    {std::vector<AssetDiagnostic> diagnostics;if(!LoadRadiance(input.source,environment,diagnostics))return {};}
    if(input.operation=="probes")
    {std::vector<Vec3> filtered;if(!PrefilterGgx(environment.pixels,environment.width,environment.height,std::clamp(input.amount,0.f,1.f),16,filtered))return {};environment.pixels=std::move(filtered);}
    for(unsigned y=0;y<image->height;++y)
    {
        if(cancel)return {};
        for(unsigned x=0;x<image->width;++x)
        {
            const float u=(x+.5f)/image->width,v=(y+.5f)/image->height;const auto index=y*image->width+x;
            const auto azimuth=(u-.5f)*2*Pi,elevation=(.5f-v)*Pi;
            const Vec3 direction{std::sin(azimuth)*std::cos(elevation),std::sin(elevation),std::cos(azimuth)*std::cos(elevation)};
            Vec3 color;
            if(input.operation=="atmosphere")color=atmosphere.SkyRadiance(direction);
            else if(input.operation=="fog"){float transmission=0;color=IntegrateFog(fog,direction,2,20+u*180,transmission)+transmission*Vec3{.3f,.5f,.8f};}
            else if(input.operation=="probes")
            {
                const float sx=(u-.5f)*4,sy=(.5f-v)*2,r2=sx*sx+sy*sy;
                if(r2>1)color={.025f,.025f,.025f};else
                {
                    const auto normal=Normalize(Vec3{sx,sy,std::sqrt(1-r2)});const auto reflected=2*normal.z*normal-Vec3{0,0,1};Vec3 projected;
                    if(!BoxProjectReflection({},Vec3{3,3,3},normal,reflected,projected))projected=reflected;
                    color=SampleEquirectangular(environment,projected);
                }
            }
            else if(input.operation=="occlusion"){const auto value=ao[index]*contact[index];color={value,value,value};}
            else
            {
                SsrHit hit;SsrSettings ssr;ssr.thickness=std::clamp(input.amount,.001f,1.f);
                if(!TraceScreenSpaceReflection(screen,ssr,x,y,Normalize(Vec3{.7f,.1f,-.3f}),hit))return {};
                color=hit.hit?Vec3{hit.u,hit.v,hit.confidence}:Vec3{.02f,.02f,.02f};
            }
            Pixel(*image,index,color);
        }
    }
    return image;
}
}
FeatureToolResult ExecuteFeatureTool(const FeatureToolInput& input,const std::atomic<bool>& cancel)
{
    FeatureToolResult result;std::vector<AssetDiagnostic> diagnostics;std::string error;
    try
    {
        std::error_code ec;if(std::filesystem::exists(input.destination)||!std::filesystem::create_directories(input.stage,ec)||ec){result.error="Feature.Tools.OutputExists";return result;}
        const auto file=[&](std::string name){const auto path=input.stage/asset_io::FromUtf8(name);result.artifacts.push_back(path.filename());return path;};
        bool ok=true;
        if(input.operation=="lod"||input.operation=="meshlets"||input.operation=="impostor")
        {
            if(!input.model||input.model->meshes.empty())ok=false;
            else for(std::size_t i=0;ok&&i<input.model->meshes.size()&&!cancel;++i)
            {
                const auto& mesh=input.model->meshes[i];const auto prefix="mesh-"+std::to_string(i);
                if(input.operation=="lod"){LodChain chain;ok=BakeLodChain(*input.model,static_cast<unsigned>(i),static_cast<unsigned>(std::clamp(input.amount,2.f,8.f)),chain,diagnostics)&&WriteLodChain(file(prefix+".selod"),chain,error);}
                else if(input.operation=="meshlets"){MeshletSet set;ok=BakeMeshlets(mesh,set,diagnostics)&&WriteMeshletSet(file(prefix+".semeshlet"),set,error);}
                else
                {
                    ImpostorAtlas atlas;ImpostorBakeSettings settings;settings.tileSize=static_cast<unsigned>(std::clamp(input.amount,16.f,128.f));
                    ok=BakeImpostorAtlas(mesh,Mat4{},settings,atlas,diagnostics)&&WriteImpostorAtlas(file(prefix+".seimp"),atlas,error);
                    if(ok&&!result.preview){result.preview=std::make_shared<ImageData>();result.preview->width=atlas.Width();result.preview->height=atlas.Height();result.preview->rgba8=atlas.rgba;}
                }
            }
        }
        else if(input.operation=="vertices")
        {
            nlohmann::json report={{"format","Flare.VertexCompressionAnalysis"},{"version",1},{"meshes",nlohmann::json::array()}};
            std::size_t sourceBytes=0,packedBytes=0;
            ok=input.model&&!input.model->meshes.empty();
            if(ok)for(const auto& mesh:input.model->meshes)
            {
                if(cancel)break;if(mesh.vertices.empty()){ok=false;break;}const auto& first=mesh.vertices.front().position;Aabb bounds{{first[0],first[1],first[2]},{first[0],first[1],first[2]}};
                for(const auto& vertex:mesh.vertices){const auto& p=vertex.position;bounds.minimum={std::min(bounds.minimum.x,p[0]),std::min(bounds.minimum.y,p[1]),std::min(bounds.minimum.z,p[2])};bounds.maximum={std::max(bounds.maximum.x,p[0]),std::max(bounds.maximum.y,p[1]),std::max(bounds.maximum.z,p[2])};}
                std::vector<PackedVertex> packed;MeshQuantization quantization;if(!CompressMeshVertices(mesh.vertices,bounds,packed,quantization,error)){ok=false;break;}
                float maximumError=0;for(std::size_t i=0;i<packed.size();++i){const auto& p=mesh.vertices[i].position;maximumError=std::max(maximumError,Length(DecodePackedPosition(packed[i],quantization)-Vec3{p[0],p[1],p[2]}));}
                report["meshes"].push_back({{"vertices",packed.size()},{"sourceBytes",mesh.vertices.size()*sizeof(MeshVertex)},{"packedBytes",packed.size()*sizeof(PackedVertex)},{"maximumPositionError",maximumError},{"minimum",quantization.minimum},{"extent",quantization.extent}});
                sourceBytes+=mesh.vertices.size()*sizeof(MeshVertex);packedBytes+=packed.size()*sizeof(PackedVertex);
            }
            if(ok&&!cancel){std::ofstream stream(file("vertex-compression.json"),std::ios::binary);stream<<report.dump(2);ok=bool(stream);result.detail=std::to_string(sourceBytes)+" -> "+std::to_string(packedBytes)+" B";}
        }
        else if(input.operation=="hlod")
        {
            std::vector<HlodInstance> instances;for(const auto& item:input.instances)
            {
                StaticModelLayout layout;if(!BuildStaticModelLayout(*item.model,layout,diagnostics)){ok=false;break;}
                for(const auto& part:layout.parts){const auto& mesh=item.model->meshes.at(part.mesh);std::string material;
                    if(mesh.material>=0&&static_cast<std::size_t>(mesh.material)<item.materials.size())material=item.materials[mesh.material];
                    else if(mesh.material<0&&!item.materials.empty())material=item.materials.front();
                    // 没有显式材质资产的部件不能合并为其他模型的同号材质。
                    if(material.empty()&&mesh.material>=0){result.error="Feature.Tools.HlodMaterial";return result;}
                    instances.push_back({&mesh,nullptr,item.world*part.modelFromMesh,0,std::move(material)});}
            }
            HlodScene scene;ok=ok&&!instances.empty()&&BakeHlod(instances,std::clamp(input.amount,1.f,1024.f),scene,diagnostics)&&WriteHlodScene(file("scene.sehlod"),scene,error);
        }
        else if(input.operation=="environment")
        {EnvironmentSettings settings;const auto requested=static_cast<unsigned>(std::clamp(input.amount,16.f,128.f));settings.cubeSize=16;while(settings.cubeSize<requested)settings.cubeSize*=2;settings.samples=128;bool hit=false;ok=CookEnvironment(input.source,file("environment.seenv"),settings,hit,diagnostics);}
        else if(input.operation=="bc1"||input.operation=="bc4"||input.operation=="bc5")
        {
            BakedTexture baked;ok=input.image&&(input.operation=="bc5"?BakeBc5Texture(*input.image,baked,error):input.operation=="bc4"?BakeBc4Texture(*input.image,baked,error):BakeBc1Texture(*input.image,BakedTextureFormat::Bc1UnormSrgb,baked,error));
            if(ok)ok=WriteBakedTexture(file("texture.sebc"),baked);
        }
        else if(input.operation=="shader")
        {
            for(const auto pass:UserShaderPasses)
            {
                if(cancel)break;const auto artifact=UserShaderVariantPath(input.stage/"shader.sebin",pass);result.artifacts.push_back(artifact.filename());
                ProcessRunResult process;const std::wstring mode=L"user-"+std::wstring(pass.begin(),pass.end());
                ok=RunEditorProcess(input.shaderBuilder.wstring(),{mode,input.source.wstring(),artifact.wstring()},input.stage,input.stage/(std::string(pass)+".log"),&cancel,process)&&process.exitCode==0;
                if(!ok){error="Feature.Tools.ShaderFailed";result.detail=process.output;break;}
            }
        }
        else if(input.operation=="navmesh")
        {
            NavBakeSettings settings;
            settings.cellSizeMeters=std::clamp(input.amount,0.05f,2.0f);
            NavMeshData navMesh;
            std::vector<std::uint8_t> bytes;
            ok=BakeNavMesh(input.navVertices,input.navIndices,settings,navMesh,error)&&navMesh.Serialize(bytes);
            if(ok&&!cancel)
            {
                ok=asset_io::WriteAtomic(file("scene.senav"),bytes);
                result.detail=std::to_string(navMesh.Polygons().size())+" polygons";
            }
        }
        else
        {result.preview=ReferencePreview(input,cancel);ok=static_cast<bool>(result.preview);}
        if(result.preview&&ok&&!cancel)ok=WritePpmImage(file("preview.ppm"),result.preview->width,result.preview->height,result.preview->rgba8,diagnostics);
        if(cancel){result.cancelled=true;result.error="Feature.Tools.Cancelled";return result;}
        if(!ok){result.error=error.empty()?(diagnostics.empty()?"Feature.Tools.InvalidInput":diagnostics.back().key):error;return result;}
        std::filesystem::create_directories(input.destination.parent_path(),ec);if(ec){result.error="Assets.WriteFailed";return result;}
        std::filesystem::rename(input.stage,input.destination,ec);if(ec){result.error="Assets.WriteFailed";return result;}
        result.directory=input.destination;for(auto& artifact:result.artifacts)artifact=result.directory/artifact;
        result.success=true;return result;
    }
    catch(const std::exception& exception){result.error="Feature.Tools.Failed";result.detail=exception.what();return result;}
}

EditorFeatureTools::EditorFeatureTools(std::filesystem::path root,AssetStore& assets,EditorSession& session,const Localization& text)
    :root_(std::move(root)),assets_(assets),session_(session),text_(text){}
EditorFeatureTools::~EditorFeatureTools(){cancel_=true;if(job_.valid())job_.wait();}
void EditorFeatureTools::Attach(EditorShell& shell)
{
    shell_=&shell;shell.SetFormHandlers(EditorPanelId::AssetTools,[this](auto key,auto value){return Commit(std::move(key),std::move(value));},[this]{return Start();});
    shell.SetFormPrimaryLabel(EditorPanelId::AssetTools,text_.GetText("Feature.Tools.Run"));
    shell.SetFormManagedStatus(EditorPanelId::AssetTools,true);
    shell.SetFormAction(EditorPanelId::AssetTools,text_.GetText("Feature.Tools.OpenOutput"),[this]{
        if(job_.valid()){cancel_=true;status_=text_.GetText("Feature.Tools.Cancelling");Refresh();return true;}
        if(output_.empty())return false;return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,L"open",output_.c_str(),nullptr,nullptr,SW_SHOWNORMAL))>32;});
    Refresh();
}
void EditorFeatureTools::Refresh()
{
    if(!shell_)return;const bool busy=job_.valid();std::vector<EditorProperty> rows;
    const auto combo=[&](const char* key,const char* label,const std::string& value,std::vector<std::string> options){
        EditorProperty row{key,text_.GetText(label),EditorPropertyType::Combo,value,busy,std::move(options)};
        for(const auto& option:row.options)row.optionLabels.push_back(text_.GetText("Feature.Tools."+option));rows.push_back(std::move(row));};
    combo("family","Feature.Tools.Category",family_,{"geometry","images","reference"});combo("operation","Feature.Tools.Operation",operation_,Operations(family_));
    rows.push_back({"source",text_.GetText("Feature.Tools.Source"),EditorPropertyType::String,source_,busy});
    if(operation_!="meshlets"&&operation_!="vertices"&&!operation_.starts_with("bc")&&operation_!="shader")
        rows.push_back({"amount",text_.GetText("Feature.Tools.Amount."+operation_),EditorPropertyType::Number,std::to_string(amount_),busy});
    rows.push_back({"scope",text_.GetText("Feature.Tools.Scope"),EditorPropertyType::String,text_.GetText(operation_=="vertices"?"Feature.Tools.VertexScope":family_=="reference"?"Feature.Tools.CpuReference":"Feature.Tools.Generated"),true});
    rows.push_back({"output",text_.GetText("Feature.Tools.Output"),EditorPropertyType::String,asset_io::Utf8(output_),true});
    shell_->SetFormProperties(EditorPanelId::AssetTools,std::move(rows));shell_->SetFormStatus(EditorPanelId::AssetTools,status_);
    shell_->SetFormActionLabel(EditorPanelId::AssetTools,text_.GetText(busy?"Feature.Tools.Cancel":"Feature.Tools.OpenOutput"));
}
bool EditorFeatureTools::Commit(std::string key,std::string value)
{
    if(job_.valid())return false;
    if(key=="source")source_=std::move(value);
    else if(key=="amount"){float n;auto [end,error]=std::from_chars(value.data(),value.data()+value.size(),n);if(error!=std::errc{}||end!=value.data()+value.size()||!IsFinite(n)||n<0||n>10000)return false;amount_=n;}
    else if(key=="family"||key=="operation")
    {
        if(key=="family"){if(value!="geometry"&&value!="images"&&value!="reference")return false;family_=value;operation_=Operations(family_).front();}
        else {const auto options=Operations(family_);if(std::find(options.begin(),options.end(),value)==options.end())return false;operation_=value;}
        amount_=operation_=="lod"?4.f:operation_=="hlod"||operation_=="impostor"||operation_=="environment"?64.f:operation_=="fog"?.01f:operation_=="ssr"?.05f:.5f;
    }
    else return false;refreshPending_=true;return true;
}
bool EditorFeatureTools::Start()
{
    if(job_.valid()||session_.IsPlaying())return false;
    if(source_.empty())if(const auto* mesh=session_.EditWorld().GetMesh(session_.EditWorld().FindByStableId(session_.Selected())))source_=mesh->meshAsset;
    FeatureToolInput input;input.operation=operation_;input.amount=amount_;
    input.source=assets_.Resolve(source_);if(input.source.empty())input.source=asset_io::FromUtf8(source_);if(!input.source.empty()&&input.source.is_relative())input.source=root_/input.source;
    std::vector<AssetDiagnostic> diagnostics;
    AssetId sourceId=source_;for(const auto& id:assets_.ListIds())if(asset_io::Within(assets_.SourceRoot(),input.source)&&assets_.Resolve(id).lexically_normal()==input.source.lexically_normal()){sourceId=id;break;}
    if(family_=="geometry"&&operation_!="hlod"&&operation_!="navmesh")input.model=Model(assets_,sourceId,diagnostics);
    if(operation_=="navmesh")CollectNavigationSceneGeometry(session_.EditWorld(),&assets_,input.navVertices,input.navIndices);
    if(operation_.starts_with("bc"))input.image=assets_.LoadTextureImage(sourceId,diagnostics);
    if(operation_=="hlod")for(auto entity:session_.EditWorld().Entities())if(const auto* mesh=session_.EditWorld().GetMesh(entity))
    {auto model=Model(assets_,mesh->meshAsset,diagnostics);if(model&&session_.EditWorld().IsEnabled(entity))input.instances.push_back({std::move(model),session_.EditWorld().GetWorldMatrix(entity),mesh->materials});}
    static std::atomic<unsigned> sequence=0;const auto name=std::to_string(GetTickCount64())+'-'+std::to_string(++sequence);
    input.stage=root_/L"out/AssetTools"/name;input.destination=assets_.SourceRoot()/L"Derived"/name;input.shaderBuilder=GetExecutableDirectory()/L"ShaderBuilder.exe";
    if(!std::filesystem::is_regular_file(input.shaderBuilder))input.shaderBuilder=GetExecutableDirectory().parent_path()/L"Tools/ShaderBuilder.exe";
    if(!asset_io::Within(root_,input.stage)||!asset_io::Within(assets_.SourceRoot(),input.destination))return false;
    cancel_=false;status_=text_.GetText("Feature.Tools.Running");job_=std::async(std::launch::async,[this,input=std::move(input)]{return ExecuteFeatureTool(input,cancel_);});Refresh();return true;
}
void EditorFeatureTools::Poll()
{
    if(refreshPending_){refreshPending_=false;Refresh();}
    if(!job_.valid()||job_.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return;
    auto result=job_.get();output_=result.directory;status_=text_.GetText(result.success?"Feature.Tools.Succeeded":result.cancelled?"Feature.Tools.Cancelled":"Feature.Tools.Failed");
    if(!result.error.empty())status_+=" "+text_.GetText(result.error);if(!result.detail.empty())status_+=" "+result.detail;
    if(result.success)
    {
        preview_=std::move(result.preview);++previewToken_;
        for(const auto& path:result.artifacts)
        {AssetId id;std::vector<AssetDiagnostic> diagnostics;if(!assets_.RegisterSource(path,{},id,diagnostics)&&!diagnostics.empty())status_+=" "+text_.GetText(diagnostics.back().key);}
        std::vector<EditorAssetIssue> issues;if(!EditorAssetActions::SyncProjectAssets(root_,assets_,issues)&&!issues.empty())status_+=" "+text_.GetText(issues.front().key);
        shell_->SetFormPreview(EditorPanelId::AssetTools,preview_?"feature-preview":"",preview_?float(preview_->width)/preview_->height:1);
    }
    Refresh();
}
}
