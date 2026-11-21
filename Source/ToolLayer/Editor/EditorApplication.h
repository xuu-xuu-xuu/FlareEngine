#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include "EditorExternalDocuments.h"

class Renderer;
struct InputState;

namespace se
{
class World;
class UiTree;
class EditorSession;
class EditorWorkspaceHost;
class EditorFloatingWindows;
struct FrameData;
namespace ui::editor
{
class EditorShell;
}

struct EditorApplicationHooks
{
    std::filesystem::path resourceDirectory;
    std::function<bool(EditorSession&, UiTree&)> initialize;
    std::function<void(EditorWorkspaceHost&)> configure;
    std::function<bool(World&, UiTree&)> playStarted;
    std::function<void(World&, UiTree&)> playStopping;
    std::function<bool(World&, UiTree&, double)> update;
    std::function<bool(World&, UiTree&)> singleStep;
    std::function<bool(EditorSession&, double)> frame;
    std::function<void(const InputState&, bool, bool)> input;
    std::function<void(World&, UiTree&, FrameData&)> extract;
    std::function<bool(std::uint64_t, std::string&, std::string&)> scriptQuery;
    std::function<bool(std::uint64_t, const std::string&, std::string&)> scriptApply;
    std::function<void(std::vector<std::string>&)> drainScriptLogs;
    std::function<bool(void* window, const int (&screenRect)[4])> syncPlayCursor;
    EditorExternalDocuments externalDocuments;
    std::function<void(EditorWorkspaceHost&, EditorSession&)> operationFrame;
    std::function<void(EditorSession&, UiTree&)> closing;
    std::function<std::filesystem::path()> chooseAssetImportSource;
    std::function<void(ui::editor::EditorShell&, EditorSession&)> toolkitDrawn;
    std::function<void(EditorFloatingWindows&, const FrameData&)> floatingDrawn;
    bool startPlaying = false;
};

// 通过公共窗口宿主驱动编辑器；EditorUi 仅输出抽象列表，渲染器消费自有 GUI 几何。
int RunEditorApp(Renderer& renderer, const std::string& locale,
    const std::filesystem::path& projectRoot, bool hidden = false, std::uint64_t maximumFrames = 0,
    EditorApplicationHooks* hooks = nullptr);

} // namespace se
