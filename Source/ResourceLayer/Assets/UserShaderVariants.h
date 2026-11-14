#pragma once
#include <array>
#include <filesystem>
#include <string>
#include <string_view>
namespace se
{
// 主资产保留旧 PBR 包身份；前向/阴影/环境产物共享同一目录和确定的文件命名。
inline constexpr std::array<std::string_view,4> UserShaderPasses{"pbr","forward","shadowed","environment"};
inline std::filesystem::path UserShaderVariantPath(const std::filesystem::path& primary,std::string_view pass)
{if(pass=="pbr")return primary;auto path=primary;path.replace_extension("."+std::string(pass)+".sebin");return path;}
}
