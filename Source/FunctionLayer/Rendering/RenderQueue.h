#pragma once
#include "PlatformLayer/Graphics/Rhi.h"
#include <compare>

namespace se
{
// 只用于不透明队列；完全相同的键保持原先输入顺序。
struct OpaqueSortKey
{
    std::array<std::uint64_t,23> words{};
    auto operator<=>(const OpaqueSortKey&) const=default;
};
OpaqueSortKey BuildOpaqueSortKey(const rhi::DrawIndexed& draw);
void SortOpaqueDraws(std::vector<rhi::DrawIndexed>& draws);
// 右手视空间-z为正视深；远到近，相同深度按完整64位ID，再保留部件输入顺序。
bool SortTransparentDraws(std::vector<rhi::DrawIndexed>& draws,const Mat4& view);
}
