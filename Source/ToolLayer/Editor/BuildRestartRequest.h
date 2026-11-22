#pragma once
#include <filesystem>
#include <string>

namespace flare
{
// 写出 EditorWorker 消费的 compile 请求；路径来自 FLARE_EDITOR_RESTART_REQUEST。
bool WriteBuildRestartRequest(const std::filesystem::path& path, std::string& error);
} // namespace flare
