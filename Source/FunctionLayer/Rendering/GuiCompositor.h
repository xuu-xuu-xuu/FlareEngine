#pragma once
#include "PlatformLayer/Graphics/Rhi.h"
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>
namespace se
{
struct FrameData;
// 只负责 GUI 字体、合成目标和最终 GUI 合成；3D 场景目标由 EditorViewportTarget 拥有。
class GuiCompositor
{
public:
    static constexpr std::uint64_t FontToken=1,SceneToken=2,WhiteToken=3;
    static constexpr std::uint64_t FontExtraPageBase=0x200000000ull;
    static constexpr std::uint32_t MaxFontPages=64;
    [[nodiscard]] static constexpr std::uint64_t FontPageToken(std::uint32_t page) noexcept
    {
        return page==0?FontToken:FontExtraPageBase+static_cast<std::uint64_t>(page-1);
    }
    [[nodiscard]] static std::optional<std::uint32_t> FontPageIndex(std::uint64_t token) noexcept;
    [[nodiscard]] static bool ValidFontPages(const FrameData& frame) noexcept;
    rhi::Result Configure(rhi::Device& device,rhi::CommandContext& commands,const FrameData& frame,
        rhi::TextureHandle scene,std::uint32_t width,std::uint32_t height,const std::filesystem::path& shaders);
    void Clear(){fontPages_.clear();composite_.reset();white_.reset();pipeline_.reset();images_.clear();imageSizes_.clear();imageRevisions_.clear();width_=height_=0;}
private:
    struct FontPageTexture
    {
        rhi::Texture texture;
        std::uint32_t width=0,height=0;
        std::uint64_t revision=0;
    };
    std::vector<FontPageTexture> fontPages_;
    rhi::Texture composite_,white_;
    std::unordered_map<std::uint64_t, rhi::Texture> images_;
    std::unordered_map<std::uint64_t, std::pair<std::uint32_t, std::uint32_t>> imageSizes_;
    std::unordered_map<std::uint64_t, std::uint64_t> imageRevisions_;
    rhi::Pipeline pipeline_;
    std::uint32_t width_=0,height_=0;
};
}
