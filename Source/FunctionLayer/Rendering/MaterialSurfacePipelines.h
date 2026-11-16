#pragma once
#include "PlatformLayer/Graphics/Rhi.h"
#include <map>
#include <tuple>

namespace se
{
// 与基底灯光/阴影布局正交的材质覆盖PSO缓存。只创建场景真正用到的组合。
class MaterialSurfacePipelineCache
{
public:
    rhi::Pipeline Load(rhi::Device& device,const std::filesystem::path& shaders,rhi::PipelineKind kind,
        MaterialAlphaMode mode,bool skinned=false,bool filtered=false,bool compressed=false,bool normalBc5=false,bool shading=false);
    void Clear() noexcept { pipelines_.clear(); }
    std::size_t Count() const noexcept { return pipelines_.size(); }
private:
    using Key=std::tuple<std::filesystem::path,rhi::PipelineKind,MaterialAlphaMode,bool,bool,bool,bool,bool>;
    std::map<Key,rhi::Pipeline> pipelines_;
};
}
