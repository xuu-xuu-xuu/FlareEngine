#include "ShadingResources.h"
#include "ResourceLayer/Assets/BrdfEnergyBake.h"
#include "ResourceLayer/Assets/LtcBake.h"
#include "ResourceLayer/Assets/SheenEnergyBake.h"
#include "ResourceLayer/Assets/SkinDiffuseBake.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "CoreLayer/Math/Half.h"
#include "CoreLayer/Hash.h"
#include <algorithm>
#include <set>

namespace se
{
namespace
{
struct Usage {bool layers=false,sheen=false,skin=false;};
template<class Material>void Include(const Material& m,Usage& u)
{
    u.sheen|=m.sheen.color.x>0||m.sheen.color.y>0||m.sheen.color.z>0;
    u.skin|=m.subsurface.weight>0;u.layers|=m.clearCoat.weight>0||u.sheen||u.skin||m.anisotropy.strength>0;
}
Usage MaterialUsage(const FrameData& frame)
{
    Usage result;std::vector<AssetDiagnostic> ignored;std::set<AssetId> models,materials;
    if(frame.materialPreview)Include(frame.materialPreview->document,result);
    for(const auto& mesh:frame.meshes)
    {
        for(const auto& id:mesh.mesh.materials)if(!id.empty())materials.insert(id);
        if(!mesh.mesh.meshAsset.empty()&&!IsBuiltinMesh(mesh.mesh.meshAsset))models.insert(mesh.mesh.meshAsset);
    }
    if(!frame.assets){if(!models.empty()||!materials.empty())result.layers=true;return result;}
    for(const auto& id:models)
    {
        if(const auto model=frame.assets->LoadModel(id,ignored))for(const auto& m:model->materials)Include(m,result);
        else result.layers=true;
    }
    for(const auto& id:materials)
    {
        if(frame.materialPreview&&frame.materialPreview->id==id)Include(frame.materialPreview->document,result);
        else if(const auto material=frame.assets->LoadMaterial(id,ignored))Include(*material,result);
        else result.layers=true;
    }
    return result;
}
void Fallback(std::vector<AssetDiagnostic>& d,const std::string& detail)
{asset_io::Diagnostic(d,"Rendering.ShadingResourceFallback",detail,true);}
std::string Stamp(const std::filesystem::path& path)
{
    std::error_code e;const auto size=std::filesystem::file_size(path,e);if(e)return asset_io::Utf8(path)+":missing";
    const auto time=std::filesystem::last_write_time(path,e);if(e)return asset_io::Utf8(path)+":unreadable";
    return asset_io::Utf8(path)+":"+std::to_string(size)+":"+std::to_string(time.time_since_epoch().count());
}
bool ReadResource(const std::filesystem::path& path,std::uint64_t maximum,std::vector<std::uint8_t>& bytes)
{std::error_code error;const auto size=std::filesystem::file_size(path,error);return !error&&size<=maximum&&asset_io::Read(path,bytes);}
struct FloatTexture {std::uint32_t width=0,height=0;std::vector<Vec4> pixels;std::string name;};
bool Upload(std::span<const FloatTexture> images,rhi::Device& device,std::vector<rhi::Texture>& output,std::vector<AssetDiagnostic>& diagnostics)
{
    for(const auto& image:images)if(!image.width||!image.height||image.width>16384||image.height>16384||image.pixels.size()!=std::size_t(image.width)*image.height)return false;
    auto result=device.BeginUploadBatch();if(!result){asset_io::Diagnostic(diagnostics,result.Key());return false;}
    struct Batch {rhi::Device& device;bool finished=false;~Batch(){if(!finished){rhi::CompletionToken ignored;device.EndUploadBatch(ignored);}}} batch{device};
    std::vector<rhi::Texture> textures(images.size());
    for(std::size_t i=0;i<images.size();++i)
    {
        const auto& image=images[i];const auto bytes=std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(image.pixels.data()),image.pixels.size()*sizeof(Vec4));
        const rhi::TextureSubresource subresource{bytes,0};
        result=device.CreateTextureSubresources({image.width,image.height,1,1,rhi::TextureFormat::Rgba32Float,image.name},std::span(&subresource,1),textures[i]);
        if(!result){asset_io::Diagnostic(diagnostics,result.Key(),image.name);return false;}
    }
    rhi::CompletionToken token;batch.finished=true;result=device.EndUploadBatch(token);
    if(!result){asset_io::Diagnostic(diagnostics,result.Key());return false;}output=std::move(textures);return true;
}
// source irradiance cube 存 E，既已卷积；投影后只除 pi，不再做第二次 cosine 卷积。
bool IrradianceSh(const EnvironmentAsset& environment,Sh9Rgb& output)
{
    if(!ValidateEnvironment(environment))return false;const auto& cube=environment.textures[0];double sum[9][3]{};
    const auto area=[](double x,double y){return std::atan2(x*y,std::sqrt(x*x+y*y+1));};
    for(std::uint32_t face=0;face<6;++face)for(std::uint32_t y=0;y<cube.size;++y)for(std::uint32_t x=0;x<cube.size;++x)
    {
        const double x0=2.0*x/cube.size-1,x1=2.0*(x+1)/cube.size-1,y0=2.0*y/cube.size-1,y1=2.0*(y+1)/cube.size-1;
        const double weight=(area(x1,y1)-area(x0,y1)-area(x1,y0)+area(x0,y0))/3.14159265358979323846;
        std::array<float,9> basis;if(!RealSh9Basis(EnvironmentCubeDirection(face,(float(x)+.5f)/cube.size,(float(y)+.5f)/cube.size),basis))return false;
        const auto& pixels=cube.subresources[face*cube.mipLevels];const auto offset=(std::size_t(y)*cube.size+x)*4;
        const double rgb[]{HalfToFloat(pixels[offset]),HalfToFloat(pixels[offset+1]),HalfToFloat(pixels[offset+2])};
        for(unsigned k=0;k<9;++k)for(unsigned c=0;c<3;++c)sum[k][c]+=rgb[c]*basis[k]*weight;
    }
    Sh9Rgb result;for(unsigned k=0;k<9;++k)result.coefficients[k]={float(sum[k][0]),float(sum[k][1]),float(sum[k][2])};if(!IsFinite(result))return false;output=result;return true;
}
bool ValidRectangle(const RectangleEmitter& e)
{
    if(!IsFinite(e.center)||!IsFinite(e.halfU)||!IsFinite(e.halfV)||!IsFinite(e.radiance)||e.radiance.x<0||e.radiance.y<0||e.radiance.z<0)return false;
    const double u=Dot(e.halfU,e.halfU),v=Dot(e.halfV,e.halfV),cross=LengthSquared(Cross(e.halfU,e.halfV));
    return std::isfinite(u)&&std::isfinite(v)&&std::isfinite(cross)&&u>1e-12&&v>1e-12&&cross>1e-20&&std::abs(Dot(e.halfU,e.halfV))<=1e-5*std::sqrt(u*v);
}
}
struct ShadingResourceCache::Impl
{
    rhi::Device* device=nullptr;
    std::string directAttempt,pairAttempt,skinAttempt,probeAttempt,probeAttemptKey,probeKey,environmentKey;
    std::string directHash,pairHash,skinHash,probeHash;
    rhi::Texture direct,matrix,amplitude,skin,probes;
    std::shared_ptr<const DiffuseProbeAsset> probeAsset;
    std::filesystem::path probePath;
    Sh9Rgb environmentSh;bool environmentValid=false;
    bool probeSelected=false,warnedMissingGlobal=false,warnedMissingLocal=false;
    void Direct(const std::filesystem::path& path,rhi::Device& d,std::vector<AssetDiagnostic>& diagnostics)
    {
        const auto stamp=Stamp(path);if(stamp==directAttempt)return;directAttempt=stamp;
        std::vector<std::uint8_t> bytes;BrdfEnergyLut lut;
        if(!ReadResource(path,16*1024*1024,bytes)||!DecodeBrdfEnergy(bytes,lut,diagnostics)||Stamp(path)!=stamp){Fallback(diagnostics,asset_io::Utf8(path.filename()));return;}
        const auto hash=Sha256(bytes);if(hash==directHash&&direct)return;
        FloatTexture image{lut.settings.width,lut.settings.height,{},"Shading.DirectEnergy"};image.pixels.reserve(lut.directional.size());
        for(std::uint32_t y=0;y<image.height;++y)for(std::uint32_t x=0;x<image.width;++x)
        {const auto ab=lut.directional[x+y*image.width],average=lut.average[y];image.pixels.push_back({ab.x,ab.y,average.x,average.y});}
        std::vector<rhi::Texture> uploaded;if(!Upload(std::span(&image,1),d,uploaded,diagnostics)){Fallback(diagnostics,image.name);return;}direct=uploaded[0];directHash=hash;
    }
    void Pair(const std::filesystem::path& ltcPath,const std::filesystem::path& sheenPath,rhi::Device& d,std::vector<AssetDiagnostic>& diagnostics)
    {
        const auto stamp=Stamp(ltcPath)+"|"+Stamp(sheenPath);if(stamp==pairAttempt)return;pairAttempt=stamp;
        std::vector<std::uint8_t> bytes,sheenBytes;LtcLut ltc;SheenEnergyLut sheen;
        if(!ReadResource(ltcPath,2*1024*1024,bytes)||!DecodeLtc(bytes,ltc,diagnostics)||!ReadResource(sheenPath,1024*1024,sheenBytes)||!LoadSheenEnergy(sheenPath,sheen)
            ||ltc.settings.size!=sheen.size||Stamp(ltcPath)+"|"+Stamp(sheenPath)!=stamp)
        {Fallback(diagnostics,"rectangle.seltc + sheen.sesheen");return;}
        const auto hash=Sha256(bytes)+Sha256(sheenBytes);if(hash==pairHash&&matrix&&amplitude)return;
        std::array<FloatTexture,2> images{{{ltc.settings.size,ltc.settings.size,ltc.inverseMatrices,"Shading.LtcMatrix"},{ltc.settings.size,ltc.settings.size,{},"Shading.LtcAmplitudeSheen"}}};
        images[1].pixels.reserve(ltc.magnitudeFresnel.size());
        for(std::size_t i=0;i<ltc.magnitudeFresnel.size();++i){const auto a=ltc.magnitudeFresnel[i],s=sheen.values[i];images[1].pixels.push_back({a.x,a.y,s.x,s.y});}
        std::vector<rhi::Texture> uploaded;if(!Upload(images,d,uploaded,diagnostics)){Fallback(diagnostics,"LTC/Sheen upload");return;}
        matrix=uploaded[0];amplitude=uploaded[1];pairHash=hash;
    }
    void Skin(const std::filesystem::path& path,rhi::Device& d,std::vector<AssetDiagnostic>& diagnostics)
    {
        const auto stamp=Stamp(path);if(stamp==skinAttempt)return;skinAttempt=stamp;
        std::vector<std::uint8_t> bytes;SkinDiffuseLut lut;
        if(!ReadResource(path,4*1024*1024,bytes)||!DecodeSkinDiffuse(bytes,lut,diagnostics)||Stamp(path)!=stamp){Fallback(diagnostics,asset_io::Utf8(path.filename()));return;}
        const auto hash=Sha256(bytes);if(hash==skinHash&&skin)return;
        FloatTexture image{lut.settings.width,lut.settings.height,{},"Shading.SkinDiffuse"};image.pixels.reserve(lut.pixels.size());for(const auto p:lut.pixels)image.pixels.push_back({p.x,p.y,p.z,1});
        std::vector<rhi::Texture> uploaded;if(!Upload(std::span(&image,1),d,uploaded,diagnostics)){Fallback(diagnostics,image.name);return;}skin=uploaded[0];skinHash=hash;
    }
    void Probes(const std::filesystem::path& path,const std::string& key,rhi::Device& d,std::vector<AssetDiagnostic>& diagnostics)
    {
        const auto stamp=Stamp(path);if(stamp==probeAttempt&&key==probeAttemptKey)return;probeAttempt=stamp;probeAttemptKey=key;
        std::vector<std::uint8_t> bytes;DiffuseProbeAsset asset;
        if(!ReadResource(path,16*1024*1024,bytes)||!DecodeDiffuseProbeAsset(bytes,asset,diagnostics)||Stamp(path)!=stamp){Fallback(diagnostics,asset_io::Utf8(path.filename()));return;}
        const auto hash=Sha256(bytes);if(hash==probeHash&&key==probeKey&&probeAsset)return;
        rhi::Texture texture;
        if(asset.local)
        {
            const auto& grid=*asset.local;const auto count=grid.diffuse.size();
            if(count>16384){Fallback(diagnostics,"local probe rows exceed 16384; global fallback");}
            else
            {
                FloatTexture image{10,static_cast<std::uint32_t>(count),{},"Shading.DiffuseProbes"};image.pixels.resize(count*10);
                for(std::size_t row=0;row<count;++row)
                {for(unsigned k=0;k<9;++k){const auto c=grid.diffuse[row].coefficients[k];image.pixels[row*10+k]={c.x,c.y,c.z,0};}image.pixels[row*10+9].x=float(grid.probeValid[row]);}
                const auto nx=grid.dimensions[0],ny=grid.dimensions[1],nz=grid.dimensions[2];
                for(std::uint32_t z=0;z+1<nz;++z)for(std::uint32_t y=0;y+1<ny;++y)for(std::uint32_t x=0;x+1<nx;++x)
                {const auto cell=x+y*(nx-1)+z*(nx-1)*(ny-1),row=x+y*nx+z*nx*ny;image.pixels[std::size_t(row)*10+9].y=float(grid.cellValid[cell]);}
                std::vector<rhi::Texture> uploaded;if(!Upload(std::span(&image,1),d,uploaded,diagnostics)){Fallback(diagnostics,image.name);return;}texture=uploaded[0];
            }
        }
        probeAsset=std::make_shared<const DiffuseProbeAsset>(std::move(asset));probes=std::move(texture);probePath=path;probeKey=key;probeHash=hash;
    }
};
bool RequiresMaterialShading(const FrameData& frame){return MaterialUsage(frame).layers;}
ShadingResourceCache::ShadingResourceCache():impl_(std::make_unique<Impl>()){}
ShadingResourceCache::~ShadingResourceCache()=default;
void ShadingResourceCache::Clear(){impl_=std::make_unique<Impl>();}
const std::vector<DiffuseProbeSource>& ShadingResourceCache::ProbeSources() const noexcept
{static const std::vector<DiffuseProbeSource> empty;return impl_->probeSelected&&impl_->probeAsset?impl_->probeAsset->sources:empty;}
const std::filesystem::path& ShadingResourceCache::ProbeAssetPath() const noexcept
{static const std::filesystem::path empty;return impl_->probeSelected?impl_->probePath:empty;}
bool ShadingResourceCache::Prepare(const FrameData& frame,const CameraSnapshot& camera,const RenderEnvironment* environment,rhi::Device& device,
    const std::filesystem::path& directory,rhi::ShadingFrameBindings& output,std::vector<AssetDiagnostic>& diagnostics)
{
    if(!IsFinite(camera.view)||!IsFinite(camera.projection)||!IsFinite(camera.viewport.width)||!IsFinite(camera.viewport.height)||camera.viewport.width<=0||camera.viewport.height<=0)return false;
    if(impl_->device&&impl_->device!=&device)Clear();impl_->device=&device;auto& p=*impl_;p.probeSelected=false;const auto usage=MaterialUsage(frame);
    rhi::ShadingFrameBindings result;result.enabled=usage.layers||frame.multiscatter||frame.diffuseSh||frame.localDiffuseSh||!frame.rectangleLights.empty()||frame.gtao||frame.contactShadows;
    result.constants.viewport={camera.viewport.width,camera.viewport.height,frame.gtao?1.f:0.f,frame.contactShadows?1.f:0.f};
    if(!result.enabled){output=result;return true;}
    try
    {
        const auto root=(directory.empty()?std::filesystem::current_path()/"Resources":directory)/"Lighting";
        if(frame.multiscatter)p.Direct(root/"direct-energy.sebrdf",device,diagnostics);
        if(usage.sheen||!frame.rectangleLights.empty())p.Pair(root/"rectangle.seltc",root/"sheen.sesheen",device,diagnostics);
        if(usage.skin)p.Skin(root/"skin.seskin",device,diagnostics);
        const bool pairReady=bool(p.matrix)&&bool(p.amplitude);result.constants.probeOrigin.w=pairReady?1.f:0.f;result.constants.probeDimensions.w=p.skin?1.f:0.f;
        result.constants.modes.x=frame.multiscatter&&p.direct?1.f:0.f;
        if(p.direct)result.textures[0]=*p.direct;if(p.matrix)result.textures[2]=*p.matrix;if(p.amplitude)result.textures[3]=*p.amplitude;if(p.skin)result.textures[4]=*p.skin;
        if(frame.diffuseSh||frame.localDiffuseSh)
        {
            std::string requestedProbe;
            if(frame.assets&&!frame.diffuseProbeAsset.empty())
            {
                requestedProbe=asset_io::Utf8(frame.assets->SourceRoot())+":"+frame.diffuseProbeAsset;
                const auto path=frame.assets->Resolve(frame.diffuseProbeAsset);
                if(!path.empty()&&asset_io::Within(frame.assets->SourceRoot(),path))p.Probes(path,requestedProbe,device,diagnostics);else Fallback(diagnostics,"diffuse probe asset unavailable");
            }
            Sh9Rgb global;bool valid=false;const bool assetSelected=!requestedProbe.empty()&&requestedProbe==p.probeKey&&bool(p.probeAsset);
            p.probeSelected=assetSelected;
            if(assetSelected){global=p.probeAsset->globalDiffuse;valid=IsFinite(global);}
            else if(environment&&environment->source)
            {
                if(p.environmentKey!=environment->source->cookKey)
                {Sh9Rgb candidate;if(IrradianceSh(*environment->source,candidate)){p.environmentSh=candidate;p.environmentValid=true;}else Fallback(diagnostics,"environment SH projection");p.environmentKey=environment->source->cookKey;}
                if(p.environmentValid){global=p.environmentSh;valid=true;}
            }
            if(valid)
            {
                result.constants.modes.y=1;for(unsigned i=0;i<9;++i){const auto c=global.coefficients[i];result.constants.globalSh[i]={c.x,c.y,c.z,0};}
                if(frame.localDiffuseSh&&assetSelected&&p.probeAsset->local&&p.probes)
                {
                    const auto& grid=*p.probeAsset->local;result.constants.modes.z=1;result.textures[1]=*p.probes;
                    result.constants.probeOrigin.x=grid.origin.x;result.constants.probeOrigin.y=grid.origin.y;result.constants.probeOrigin.z=grid.origin.z;
                    result.constants.probeSpacing={grid.spacing.x,grid.spacing.y,grid.spacing.z,0};
                    result.constants.probeDimensions.x=float(grid.dimensions[0]);result.constants.probeDimensions.y=float(grid.dimensions[1]);result.constants.probeDimensions.z=float(grid.dimensions[2]);
                }
            }
            if(!valid&&!p.warnedMissingGlobal)Fallback(diagnostics,"diffuse SH source unavailable; environment fallback");
            p.warnedMissingGlobal=!valid;
            const bool missingLocal=frame.localDiffuseSh&&result.constants.modes.z<.5f;
            if(missingLocal&&!p.warnedMissingLocal)Fallback(diagnostics,"local diffuse grid unavailable; global fallback");
            p.warnedMissingLocal=missingLocal;
        }
        if(pairReady)
        {
            std::vector<const RectangleLightInstance*> lights;for(const auto& light:frame.rectangleLights)lights.push_back(&light);
            std::stable_sort(lights.begin(),lights.end(),[](const auto* a,const auto* b){return a->entity<b->entity;});unsigned count=0;
            for(const auto* light:lights)
            {
                if(!ValidRectangle(light->emitter)){Fallback(diagnostics,"invalid rectangle emitter");continue;}if(count==4){Fallback(diagnostics,"rectangle light limit 4");break;}
                const auto& e=light->emitter;result.constants.rectCenter[count]={e.center.x,e.center.y,e.center.z,0};result.constants.rectHalfU[count]={e.halfU.x,e.halfU.y,e.halfU.z,0};
                result.constants.rectHalfV[count]={e.halfV.x,e.halfV.y,e.halfV.z,0};result.constants.rectRadiance[count]={e.radiance.x,e.radiance.y,e.radiance.z,0};++count;
            }
            result.constants.modes.w=float(count);
        }
        output=result;return true;
    }
    catch(const std::exception&){Fallback(diagnostics,"shading resources");output=result;return true;}
}
}
