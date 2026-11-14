#pragma once
#include "ComponentMetadata.h"

namespace se
{
// 引擎扩展区固定类型 ID；配置由同一反射表负责编辑、快照和场景持久化。
template<ComponentTypeId Id> class FeatureComponent final : public ComponentBase
{
public:
    static constexpr ComponentTypeId TypeId=Id;
    std::vector<PropertyValue> values;
};
using PhysicsBodyComponent=FeatureComponent<0x464C0001>;
using CharacterSettingsComponent=FeatureComponent<0x464C0002>;
using RenderSettingsComponent=FeatureComponent<0x464C0003>;
using EnvironmentSettingsComponent=FeatureComponent<0x464C0004>;

void AppendEngineFeatureMetadata(std::vector<ComponentDescriptor>& descriptors);
bool IsEngineFeature(ComponentTypeId type) noexcept;
// 未挂载、禁用或缺失时返回空；稳定 ID 最小的有效配置作为场景级设置。
ComponentHandle SceneFeature(const World& world,ComponentTypeId type);
PropertyValue FeatureValue(const World& world,EntityHandle owner,std::string_view key);
}
