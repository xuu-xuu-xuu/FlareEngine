#pragma once
#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace flare::asset_mip
{
// 沿用现有RGBA字节空间2×2盒式mip；所有BC格式共用相同源mip，旧链逐字节保持。
inline void DownsampleHalf(std::span<const std::uint8_t> source,std::uint32_t width,std::uint32_t height,
    std::vector<std::uint8_t>& next,std::uint32_t& nextWidth,std::uint32_t& nextHeight)
{
    nextWidth=(std::max)(1u,width/2);nextHeight=(std::max)(1u,height/2);
    next.assign(std::size_t(nextWidth)*nextHeight*4,0);
    for(std::uint32_t y=0;y<nextHeight;++y)for(std::uint32_t x=0;x<nextWidth;++x)
    {
        std::uint32_t sums[4]{};
        for(std::uint32_t dy=0;dy<2;++dy)for(std::uint32_t dx=0;dx<2;++dx)
        {
            const std::uint32_t sx=(std::min)(width-1,x*2+dx),sy=(std::min)(height-1,y*2+dy);
            const auto* pixel=source.data()+(std::size_t(sy)*width+sx)*4;
            for(int channel=0;channel<4;++channel)sums[channel]+=pixel[channel];
        }
        auto* pixel=next.data()+(std::size_t(y)*nextWidth+x)*4;
        for(int channel=0;channel<4;++channel)pixel[channel]=static_cast<std::uint8_t>(sums[channel]/4);
    }
}
}
