#include "StaticModelRender.h"
#include "ResourceLayer/Assets/UserShaderVariants.h"
#include "CoreLayer/Log.h"
#include "MaterialInstance.h"
#include "ResourceLayer/Assets/VertexCompression.h"
#include "FunctionLayer/Rendering/MeshOptimization.h"
#include "ResourceLayer/Assets/AssetTasks.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Assets/TextureMip.h"
#include "CoreLayer/Hash.h"
#include <algorithm>
#include <bit>
#include <utility>
#include <unordered_set>

namespace flare
{
namespace
{
bool Invalid(std::vector<AssetDiagnostic>& diagnostics)
{
    asset_io::Diagnostic(diagnostics,"Rendering.InvalidModel");return false;
}
ImageData White(){return DefaultTextureImage(DefaultTextureKind::White);}
ImageData FlatNormal(){return DefaultTextureImage(DefaultTextureKind::FlatNormal);}
rhi::TextureFormat RhiFormat(BakedTextureFormat format)
{
    if(format==BakedTextureFormat::Bc1UnormSrgb)return rhi::TextureFormat::Bc1UnormSrgb;
    if(format==BakedTextureFormat::Bc5Unorm)return rhi::TextureFormat::Bc5Unorm;
    if(format==BakedTextureFormat::Bc4Unorm)return rhi::TextureFormat::Bc4Unorm;
    if(format==BakedTextureFormat::Bc7Unorm)return rhi::TextureFormat::Bc7Unorm;
    if(format==BakedTextureFormat::Bc7UnormSrgb)return rhi::TextureFormat::Bc7UnormSrgb;
    return rhi::TextureFormat::Bc1Unorm;
}
std::uint64_t BakedBlockBytes(BakedTextureFormat format)
{
    return format==BakedTextureFormat::Bc5Unorm||format==BakedTextureFormat::Bc7Unorm||format==BakedTextureFormat::Bc7UnormSrgb?16:8;
}
// G05：LOD 级别索引；lodIndices 只存 1..N-1 级（0 级复用 indices）。
const rhi::Buffer& LevelIndices(const StaticRenderMesh& mesh,std::uint32_t level)
{
    return level>=1&&level<=mesh.lodIndices.size()?mesh.lodIndices[level-1]:mesh.indices;
}
std::uint32_t LevelIndexCount(const StaticRenderMesh& mesh,std::uint32_t level)
{
    if(mesh.lod&&level<mesh.lod->levels.size())
        return static_cast<std::uint32_t>(mesh.lod->levels[level].indices.size());
    return mesh.indexCount;
}
float NormalScale(float scale,std::string_view name,std::vector<AssetDiagnostic>& diagnostics)
{
    if(scale<-8||scale>8)asset_io::Diagnostic(diagnostics,"Rendering.NormalScaleClamped",std::string(name),true);
    return std::clamp(scale,-8.0f,8.0f);
}
struct BatchGuard
{
    rhi::Device& device;bool finished=false;
    ~BatchGuard(){if(!finished){rhi::CompletionToken token;device.EndUploadBatch(token);}}
    rhi::Result Finish(){finished=true;rhi::CompletionToken token;return device.EndUploadBatch(token);}
};
void ConfigureLayers(const ClearCoatParameters& coat,const SheenParameters& sheen,const AnisotropyParameters& anisotropy,
    const SubsurfaceParameters& subsurface,PbrRenderMaterial& output,std::string_view name,std::vector<AssetDiagnostic>& diagnostics,
    float occlusionStrength=0.f,float specular=0.5f)
{
    auto& c=output.layers.constants;
    c.coat={coat.weight,coat.roughness,NormalScale(coat.normalScale,name,diagnostics),std::clamp(occlusionStrength,0.f,1.f)};
    c.sheen={sheen.color.x,sheen.color.y,sheen.color.z,sheen.roughness};
    c.anisotropy={anisotropy.strength,std::remainder(anisotropy.rotation,2*Pi),std::clamp(specular*2.f-1.f,-1.f,1.f),0};
    c.subsurface={subsurface.weight,subsurface.radius,0,0};c.subsurfaceColor={subsurface.color.x,subsurface.color.y,subsurface.color.z,0};
}
void BindLayerTextures(PbrRenderMaterial& material)
{
    std::uint32_t flags=0;
    for(std::size_t i=0;i<material.layerTextures.size();++i)if(material.layerTextures[i])
    {material.layers.textures[i]=*material.layerTextures[i];flags|=1u<<i;}
    material.layers.constants.textureFlags={float(flags),0,0,0};
    for(std::size_t i=0;i<material.userTextures.size();++i)
        material.layers.userTextures[i]=material.userTextures[i]?*material.userTextures[i]:rhi::TextureHandle{};
}
}

void ConfigurePbrMaterial(const MaterialData* material,PbrRenderMaterial& binding,std::string_view name,std::vector<AssetDiagnostic>& diagnostics)
{
    if(!material)return;
    const auto& color=material->baseColor;binding.color={color[0],color[1],color[2],color[3]};
    binding.metallic=material->metallic;binding.roughness=material->roughness;
    binding.alphaMode=material->alphaMode;binding.alphaCutoff=material->alphaCutoff;
    binding.normalScale=NormalScale(material->normalScale,name,diagnostics);binding.doubleSided=material->doubleSided;
    ConfigureLayers(material->clearCoat,material->sheen,material->anisotropy,material->subsurface,binding,name,diagnostics);
}
void StaticModelRenderCache::EnableBc1Textures(bool enabled,std::filesystem::path bakeDirectory,bool dataTextures)
{
    bc1Textures_=enabled;
    bc1DataTextures_=enabled&&dataTextures;
    if(!enabled)bc5Normals_=false;
    bakeCache_.SetDirectory(enabled?std::move(bakeDirectory):std::filesystem::path{});
}
std::optional<BakedTextureFormat> StaticModelRenderCache::BakedFormatFor(TextureRole role) const
{
    // 新层数据不套 BC1/BC5；法线保 RGB、sheen roughness 保 A，颜色视图由槽位选择 sRGB。
    if(role==TextureRole::LayerColor||role==TextureRole::LayerData)return std::nullopt;
    // 现有BC1编码器只编码RGB。覆盖与混合材质必须保留原始alpha，不能复用Opaque颜色缓存。
    if(role==TextureRole::CoverageColor)return bc7Colors_?std::optional(BakedTextureFormat::Bc7UnormSrgb):std::nullopt;
    if(role==TextureRole::Color)
        return bc7Colors_?std::optional(BakedTextureFormat::Bc7UnormSrgb):bc1Textures_?std::optional(BakedTextureFormat::Bc1UnormSrgb):std::nullopt;
    if(role==TextureRole::Normal)
    {
        if(bc5Normals_)return BakedTextureFormat::Bc5Unorm;
        return bc1Textures_&&bc1DataTextures_?std::optional(BakedTextureFormat::Bc1Unorm):std::nullopt;
    }
    return bc1Textures_&&bc1DataTextures_?std::optional(BakedTextureFormat::Bc1Unorm):std::nullopt;
}
std::string StaticModelRenderCache::TextureCacheKey(std::string_view contentKey,TextureRole role) const
{
    if(role==TextureRole::LayerColor)return "rgba8-layer-srgb:"+std::string(contentKey);
    if(role==TextureRole::LayerData)return "rgba8-layer-linear:"+std::string(contentKey);
    if(role==TextureRole::CoverageColor&&!bc7Colors_)return "rgba8-coverage:"+std::string(contentKey);
    const auto format=BakedFormatFor(role);
    return format?BakedTextureKey(contentKey,*format):std::string(contentKey);
}
bool StaticModelRenderCache::UploadTexture(const ImageData& image,std::string_view name,rhi::Device& device,
    rhi::Texture& texture,std::vector<AssetDiagnostic>& diagnostics,std::string_view preparedKey,TextureRole role)
{
    const auto contentKey=preparedKey.empty()?ImageContentKey(image):std::string(preparedKey);
    if(contentKey.empty())return Invalid(diagnostics);
    std::string key=TextureCacheKey(contentKey,role);
    if(const auto format=BakedFormatFor(role))
    {
        auto found=textures_.find(key);
        texture=found==textures_.end()?rhi::Texture{}:found->second.handle.lock();
        if(texture)return true;
        std::string error;
        const auto* baked=*format==BakedTextureFormat::Bc5Unorm
            ?bakeCache_.BakeBc5(image,contentKey,error)
            :bakeCache_.Bake(image,contentKey,*format,error);
        if(baked)
        {
            bool valid=baked->format==*format&&baked->width==image.width&&baked->height==image.height
                &&baked->mipLevels==Bc1MipLevels(image.width,image.height)&&baked->mipOffsets.size()==baked->mipLevels;
            std::uint64_t bytes=0;
            if(valid)for(std::uint32_t mip=0;mip<baked->mipLevels;++mip)
            {if(baked->mipOffsets[mip]!=bytes){valid=false;break;}bytes+=BakedMipBytes(*format,image.width,image.height,mip);}
            if(!valid||bytes!=baked->bytes.size())
            {asset_io::Diagnostic(diagnostics,"Rendering.TextureCacheRejected",std::string(name),true);baked=nullptr;}
        }
        if(baked)
        {
            rhi::TextureDescription description{image.width,image.height,baked->mipLevels,1,
                RhiFormat(*format),std::string(name)};
            const auto residencyId=TextureResidencyId(key);
            // 驻留按 mip0 字节与块尺寸记账；预算不足时从最粗 mip 开始，只上传驻留区间。
            residency_.Request(residencyId,baked->mipLevels,0,
                BakedMipBytes(*format,image.width,image.height,0),++residencyFrame_,BakedBlockBytes(*format));
            const auto* resident=residency_.Find(residencyId);
            const auto desired=resident?resident->desiredMip:0;
            const auto subresource=[&](std::uint32_t mip)
            {
                return rhi::TextureSubresource{std::span<const std::uint8_t>(baked->bytes).subspan(
                    baked->mipOffsets[mip],static_cast<std::size_t>(BakedMipBytes(*format,image.width,image.height,mip))),0};
            };
            std::vector<rhi::TextureSubresource> residentMips;
            for(std::uint32_t mip=desired;mip<baked->mipLevels;++mip)residentMips.push_back(subresource(mip));
            auto result=device.CreateTextureSubresourcesRange(description,desired,residentMips,texture);
            std::uint32_t uploaded=desired;
            if(!result)
            {
                // 区间上传失败时回退整链，避免留下未初始化的 mip。
                std::vector<rhi::TextureSubresource> allMips;
                for(std::uint32_t mip=0;mip<baked->mipLevels;++mip)allMips.push_back(subresource(mip));
                result=device.CreateTextureSubresources(description,allMips,texture);
                uploaded=0;
            }
            if(result)
            {
                if(desired&&!device.SetTextureMipRange(texture,desired))
                {
                    asset_io::Diagnostic(diagnostics,"D3D12.DescriptorCapacityExceeded",std::string(name));
                    return false;
                }
                if(desired)++pendingDegradations_;
                ++compressedUploads_;
                textures_.insert_or_assign(key,TextureEntry{texture,baked->mipLevels,residencyId,
                    desired,uploaded,*format,image.width,image.height});
                return true;
            }
            asset_io::Diagnostic(diagnostics,result.Key(),std::string(name));
        }
        else asset_io::Diagnostic(diagnostics,error.empty()?"Rendering.TextureEncodeFailed":error.c_str(),
            std::string(name),true);
        // 烘焙/上传失败时回退未压缩 RGBA8；键必须区分格式，避免复用到错误的采样视图。
        key="rgba8:"+contentKey;
    }
    auto found=textures_.find(key);
    texture=found==textures_.end()?rhi::Texture{}:found->second.handle.lock();
    if(texture)return true;
    if(rgba8Mipmaps_&&(image.width>=4||image.height>=4))
    {
        const auto mipCount=static_cast<std::uint32_t>(std::bit_width((std::max)(image.width,image.height)));
        std::vector<std::vector<std::uint8_t>> mipChain;
        mipChain.reserve(mipCount>1?mipCount-1:0);
        std::vector<rhi::TextureSubresource> subresources;
        subresources.reserve(mipCount);
        subresources.push_back({image.rgba8,0});
        std::span<const std::uint8_t> prevBytes=image.rgba8;
        std::uint32_t prevW=image.width,prevH=image.height;
        for(std::uint32_t mip=1;mip<mipCount;++mip)
        {
            std::vector<std::uint8_t> nextBytes;
            std::uint32_t nextW=1,nextH=1;
            asset_mip::DownsampleHalf(prevBytes,prevW,prevH,nextBytes,nextW,nextH);
            mipChain.push_back(std::move(nextBytes));
            prevBytes=mipChain.back();prevW=nextW;prevH=nextH;
            subresources.push_back({prevBytes,0});
        }
        const auto mipResult=device.CreateTextureSubresources(
            {image.width,image.height,mipCount,1,rhi::TextureFormat::Rgba8,std::string(name)},subresources,texture);
        if(!mipResult){asset_io::Diagnostic(diagnostics,mipResult.Key(),std::string(name));return false;}
        textures_.insert_or_assign(key,TextureEntry{texture,mipCount,0,0});return true;
    }
    const auto result=device.CreateTexture({image.width,image.height,1,1,rhi::TextureFormat::Rgba8,std::string(name)},image.rgba8,texture);
    if(!result){asset_io::Diagnostic(diagnostics,result.Key(),std::string(name));return false;}
    textures_.insert_or_assign(key,TextureEntry{texture,1,0,0});return true;
}
void StaticModelRenderCache::EnableBc7Colors(bool enabled,std::filesystem::path bakeDirectory)
{
    if(enabled!=bc7Colors_){Clear();bc7Colors_=enabled;}
    if(!bakeDirectory.empty())bakeCache_.SetDirectory(std::move(bakeDirectory));
}
std::uint64_t StaticModelRenderCache::TextureResidencyId(std::string_view key)
{
    const auto digest=Sha256(key);
    if(digest.size()<16)return 0;
    std::uint64_t value=0;
    for(int index=0;index<16;++index)
    {
        const char character=digest[static_cast<std::size_t>(index)];
        const std::uint64_t digit=character>='0'&&character<='9'?std::uint64_t(character-'0')
            :character>='a'&&character<='f'?std::uint64_t(character-'a'+10):0;
        value=(value<<4)|digit;
    }
    return value?value:1;
}
StaticModelRenderCache::ResidencyUpdate StaticModelRenderCache::UpdateTextureResidency(
    rhi::Device& device,std::uint64_t uploadBudgetBytes)
{
    ResidencyUpdate report;
    ++residencyFrame_;
    for(auto& [key,entry]:textures_)
    {
        if(!entry.residencyId||entry.handle.expired())continue;
        residency_.Touch(entry.residencyId,residencyFrame_);
    }
    // 预算有明显余量时把目标 mip 拉回最细（1.25× 迟滞），随后 Trim 决定实际驻留。
    const auto budget=residency_.Budget(),loaded=residency_.LoadedBytes();
    if(budget&&loaded*5<=budget*4)
        for(auto& [key,entry]:textures_)
            if(entry.residencyId&&!entry.handle.expired())residency_.SetDesired(entry.residencyId,0);
    report.degraded=residency_.TrimToBudget()+std::exchange(pendingDegradations_,0);
    std::uint64_t remaining=uploadBudgetBytes;
    for(auto& [key,entry]:textures_)
    {
        if(!entry.residencyId)continue;
        const auto handle=entry.handle.lock();
        if(!handle)continue;
        const auto* resident=residency_.Find(entry.residencyId);
        if(!resident)continue;
        // 需要更细的 mip 时从缺失区间的最粗端开始补传，单次受上传预算限制。
        if(resident->desiredMip<entry.uploadedMip&&remaining)
        {
            const auto* baked=bakeCache_.Find(key);
            if(baked&&baked->mipLevels==entry.mipLevels)
            {
                std::uint32_t count=0;std::uint64_t bytes=0;
                for(std::uint32_t mip=entry.uploadedMip;mip>resident->desiredMip;--mip)
                {
                    const auto level=mip-1;
                    const auto size=static_cast<std::size_t>(BakedMipBytes(entry.format,entry.width,entry.height,level));
                    const auto offset=baked->mipOffsets[level];
                    if(bytes+size>remaining||offset+size>baked->bytes.size())break;
                    bytes+=size;++count;
                }
                if(count==0&&entry.uploadedMip>resident->desiredMip)
                {
                    // 单级 mip 大于本次预算时也至少推进一级，避免永远停在同一个 mip。
                    const auto level=entry.uploadedMip-1;
                    const auto size=static_cast<std::size_t>(BakedMipBytes(entry.format,entry.width,entry.height,level));
                    const auto offset=baked->mipOffsets[level];
                    if(offset+size<=baked->bytes.size()){bytes=size;count=1;}
                }
                if(count)
                {
                    const auto start=entry.uploadedMip-count;
                    std::vector<rhi::TextureSubresource> missing;missing.reserve(count);
                    for(std::uint32_t level=start;level<entry.uploadedMip;++level)
                    {
                        const auto offset=baked->mipOffsets[level];
                        const auto size=static_cast<std::size_t>(BakedMipBytes(entry.format,entry.width,entry.height,level));
                        missing.push_back({std::span<const std::uint8_t>(baked->bytes).subspan(offset,size),0});
                    }
                    if(device.UploadTextureSubresourcesRange(handle,start,missing))
                    {
                        // 单个 mip 大于本次预算时允许推进一次，但不能让无符号预算下溢。
                        entry.uploadedMip=start;report.uploadedBytes+=bytes;remaining=bytes>=remaining?0:remaining-bytes;
                    }
                }
            }
        }
        // 只允许把 SRV 钳到“已经完整存在的 mip”上（desired 与 uploaded 的较粗者）。
        const auto target=(std::max)(resident->desiredMip,entry.uploadedMip);
        if(target!=entry.appliedMip&&device.SetTextureMipRange(handle,target))entry.appliedMip=target;
    }
    report.loadedBytes=residency_.LoadedBytes();
    report.budgetBytes=residency_.Budget();
    return report;
}
bool StaticModelRenderCache::Upload(std::shared_ptr<const StaticModel> model,const std::filesystem::path& source,
    const std::filesystem::path& allowedRoot,rhi::Device& device,
    std::shared_ptr<const StaticRenderModel>& output,std::vector<AssetDiagnostic>& diagnostics,bool compress,
    std::uint32_t lodLevels)
{
    auto candidate=std::make_shared<StaticRenderModel>();
    candidate->compressed=compress;
    if(!model||!BuildStaticModelLayout(*model,candidate->layout,diagnostics))return false;
    candidate->source=std::move(model);
    const auto& data=*candidate->source;
    // 所有 CPU 输入先解析完，路径拒绝不会上传半个模型。
    PreparedModelImages decoded;
    if(!PrepareModelImages(data,source,allowedRoot,decoded,diagnostics))return false;
    auto started=device.BeginUploadBatch();
    if(!started){asset_io::Diagnostic(diagnostics,started.Key(),asset_io::Utf8(source));return false;}
    BatchGuard batch{device};
    candidate->meshes.resize(data.meshes.size());candidate->fallbackMeshes.resize(data.meshes.size());
    for(std::size_t index=0;index<data.meshes.size();++index)
    {
        const auto& mesh=data.meshes[index];ModelMaterialImages selected;
        if(!SelectModelMaterialImages(data,decoded,index,selected))return Invalid(diagnostics);
        const bool fallback=selected.fallback[0];
        if(fallback)
        {
            candidate->state=ModelRenderState::Fallback;candidate->fallbackMeshes[index]=true;
            asset_io::Diagnostic(diagnostics,"Rendering.FallbackMaterial",mesh.name,true);
        }
        rhi::Texture texture;
        const auto colorRole=selected.material&&selected.material->alphaMode!=MaterialAlphaMode::Opaque
            ?TextureRole::CoverageColor:TextureRole::Color;
        if(!UploadTexture(*selected.images[0],mesh.name,device,texture,diagnostics,selected.keys[0],colorRole))return false;
        std::vector<rhi::Vertex> vertices;
        std::vector<rhi::SkinnedVertex> skinnedVertices;
        std::vector<PackedVertex> packed;
        auto& gpu=candidate->meshes[index];gpu.texture=std::move(texture);
        {
            Vec3 minimum{mesh.vertices[0].position[0],mesh.vertices[0].position[1],mesh.vertices[0].position[2]},maximum=minimum;
            for(const auto& vertex:mesh.vertices)
            {minimum={std::min(minimum.x,vertex.position[0]),std::min(minimum.y,vertex.position[1]),std::min(minimum.z,vertex.position[2])};
             maximum={std::max(maximum.x,vertex.position[0]),std::max(maximum.y,vertex.position[1]),std::max(maximum.z,vertex.position[2])};}
            gpu.localCenter=(minimum+maximum)*.5f;
            gpu.localBounds.minimum=minimum;gpu.localBounds.maximum=maximum;
        }
        // W03：蒙皮网格不能走压缩顶点布局（IMPORT 已保证 skinning 与 vertices 等长），
        // 80 字节 v3 顶点缓冲由 rhi::CreateBuffer 直接接受。
        const bool skinnedMesh=mesh.skinned&&mesh.skinning.size()==mesh.vertices.size();
        if(skinnedMesh)
        {
            skinnedVertices.resize(mesh.vertices.size());
            for(std::size_t vertex=0;vertex<mesh.vertices.size();++vertex)
            {
                auto& target=skinnedVertices[vertex];
                const auto& source=mesh.vertices[vertex];
                target.base={source.position,source.normal,source.uv,source.tangent};
                for(std::size_t influence=0;influence<4;++influence)
                {
                    target.joints[influence]=mesh.skinning[vertex].joints[influence];
                    target.weights[influence]=mesh.skinning[vertex].weights[influence];
                }
            }
            gpu.vertexLayoutVersion=rhi::VertexLayoutVersionSkinned;
            gpu.skinned=true;
            gpu.skinJointCount=static_cast<std::uint32_t>(data.joints.size());
        }
        else if(compress)
        {
            Aabb bounds;bounds.minimum={mesh.vertices[0].position[0],mesh.vertices[0].position[1],mesh.vertices[0].position[2]};bounds.maximum=bounds.minimum;
            for(const auto& vertex:mesh.vertices)
            {
                bounds.minimum.x=(std::min)(bounds.minimum.x,vertex.position[0]);
                bounds.minimum.y=(std::min)(bounds.minimum.y,vertex.position[1]);
                bounds.minimum.z=(std::min)(bounds.minimum.z,vertex.position[2]);
                bounds.maximum.x=(std::max)(bounds.maximum.x,vertex.position[0]);
                bounds.maximum.y=(std::max)(bounds.maximum.y,vertex.position[1]);
                bounds.maximum.z=(std::max)(bounds.maximum.z,vertex.position[2]);
            }
            MeshQuantization quantization;std::string error;
            if(!CompressMeshVertices(mesh.vertices,bounds,packed,quantization,error))
            {asset_io::Diagnostic(diagnostics,error.c_str(),mesh.name);return false;}
            gpu.vertexLayoutVersion=rhi::VertexLayoutVersionCompressed;
            gpu.quantizationMinimum={quantization.minimum[0],quantization.minimum[1],quantization.minimum[2]};
            gpu.quantizationExtent=quantization.extent[0];
        }
        else
        {
            vertices.reserve(mesh.vertices.size());
            for(const auto& vertex:mesh.vertices)vertices.push_back({vertex.position,vertex.normal,vertex.uv,vertex.tangent});
        }
        if(pbr_)
        {
            auto binding=std::make_shared<PbrRenderMaterial>();binding->baseColor=gpu.texture;
            ConfigurePbrMaterial(selected.material,*binding,mesh.name,diagnostics);
            rhi::Texture* targets[]{&binding->normal,&binding->metallicRoughness};
            for(std::size_t slot=0;slot<2;++slot)
            {
                const bool bad=selected.fallback[slot+1];
                if(!UploadTexture(*selected.images[slot+1],mesh.name,device,*targets[slot],diagnostics,selected.keys[slot+1],
                    slot==0?TextureRole::Normal:TextureRole::Data))return false;
                if(bad)
                {
                    candidate->state=ModelRenderState::Fallback;
                    asset_io::Diagnostic(diagnostics,"Rendering.FallbackMaterial",mesh.name,true);
                }
            }
            binding->normalMapping=selected.normalMapping;
            if(selected.material)
            {
                const auto& m=*selected.material;const std::int32_t layerIndices[]{m.clearCoatWeightTexture,m.clearCoatRoughnessTexture,m.clearCoatNormalTexture,m.sheenColorTexture,m.sheenRoughnessTexture,m.anisotropyTexture};
                for(std::size_t slot=0;slot<6;++slot)if(layerIndices[slot]>=0)
                {
                    const auto image=static_cast<std::size_t>(layerIndices[slot]);
                    if(image>=decoded.images.size()||image>=decoded.unavailable.size()||image>=decoded.keys.size())return Invalid(diagnostics);
                    const bool bad=decoded.unavailable[image];
                    const auto& pixels=bad?DefaultTextureImage(slot==2?DefaultTextureKind::FlatNormal:DefaultTextureKind::White):decoded.images[image];
                    if(!UploadTexture(pixels,mesh.name+".layer"+std::to_string(slot),device,binding->layerTextures[slot],diagnostics,
                        bad?std::string_view{}:std::string_view(decoded.keys[image]),slot==3?TextureRole::LayerColor:TextureRole::LayerData))return false;
                    if(bad){candidate->state=ModelRenderState::Fallback;asset_io::Diagnostic(diagnostics,"Rendering.FallbackMaterial",mesh.name,true);}
                }
            }
            BindLayerTextures(*binding);
            binding->featureMask=flare::FeatureMaskFor(*binding);
            gpu.material=std::move(binding);
        }
        auto result=skinnedMesh
            ?device.CreateBuffer({rhi::BufferUsage::Vertex,sizeof(rhi::SkinnedVertex),mesh.name+".vertices"},std::as_bytes(std::span(skinnedVertices)),gpu.vertices)
            :compress
            ?device.CreateBuffer({rhi::BufferUsage::Vertex,sizeof(PackedVertex),mesh.name+".vertices"},std::as_bytes(std::span(packed)),gpu.vertices)
            :device.CreateBuffer({rhi::BufferUsage::Vertex,sizeof(rhi::Vertex),mesh.name+".vertices"},std::as_bytes(std::span(vertices)),gpu.vertices);
        const auto reordered=flare::OptimizeVertexCache(mesh.indices);
        if(meshletSubmission_&&!skinnedMesh&&!compress)
        {
            MeshData sourceMesh;sourceMesh.name=mesh.name;sourceMesh.vertices=mesh.vertices;sourceMesh.indices=reordered;
            auto submission=std::make_shared<MeshletSubmission>();
            if(BakeMeshletSubmission(sourceMesh,*submission,diagnostics)&&submission->indices==reordered)
            {
                gpu.geometryClusters.reserve(submission->ranges.size());
                for(const auto& range:submission->ranges)gpu.geometryClusters.push_back({range.firstIndex,range.indexCount,range.localBounds});
                gpu.meshlets=std::move(submission);
            }
            else asset_io::Diagnostic(diagnostics,"Rendering.MeshletCpuFallback",mesh.name,true);
        }
        std::vector<std::uint16_t> narrowIndices;
        const bool narrow=mesh.vertices.size()<=65535;
        if(narrow){narrowIndices.reserve(reordered.size());for(const auto value:reordered)narrowIndices.push_back(static_cast<std::uint16_t>(value));}
        if(result)result=narrow
            ?device.CreateBuffer({rhi::BufferUsage::Index,sizeof(std::uint16_t),mesh.name+".indices"},std::as_bytes(std::span(narrowIndices)),gpu.indices)
            :device.CreateBuffer({rhi::BufferUsage::Index,sizeof(std::uint32_t),mesh.name+".indices"},std::as_bytes(std::span(reordered)),gpu.indices);
        if(!result){asset_io::Diagnostic(diagnostics,result.Key(),mesh.name);return false;}
        gpu.indexCount=static_cast<std::uint32_t>(mesh.indices.size());
        if(lodLevels>=2)
        {
            LodChain chain;
            if(BakeLodChain(data,static_cast<std::uint32_t>(index),lodLevels,chain,diagnostics))
            {
                for(std::uint32_t level=1;level<chain.levels.size();++level)
                {
                    rhi::Buffer buffer;rhi::Result lodResult;
                    if(narrow)
                    {
                        std::vector<std::uint16_t> values;values.reserve(chain.levels[level].indices.size());
                        for(const auto value:chain.levels[level].indices)values.push_back(static_cast<std::uint16_t>(value));
                        lodResult=device.CreateBuffer({rhi::BufferUsage::Index,sizeof(std::uint16_t),
                            mesh.name+".lod"+std::to_string(level)},std::as_bytes(std::span(values)),buffer);
                    }
                    else lodResult=device.CreateBuffer({rhi::BufferUsage::Index,sizeof(std::uint32_t),
                        mesh.name+".lod"+std::to_string(level)},std::as_bytes(std::span(chain.levels[level].indices)),buffer);
                    if(!lodResult){asset_io::Diagnostic(diagnostics,lodResult.Key(),mesh.name);return false;}
                    gpu.lodIndices.push_back(std::move(buffer));
                }
                gpu.lod=std::make_shared<const LodChain>(std::move(chain));
            }
            else asset_io::Diagnostic(diagnostics,"Rendering.LodAssetInvalid",mesh.name,true);
        }
    }
    const auto submitted=batch.Finish();
    if(!submitted){asset_io::Diagnostic(diagnostics,submitted.Key(),asset_io::Utf8(source));return false;}
    output=std::move(candidate);return true;
}

std::shared_ptr<const StaticRenderModel> StaticModelRenderCache::Load(AssetStore& assets,const AssetId& id,
    rhi::Device& device,std::vector<AssetDiagnostic>& diagnostics)
{
    const auto source=assets.Resolve(id);
    const auto key=asset_io::Utf8(assets.SourceRoot())+":"+id;
    const auto previous=models_.find(key);
    auto old=previous==models_.end()?std::shared_ptr<const StaticRenderModel>{}:previous->second;
    auto cpu=assets.LoadModel(id,diagnostics);
    if(!cpu)return old;
    if(old&&old->source==cpu)return old;
    const auto attempted=attempted_.find(key);
    if(attempted!=attempted_.end()&&attempted->second==cpu)return old;
    attempted_.insert_or_assign(key,cpu);
    std::shared_ptr<const StaticRenderModel> candidate;
    if(!Upload(std::move(cpu),source,assets.SourceRoot(),device,candidate,diagnostics,false,lodLevels_))return old;
    // 指定贴图加载失败不能把已就绪的旧版本替换为降级候选。
    if(old&&candidate->state==ModelRenderState::Fallback)return old;
    models_.insert_or_assign(key,candidate);
    return candidate;
}
std::shared_ptr<const PbrRenderMaterial> StaticModelRenderCache::LoadMaterial(AssetStore& assets,const AssetId& id,
    rhi::Device& device,std::vector<AssetDiagnostic>& diagnostics,const MaterialDocument* preview)
{
    if(!pbr_)return {};
    const auto key=asset_io::Utf8(assets.SourceRoot())+":"+id;
    const auto previous=materials_.find(key);
    const auto old=previous==materials_.end()?std::shared_ptr<const PbrRenderMaterial>{}:previous->second.gpu;
    std::string previewKey;std::shared_ptr<const MaterialDocument> source;
    if(preview)
    {
        if(assets.Resolve(id).empty()){asset_io::Diagnostic(diagnostics,"Assets.UnknownId",id);return old;}
        if(!SerializeMaterialDocument(*preview,previewKey,diagnostics))return old;
        source=previous!=materials_.end()&&previous->second.previewKey==previewKey?previous->second.source:std::make_shared<const MaterialDocument>(*preview);
    }
    else source=assets.LoadMaterial(id,diagnostics);
    if(!source)return old;
    const bool isUserSurface=!source->shader.empty()&&IsUserSurfaceAssetPath(assets.Resolve(source->shader));
    if(!isUserSurface&&source->alphaMode!=MaterialAlphaMode::Opaque&&!source->shader.empty())
        asset_io::Diagnostic(diagnostics,"Rendering.MaterialShaderCoverageFallback",source->name,true);
    const bool layered=source->clearCoat.weight>0||source->sheen.color.x>0||source->sheen.color.y>0||source->sheen.color.z>0
        ||source->anisotropy.strength>0||source->subsurface.weight>0;
    if(!isUserSurface&&layered&&!source->shader.empty()&&(previous==materials_.end()||previous->second.source!=source))
        asset_io::Diagnostic(diagnostics,"Material.ShaderFallback",source->shader,true);
    // 先按内容修订解析用户产物：失败时保留上一有效材质/Pipeline；首次加载失败退回默认 PBR 并记录诊断。
    ShaderArtifactSource artifact;std::string shaderKey;
    std::shared_ptr<const UserSurfaceShaderSource> loadedUserSurface;
    if(isUserSurface)
    {
        UserSurfaceShaderSource userSrc;
        if(assets.LoadSurfaceShader(source->shader,userSrc,diagnostics))
        {
            userSrc.materialParameters=source->shaderParameters;
            shaderKey="user:"+userSrc.content_sha256;
            loadedUserSurface=std::make_shared<const UserSurfaceShaderSource>(std::move(userSrc));
        }
        else if(previous!=materials_.end())
        {
            return old;
        }
    }
    else if(!source->shader.empty()&&customMaterialPipelines_&&source->alphaMode==MaterialAlphaMode::Opaque&&!layered)
    {
        if(assets.LoadShaderArtifact(source->shader,artifact,diagnostics))
        {
            if(materialKind_==rhi::PipelineKind::PbrMesh)shaderKey=artifact.content_sha256;
            else
            {
                const auto pass=materialKind_==rhi::PipelineKind::EnvironmentMesh?"environment":materialKind_==rhi::PipelineKind::ShadowedMesh?"shadowed":"forward";
                const auto path=UserShaderVariantPath(artifact.source,pass);std::vector<std::uint8_t> bytes;
                if(asset_io::Within(assets.SourceRoot(),path)&&asset_io::Read(path,bytes)){artifact.source=path;shaderKey=asset_io::Hash(bytes);}
                if(shaderKey.empty())asset_io::Diagnostic(diagnostics,"Material.ShaderFallback",source->shader);
            }
        }
        else
        {
            if(previous!=materials_.end())return old;
            asset_io::Diagnostic(diagnostics,"Material.ShaderFallback",source->shader);
        }
    }
    AssetId ids[13]{source->baseColorTexture,source->normalTexture,source->metallicRoughnessTexture,
        source->clearCoatWeightTexture,source->clearCoatRoughnessTexture,source->clearCoatNormalTexture,source->sheenColorTexture,source->sheenRoughnessTexture,source->anisotropyTexture};
    std::array<std::uint8_t,4> userDefKinds{};
    std::array<bool,4> userLinear{};
    if(loadedUserSurface)
    {
        for(const auto& texMeta:loadedUserSurface->metadata.textures)
        {
            if(texMeta.slotIndex>=4)continue;
            userDefKinds[texMeta.slotIndex]=static_cast<std::uint8_t>(texMeta.defaultKind);
            userLinear[texMeta.slotIndex]=(texMeta.defaultKind==SurfaceTextureDefault::Bump);
            if(const auto it=source->shaderTextures.find(texMeta.name);it!=source->shaderTextures.end())
                ids[9+texMeta.slotIndex]=it->second;
        }
    }
    std::array<std::shared_ptr<const ImageData>,13> images;
    for(std::size_t index=0;index<13;++index)if(!ids[index].empty())
    {images[index]=assets.LoadTextureImage(ids[index],diagnostics);if(!images[index])return old;}
    if(previous!=materials_.end()&&previous->second.source==source&&previous->second.images==images
        &&previous->second.previewKey==previewKey&&previous->second.shaderKey==shaderKey)return old;
    auto gpu=std::make_shared<PbrRenderMaterial>();const auto& c=source->baseColor;gpu->color={c[0],c[1],c[2],c[3]};
    gpu->metallic=source->metallic;gpu->roughness=source->roughness;gpu->doubleSided=source->doubleSided;
    gpu->alphaMode=source->alphaMode;gpu->alphaCutoff=source->alphaCutoff;
    if(loadedUserSurface)
    {
        if(loadedUserSurface->metadata.blendOverride)gpu->alphaMode=*loadedUserSurface->metadata.blendOverride;
        if(loadedUserSurface->metadata.doubleSidedOverride)gpu->doubleSided=*loadedUserSurface->metadata.doubleSidedOverride;
        gpu->userParams=PackSurfaceShaderParameters(loadedUserSurface->metadata,source->shaderParameters);
        gpu->userSurface=loadedUserSurface;
        gpu->layers.userTextureDefaults=userDefKinds;
        gpu->layers.userTextureLinear=userLinear;
    }
    gpu->normalScale=NormalScale(source->normalScale,source->name,diagnostics);gpu->normalMapping=!source->normalTexture.empty();
    ConfigureLayers(source->clearCoat,source->sheen,source->anisotropy,source->subsurface,*gpu,source->name,diagnostics,source->occlusionStrength,source->specular);
    rhi::Texture* targets[]{&gpu->baseColor,&gpu->normal,&gpu->metallicRoughness,&gpu->layerTextures[0],&gpu->layerTextures[1],&gpu->layerTextures[2],&gpu->layerTextures[3],&gpu->layerTextures[4],&gpu->layerTextures[5],
        &gpu->userTextures[0],&gpu->userTextures[1],&gpu->userTextures[2],&gpu->userTextures[3]};
    const auto roleFor=[&](std::size_t index){
        if(index>=9)return userLinear[index-9]?TextureRole::LayerData:TextureRole::LayerColor;
        return index>=3?(index==6?TextureRole::LayerColor:TextureRole::LayerData)
            :index==0?((gpu->alphaMode==MaterialAlphaMode::Opaque&&!(loadedUserSurface&&loadedUserSurface->metadata.usesAlphaCoverage))?TextureRole::Color:TextureRole::CoverageColor):index==1?TextureRole::Normal:TextureRole::Data;
    };
    std::array<const ImageData*,13> pixels{};std::array<std::string,13> keys;bool needsUpload=false;
    for(std::size_t index=0;index<13;++index)
    {
        if(index>=3&&!images[index])continue;
        pixels[index]=images[index]?images[index].get():&DefaultTextureImage(index==1?DefaultTextureKind::FlatNormal:DefaultTextureKind::White);
        keys[index]=ImageContentKey(*pixels[index]);if(keys[index].empty()){Invalid(diagnostics);return old;}
        const auto role=roleFor(index);
        const auto cached=textures_.find(TextureCacheKey(keys[index],role));
        *targets[index]=cached==textures_.end()?rhi::Texture{}:cached->second.handle.lock();needsUpload=needsUpload||!*targets[index];
    }
    if(needsUpload)
    {
        // 多贴图高分辨率材质（如 4K 图集 + 多槽自定义 2D 贴图）未压缩 mip 链总和可超单帧 256 MB 上传池；
        // 按 128 MB 阈值分批提交 UploadBatch，普通材质仍保持单批次零额外开销。
        constexpr std::uint64_t maxBatchBytes=128ull*1024*1024;
        struct ScopedUploadBatch
        {
            rhi::Device* dev=nullptr;
            ~ScopedUploadBatch(){if(dev){rhi::CompletionToken ignored;dev->EndUploadBatch(ignored);}}
            rhi::Result Flush(){auto* d=std::exchange(dev,nullptr);rhi::CompletionToken token;return d->EndUploadBatch(token);}
        } activeBatch;
        std::uint64_t batchBytes=0;
        for(std::size_t index=0;index<13;++index)
        {
            if(!pixels[index]||*targets[index])continue;
            const auto estBytes=static_cast<std::uint64_t>(pixels[index]->rgba8.size())*3ull/2ull+65536ull;
            if(activeBatch.dev&&batchBytes>0&&batchBytes+estBytes>maxBatchBytes)
            {
                const auto flushed=activeBatch.Flush();batchBytes=0;
                if(!flushed){asset_io::Diagnostic(diagnostics,flushed.Key(),id);return old;}
            }
            if(!activeBatch.dev)
            {
                const auto started=device.BeginUploadBatch();
                if(!started){asset_io::Diagnostic(diagnostics,started.Key(),id);return old;}
                activeBatch.dev=&device;
            }
            const auto role=roleFor(index);
            if(!UploadTexture(*pixels[index],source->name,device,*targets[index],diagnostics,keys[index],role))return old;
            batchBytes+=estBytes;
        }
        if(activeBatch.dev)
        {
            const auto finished=activeBatch.Flush();
            if(!finished){asset_io::Diagnostic(diagnostics,finished.Key(),id);return old;}
        }
    }
    BindLayerTextures(*gpu);
    if(!shaderKey.empty()&&!loadedUserSurface)
    {
        if(previous!=materials_.end()&&previous->second.shaderKey==shaderKey&&old&&old->pipeline)gpu->pipeline=old->pipeline;
        else
        {
            rhi::PipelineDescription description;
            description.kind=materialKind_;
            description.colorFormat=rhi::IsForward(materialKind_)?rhi::ColorFormat::Rgba16Float:rhi::ColorFormat::Rgba8Unorm;
            description.shader=artifact.source;
            description.bindingLayoutVersion=rhi::BindingLayout(materialKind_);
            description.shaderInput=rhi::ShaderInput::Artifact;
            rhi::Pipeline created;
            if(device.CreatePipeline(description,created))gpu->pipeline=created;
            else
            {
                asset_io::Diagnostic(diagnostics,"Material.ShaderPipelineFailed",source->shader);
                if(previous!=materials_.end())return old;
                asset_io::Diagnostic(diagnostics,"Material.ShaderFallback",source->shader);
            }
        }
    }
    materials_.insert_or_assign(key,MaterialRevision{std::move(source),std::move(images),gpu,std::move(previewKey),std::move(shaderKey)});return gpu;
}
void StaticModelRenderCache::Prune(const AssetStore* assets,std::span<const AssetId> needed,std::span<const AssetId> neededMaterials)
{
    std::unordered_set<std::string> keep;
    if(assets)for(const auto& id:needed)keep.insert(asset_io::Utf8(assets->SourceRoot())+":"+id);
    std::erase_if(models_,[&](const auto& entry){return !keep.contains(entry.first);});
    std::erase_if(attempted_,[&](const auto& entry){return !keep.contains(entry.first);});
    keep.clear();if(assets)for(const auto& id:neededMaterials)keep.insert(asset_io::Utf8(assets->SourceRoot())+":"+id);
    std::erase_if(materials_,[&](const auto& entry){return !keep.contains(entry.first);});
    std::erase_if(textures_,[](const auto& entry){return entry.second.handle.expired();});
}
std::size_t StaticModelRenderCache::TextureCount() const noexcept
{return static_cast<std::size_t>(std::count_if(textures_.begin(),textures_.end(),[](const auto& entry){return !entry.second.handle.expired();}));}
Mat4 DrawWorld(const StaticRenderModel& model,const StaticModelPart& part,const Mat4& entityWorld)
{
    auto world=entityWorld*part.modelFromMesh;
    if(model.compressed)
    {
        const auto& mesh=model.meshes[part.mesh];
        const float extent=mesh.quantizationExtent;
        world=world*Translation(mesh.quantizationMinimum)*Scaling({extent,extent,extent});
    }
    return world;
}
void AppendStaticModelDraws(const StaticRenderModel& model,const Mat4& entityWorld,
    std::vector<rhi::DrawIndexed>& output,rhi::PipelineHandle pipeline,const Mat4& viewProjection,bool unlit,
    std::uint32_t lodLevel)
{
    for(const auto& part:model.layout.parts)
    {
        const bool fallback=model.fallbackMeshes[part.mesh];
        const auto& mesh=model.meshes[part.mesh];
        const auto indices=LevelIndices(mesh,lodLevel);
        output.push_back({*mesh.vertices,*indices,*mesh.texture,pipeline,DrawWorld(model,part,entityWorld),viewProjection,
            fallback?Vec4{1,1,1,1}:part.baseColor,false,unlit||fallback});
        output.back().indexCount=LevelIndexCount(mesh,lodLevel);
        if(!mesh.skinned&&!model.compressed)
            if(const auto bounds=TransformBounds(mesh.localBounds,output.back().world))
            {output.back().worldBounds=*bounds;output.back().hasWorldBounds=true;}
        if(lodLevel==0)output.back().geometryClusters=mesh.geometryClusters;
    }
}
void AppendShadowModelDraws(const StaticRenderModel& model,const Mat4& entityWorld,std::vector<rhi::DrawIndexed>& output,
    std::uint32_t lodLevel,std::span<const std::byte> skinPalette,std::uint32_t skinJointCount,rhi::PipelineHandle skinPipeline,
    std::span<const std::shared_ptr<const PbrRenderMaterial>> overrides,
    std::vector<std::shared_ptr<const UserSurfaceShaderSource>>* outUserSurfaces)
{
    for(const auto& part:model.layout.parts)
    {
        const auto& mesh=model.meshes[part.mesh];rhi::DrawIndexed draw;
        auto material=mesh.material;const auto materialIndex=model.source->meshes[part.mesh].material;
        bool overridden=false;
        if(materialIndex>=0&&static_cast<std::size_t>(materialIndex)<overrides.size()&&overrides[static_cast<std::size_t>(materialIndex)])
        {material=overrides[static_cast<std::size_t>(materialIndex)];overridden=true;}
        if(material&&material->alphaMode==MaterialAlphaMode::Blend)continue;
        if(material)
        {draw.texture=*material->baseColor;draw.tint=material->color;draw.alphaMode=material->alphaMode;
         draw.alphaCutoff=material->alphaCutoff;draw.pbr.metallic=material->metallic;
         draw.pbr.roughness=material->roughness;draw.pbr.doubleSided=material->doubleSided;
         draw.userParams=material->userParams;
         draw.unlit=model.fallbackMeshes[part.mesh]&&!overridden;}
        draw.compressedVertices=mesh.vertexLayoutVersion==rhi::VertexLayoutVersionCompressed;
        draw.vertices=*mesh.vertices;draw.indices=*LevelIndices(mesh,lodLevel);draw.world=DrawWorld(model,part,entityWorld);
        // W03：蒙皮部件的顶点缓冲是 80 字节布局，必须配蒙皮深度 PSO 与调色板；
        // 缺其中任何一项时跳过该部件，避免把蒙皮顶点当参考顶点读成错误几何。
        if(mesh.skinned)
        {
            if(skinPalette.empty()||!skinPipeline||!skinJointCount)continue;
            draw.pipeline=skinPipeline;draw.skinPalette=skinPalette;draw.skinJointCount=skinJointCount;
        }
        else if(!model.compressed)
        {
            if(const auto bounds=TransformBounds(mesh.localBounds,draw.world))
            {draw.worldBounds=*bounds;draw.hasWorldBounds=true;}
        }
        output.push_back(draw);
        if(outUserSurfaces)outUserSurfaces->push_back(material?material->userSurface:nullptr);
    }
}
void AppendPbrModelDraws(const StaticRenderModel& model,const Mat4& entityWorld,
    std::span<const std::shared_ptr<const PbrRenderMaterial>> overrides,std::vector<rhi::DrawIndexed>& output,
    rhi::PipelineHandle pipeline,const Mat4& viewProjection,Vec3 camera,Vec3 directionToLight,Vec3 radiance,
    std::uint32_t lodLevel,float lodDither,const Mat4* previousWorld,Vec4 look,std::span<const std::byte> skinPalette,
    std::uint32_t skinJointCount,rhi::PipelineHandle skinPipeline,
    std::vector<std::shared_ptr<const UserSurfaceShaderSource>>* outUserSurfaces)
{
    for(const auto& part:model.layout.parts)
    {
        const auto& mesh=model.meshes[part.mesh];const auto materialIndex=model.source->meshes[part.mesh].material;
        auto material=mesh.material;
        bool overridden=false;
        if(materialIndex>=0&&static_cast<std::size_t>(materialIndex)<overrides.size()&&overrides[static_cast<std::size_t>(materialIndex)])
        {material=overrides[static_cast<std::size_t>(materialIndex)];overridden=true;}
        if(!material)continue;
        // W03：蒙皮部件按 80 字节布局绘制；调色板或蒙皮 PSO 缺失时不画（绝不按参考布局误读）。
        if(mesh.skinned&&(skinPalette.empty()||!skinPipeline||!skinJointCount))continue;
        const auto indices=LevelIndices(mesh,lodLevel);
        const bool userUnlit=material->userSurface&&material->userSurface->metadata.shadingModel==SurfaceShadingModel::Unlit;
        rhi::DrawIndexed draw{*mesh.vertices,*indices,*material->baseColor,
            mesh.skinned?skinPipeline:(material->pipeline?*material->pipeline:pipeline),DrawWorld(model,part,entityWorld),viewProjection,
            material->color,false,(model.fallbackMeshes[part.mesh]&&!overridden)||userUnlit};
        draw.normalTexture=*material->normal;draw.metallicRoughnessTexture=*material->metallicRoughness;
        draw.alphaMode=material->alphaMode;draw.alphaCutoff=material->alphaCutoff;
        draw.layers=material->layers;
        draw.userParams=material->userParams;
        if(!mesh.skinned&&!model.compressed)
            if(const auto bounds=TransformBounds(mesh.localBounds,draw.world))
            {draw.worldBounds=*bounds;draw.hasWorldBounds=true;}
        if(lodLevel==0)draw.geometryClusters=mesh.geometryClusters;
        draw.compressedVertices=mesh.vertexLayoutVersion==rhi::VertexLayoutVersionCompressed;
        draw.sortCenter=TransformPoint(draw.world,mesh.localCenter);draw.hasSortCenter=true;
        draw.indexCount=LevelIndexCount(mesh,lodLevel);
        draw.pbr={camera,directionToLight,radiance,material->metallic,material->roughness,material->normalScale,material->normalMapping,material->doubleSided};
        draw.pbr.lodDither=lodDither;
        draw.hasPreviousWorld=previousWorld!=nullptr;
        if(previousWorld)draw.previousWorld=DrawWorld(model,part,*previousWorld);
        draw.look=look;
        if(mesh.skinned){draw.skinPalette=skinPalette;draw.skinJointCount=skinJointCount;}
        output.push_back(draw);
        if(outUserSurfaces)outUserSurfaces->push_back(material->userSurface);
    }
}
}
