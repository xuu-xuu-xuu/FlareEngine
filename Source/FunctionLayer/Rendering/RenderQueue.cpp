#include "RenderQueue.h"
#include <algorithm>
#include <bit>

namespace flare
{
OpaqueSortKey BuildOpaqueSortKey(const rhi::DrawIndexed& draw)
{
    OpaqueSortKey key;size_t index=0;key.words[index++]=(static_cast<std::uint64_t>(draw.subPassIndex)<<32)|static_cast<std::uint64_t>(rhi::PassKind::Opaque);
    const auto handle=[&](const auto& value){key.words[index++]=value.device;key.words[index++]=(std::uint64_t(value.index)<<32)|value.generation;};
    const auto scalar=[&](float value){key.words[index++]=std::bit_cast<std::uint32_t>(value);};
    handle(draw.pipeline);handle(draw.texture);handle(draw.normalTexture);handle(draw.metallicRoughnessTexture);
    scalar(draw.tint.x);scalar(draw.tint.y);scalar(draw.tint.z);scalar(draw.tint.w);
    scalar(draw.pbr.metallic);scalar(draw.pbr.roughness);scalar(draw.pbr.normalScale);
    key.words[index++]=(draw.textureIsData?1u:0u)|(draw.unlit?2u:0u)|(draw.pbr.normalMapping?4u:0u)|(draw.pbr.doubleSided?8u:0u)|(draw.pbr.receivesShadow?16u:0u)|(draw.pbr.cullFront?32u:0u);
    handle(draw.vertices);handle(draw.indices);
    key.words[index++]=static_cast<std::uint64_t>(draw.alphaMode);scalar(draw.alphaCutoff);return key;
}
void SortOpaqueDraws(std::vector<rhi::DrawIndexed>& draws)
{
    struct Item{OpaqueSortKey key;rhi::DrawIndexed draw;};std::vector<Item> items;items.reserve(draws.size());
    for(const auto& draw:draws)items.push_back({BuildOpaqueSortKey(draw),draw});
    std::stable_sort(items.begin(),items.end(),[](const auto& a,const auto& b){return a.key<b.key;});
    for(size_t index=0;index<items.size();++index)draws[index]=std::move(items[index].draw);
}
bool SortTransparentDraws(std::vector<rhi::DrawIndexed>& draws,const Mat4& view)
{
    if(!IsFinite(view))return false;
    struct Item{float depth;std::uint64_t id;rhi::DrawIndexed draw;};std::vector<Item> items;items.reserve(draws.size());
    for(const auto& draw:draws)
    {
        const auto center=draw.hasSortCenter?draw.sortCenter:TransformPoint(draw.world,{});
        const auto position=TransformPoint(view,center);
        if(!IsFinite(position))return false;
        items.push_back({-position.z,draw.stableId,draw});
    }
    std::stable_sort(items.begin(),items.end(),[](const auto& a,const auto& b){return a.depth!=b.depth?a.depth>b.depth:a.id<b.id;});
    for(std::size_t i=0;i<items.size();++i)draws[i]=std::move(items[i].draw);
    return true;
}
}
