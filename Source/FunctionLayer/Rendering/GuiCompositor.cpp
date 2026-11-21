#include "GuiCompositor.h"
#include "FunctionLayer/Application/FrameData.h"
namespace se
{
namespace
{
bool ValidPageEntry(const FrameData::UiFontPage& page,std::uint32_t index) noexcept
{
    return page.rgba8&&page.width>0&&page.height>0&&page.width<=8192&&page.height<=8192
        &&page.token==GuiCompositor::FontPageToken(index);
}
}
std::optional<std::uint32_t> GuiCompositor::FontPageIndex(std::uint64_t token) noexcept
{
    if(token==FontToken)return 0u;
    if(token>=FontExtraPageBase&&token<FontExtraPageBase+(MaxFontPages-1))
        return static_cast<std::uint32_t>((token-FontExtraPageBase)+1);
    return std::nullopt;
}
bool GuiCompositor::ValidFontPages(const FrameData& f) noexcept
{
    if(f.editorFontPages.empty()||f.editorFontPages.size()>MaxFontPages)return false;
    for(std::size_t i=0;i<f.editorFontPages.size();++i)
        if(!ValidPageEntry(f.editorFontPages[i],static_cast<std::uint32_t>(i)))return false;
    return true;
}
rhi::Result GuiCompositor::Configure(rhi::Device& device,rhi::CommandContext& commands,const FrameData& f,
    rhi::TextureHandle scene,std::uint32_t width,std::uint32_t height,const std::filesystem::path& shaders)
{
    if(!ValidFontPages(f)||!scene||!width||!height
        ||!IsFinite(f.editorGui.framebufferScale.x)||!IsFinite(f.editorGui.framebufferScale.y)
        ||f.editorGui.framebufferScale.x<=0||f.editorGui.framebufferScale.y<=0)return {rhi::Error::InvalidArgument};
    if(!pipeline_)
    {
        rhi::PipelineDescription p{rhi::PipelineKind::Gui,shaders/"Gui.hlsl",rhi::GuiLayoutVersion};
        p.colorFormat=rhi::ColorFormat::Rgba8Srgb;p.blend=rhi::BlendMode::Alpha;
        auto result=device.CreatePipeline(p,pipeline_);if(!result)return result;
    }
    const std::size_t pageCount=f.editorFontPages.size();
    fontPages_.resize(pageCount);
    for(std::size_t i=0;i<pageCount;++i)
    {
        const auto& page=f.editorFontPages[i];
        auto& slot=fontPages_[i];
        const auto pageWidth=static_cast<unsigned>(page.width),pageHeight=static_cast<unsigned>(page.height);
        if(!slot.texture||slot.width!=pageWidth||slot.height!=pageHeight||slot.revision!=page.revision)
        {
            rhi::Texture next;
            auto result=device.CreateTexture({pageWidth,pageHeight,1,1,rhi::TextureFormat::Rgba8,"GuiFont"},
                {page.rgba8,static_cast<std::size_t>(pageWidth)*pageHeight*4},next);
            if(!result)return result;
            slot.texture=std::move(next);slot.width=pageWidth;slot.height=pageHeight;slot.revision=page.revision;
        }
    }
    if(!white_)
    {
        const unsigned char pixel[4]={255,255,255,255};
        rhi::Texture next;auto result=device.CreateTexture({1,1,1,1,rhi::TextureFormat::Rgba8,"GuiWhite"},{pixel,4},next);if(!result)return result;white_=std::move(next);
    }
    for(const FrameData::UiImageSource& image : f.editorUiImages)
    {
        if(image.token==0||image.token==SceneToken||image.token==WhiteToken||FontPageIndex(image.token).has_value()||
            image.width==0||image.height==0||image.width>8192||image.height>8192||!image.rgba8||
            image.bytes!=static_cast<std::size_t>(image.width)*image.height*4)return {rhi::Error::InvalidArgument};
        const auto size=imageSizes_.find(image.token);
        const auto rev=imageRevisions_.find(image.token);
        const bool sameSize=size!=imageSizes_.end()&&size->second.first==image.width&&size->second.second==image.height&&images_.contains(image.token);
        if(!sameSize)
        {
            rhi::Texture next;
            auto result=device.CreateTexture({image.width,image.height,1,1,rhi::TextureFormat::Rgba8,"GuiImage"},
                {image.rgba8,image.bytes},next);
            if(!result)return result;
            images_[image.token]=std::move(next);
            imageSizes_[image.token]={image.width,image.height};
            imageRevisions_[image.token]=image.revision;
        }
        else if(image.revision!=0&&(rev==imageRevisions_.end()||rev->second!=image.revision))
        {
            const rhi::TextureSubresource sub{{image.rgba8,image.bytes},0};
            auto result=device.UploadTextureSubresourcesRange(images_[image.token],0,std::span(&sub,1));
            if(!result)return result;
            imageRevisions_[image.token]=image.revision;
        }
    }
    if(!composite_||width!=width_||height!=height_)
    {rhi::Texture next;auto result=device.CreateRenderTarget({width,height,false,true},next);if(!result)return result;composite_=std::move(next);width_=width;height_=height;}
    std::vector<rhi::TextureHandle> textures;textures.reserve(f.editorGui.commands.size());
    for(const auto& command:f.editorGui.commands)
    {
        if(command.texture==SceneToken)textures.push_back(scene);
        else if(command.texture==WhiteToken)textures.push_back(*white_);
        else if(const auto page=FontPageIndex(command.texture);page&&*page<fontPages_.size())textures.push_back(*fontPages_[*page].texture);
        else if(const auto image=images_.find(command.texture);image!=images_.end())textures.push_back(*image->second);
        else return {rhi::Error::InvalidHandle};
    }
    return commands.ConfigureGui(scene,*composite_,*pipeline_,f.editorGui,textures);
}
}
