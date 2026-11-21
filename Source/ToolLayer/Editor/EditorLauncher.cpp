#include "EditorLauncher.h"
#include "LauncherStyle.h"
#include "LauncherIntro.h"
#include "PlatformLayer/Windows/AppBranding.h"
#include "PlatformLayer/Windows/Window.h"
#include "EditorWorker.h"
#include "ProjectWorkspaceSession.h"
#include "CoreLayer/Localization.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include <Windows.h>
#include <commctrl.h>
#include <pathcch.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cwctype>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace se
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr int ControlNewProject = 1001, ControlOpenProject = 1002, ControlOpenSample = 1004, ControlQuit = 1005;
constexpr int ControlRecentList = 1010, ControlRecentOpen = 1011, ControlRecentRemove = 1012, ControlRecentRelocate = 1013;
constexpr int ControlRecentSearch = 1014, ControlRecentExplore = 1015;
constexpr int ControlTemplate = 2001, ControlName = 2002, ControlParent = 2003, ControlBrowse = 2004;
constexpr int ControlPreview = 2005, ControlCreate = 2006, ControlCancel = 2007;
constexpr int ControlTemplateCardBase = 2010;
constexpr int WindowWidthDip = 960, WindowHeightDip = 620;
constexpr int SidebarWidthDip = 236;
constexpr int DialogWidthDip = 720, DialogHeightDip = 500;

struct TemplateChoice
{
    ProjectTemplate type;
    const wchar_t* fallbackLabel;
    const char* descKey;
    const wchar_t* fallbackDesc;
    const char* sampleCopyName;
};
constexpr std::array<TemplateChoice, 4> templateChoices{{
    {ProjectTemplate::BlankScene, L"Blank Scene", "Editor.LauncherTemplateBlankDesc",
     L"Clean minimal scene with a main camera, directional light, and ground plane.", nullptr},
    {ProjectTemplate::Basic3D, L"Basic 3D Sample", "Editor.LauncherTemplateBasicDesc",
     L"Standard 3D starter scene with a gray plane, gray cube, directional light, and main camera.", "Basic3D"},
    {ProjectTemplate::RenderingStudio, L"Rendering Studio", "Editor.LauncherTemplateStudioDesc",
     L"PBR showcase with material spheres, cascaded shadows (CSM), IBL, and ACES tonemapping.", "RenderingStudio"},
    {ProjectTemplate::IndoorGallery, L"Indoor Gallery", "Editor.LauncherTemplateIndoorDesc",
     L"Complex indoor multi-light scene showcasing Forward+ clustered lighting and GI.", "IndoorGallery"}
}};

int ScaleDip(UINT dpi, int dip)
{
    return MulDiv(dip, dpi ? static_cast<int>(dpi) : 96, 96);
}
int ScaleWindow(HWND window, int dip)
{
    const UINT dpi = window ? GetDpiForWindow(window) : 96u;
    return ScaleDip(dpi, dip);
}

std::wstring Wide(std::string_view text)
{
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}
std::string Utf8(const std::wstring& text)
{
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}
bool ContainsCaseInsensitive(std::wstring_view haystack, std::wstring_view needle)
{
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i)
    {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j)
        {
            if (std::towlower(haystack[i + j]) != std::towlower(needle[j]))
            {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}
std::wstring ShortDate(std::string_view isoUtc)
{
    if (isoUtc.size() >= 10 && isoUtc[4] == '-' && isoUtc[7] == '-')
    {
        if (isoUtc.size() >= 16 && (isoUtc[10] == 'T' || isoUtc[10] == ' '))
            return Wide(isoUtc.substr(0, 10)) + L" " + Wide(isoUtc.substr(11, 5));
        return Wide(isoUtc.substr(0, 10));
    }
    return Wide(isoUtc);
}

struct LauncherStrings
{
    Localization localization;
    std::wstring title, subtitle, newProject, openProject, openSample, quit;
    std::wstring recentEmpty, noMatch, searchHint, invalidProject, workerFailed, safeMode, openTitle;
    std::wstring recentTitle, open, showInExplorer, remove, relocate, selectHint, newTitle, templateLabel;
    std::wstring readyBadge, missingBadge, lastOpened;
    std::wstring nameLabel, parentLabel, browse, finalPath, pathReady, pathExists, nameInvalid;
    std::wstring create, cancel, leaseBusy, openFailed, createFailed, preferenceFailed;
    std::wstring Get(std::string_view key, const wchar_t* fallback) const
    {
        const std::string localized = localization.GetText(key);
        if (localized.empty() || localized == key) return fallback;
        return Wide(localized);
    }
    std::wstring Error(std::string_view key) const
    {
        const std::string localized = localization.GetText(key);
        if (localized.empty() || localized == key) return L"[" + Wide(key) + L"]";
        return Wide(localized);
    }
    static LauncherStrings Load(const std::filesystem::path& directory, const std::string& locale)
    {
        LauncherStrings strings;
        std::string error;
        strings.localization.Load(directory / ("Resources/Localization/" + locale + ".txt"), error);
        const auto value = [&](const char* key, const wchar_t* fallback) { return strings.Get(key, fallback); };
        strings.title = value("Editor.LauncherTitle", L"Flare Engine");
        strings.subtitle = value("Editor.LauncherHubSubtitle", L"Project & Template Hub");
        strings.newProject = L"＋ " + value("Editor.LauncherNewProject", L"New Project");
        strings.openProject = value("Editor.LauncherOpenProject", L"Open Project...");
        strings.openSample = value("Editor.LauncherOpenSample", L"Open Sample") + L"  ▾";
        strings.quit = value("Editor.LauncherQuit", L"Quit");
        strings.recentEmpty = value("Editor.LauncherRecentEmpty", L"No recent project yet; create a new project or open a sample.");
        strings.noMatch = value("Editor.LauncherNoMatch", L"No recent projects match your search.");
        strings.searchHint = value("Editor.LauncherSearchHint", L"Filter recent projects by name or path...");
        strings.invalidProject = value("Editor.LauncherInvalidProject", L"Project manifest is missing or invalid:");
        strings.workerFailed = value("Editor.LauncherWorkerFailed", L"Editor worker exited with code:");
        strings.safeMode = value("Editor.LauncherSafeMode", L"Safe mode: session restore is skipped.");
        strings.openTitle = value("Editor.LauncherOpenTitle", L"Select a project folder");
        strings.recentTitle = value("Editor.LauncherRecentTitle", L"Recent projects");
        strings.open = value("Editor.LauncherOpen", L"Open");
        strings.showInExplorer = value("Editor.LauncherShowInExplorer", L"Show in Explorer");
        strings.remove = value("Editor.LauncherRemove", L"Remove record");
        strings.relocate = value("Editor.LauncherRelocate", L"Relocate...");
        strings.selectHint = value("Editor.LauncherSelectHint", L"Double-click a recent project to open it.");
        strings.readyBadge = value("Editor.LauncherReadyBadge", L"Ready");
        strings.missingBadge = value("Editor.LauncherMissingBadge", L"Missing");
        strings.lastOpened = value("Editor.LauncherLastOpened", L"Last opened: ");
        strings.newTitle = value("Editor.LauncherNewTitle", L"New project");
        strings.templateLabel = value("Editor.LauncherTemplate", L"Template");
        strings.nameLabel = value("Editor.LauncherName", L"Project name");
        strings.parentLabel = value("Editor.LauncherParent", L"Parent folder");
        strings.browse = value("Editor.LauncherBrowse", L"Browse...");
        strings.finalPath = value("Editor.LauncherFinalPath", L"Final folder:");
        strings.pathReady = value("Editor.LauncherPathReady", L"Ready — project will be created at:");
        strings.pathExists = value("Editor.LauncherPathExists", L"Note: Target folder already exists; choose a different project name.");
        strings.nameInvalid = value("Editor.LauncherNameInvalid", L"Note: Enter a valid project name.");
        strings.create = value("Editor.LauncherCreate", L"Create");
        strings.cancel = value("Editor.LauncherCancel", L"Cancel");
        strings.leaseBusy = value("Editor.LauncherLeaseBusy", L"Another editor instance is already editing this project.");
        strings.openFailed = value("Editor.LauncherOpenFailed", L"Cannot open project:");
        strings.createFailed = value("Editor.LauncherCreateFailed", L"Cannot create project:");
        strings.preferenceFailed = value("Editor.LauncherPreferenceFailed", L"Preferences could not be saved; this session can continue.");
        return strings;
    }
};

struct LauncherState
{
    LauncherStrings strings;
    std::filesystem::path executable, resourceDirectory, defaultProjectParent, templateRoot, smokeCreateParent;
    std::string locale;
    bool safeMode = false, uiErrors = true;
    ProjectWorkspaceSession workspace;
    LauncherIntro intro;
    std::vector<std::size_t> filteredIndices;
    HWND window = nullptr, brandMark = nullptr, title = nullptr, subtitle = nullptr, note = nullptr;
    HWND newProject = nullptr, openProject = nullptr, openSample = nullptr, quit = nullptr;
    HWND recentLabel = nullptr, recentSearch = nullptr, recentList = nullptr;
    HWND recentOpen = nullptr, recentExplore = nullptr, recentRelocate = nullptr, recentRemove = nullptr;
    HFONT titleFont = nullptr, sectionFont = nullptr, boldFont = nullptr, bodyFont = nullptr;

    void RebuildFonts(UINT dpi)
    {
        if (titleFont) { DeleteObject(titleFont); titleFont = nullptr; }
        if (sectionFont) { DeleteObject(sectionFont); sectionFont = nullptr; }
        if (boldFont) { DeleteObject(boldFont); boldFont = nullptr; }
        if (bodyFont) { DeleteObject(bodyFont); bodyFont = nullptr; }
        titleFont = CreateFontW(-ScaleDip(dpi, 22), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        sectionFont = CreateFontW(-ScaleDip(dpi, 17), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        boldFont = CreateFontW(-ScaleDip(dpi, 14), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        bodyFont = CreateFontW(-ScaleDip(dpi, 13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    }
    void ReleaseFonts()
    {
        if (titleFont) { DeleteObject(titleFont); titleFont = nullptr; }
        if (sectionFont) { DeleteObject(sectionFont); sectionFont = nullptr; }
        if (boldFont) { DeleteObject(boldFont); boldFont = nullptr; }
        if (bodyFont) { DeleteObject(bodyFont); bodyFont = nullptr; }
    }
};

void ShowError(const LauncherState& state, const std::wstring& detail)
{
    std::wcerr << L"[Error] launcher error\n";
    if (state.uiErrors) MessageBoxW(state.window, detail.c_str(), L"Flare Engine", MB_ICONERROR | MB_OK);
}
void ShowError(const LauncherStrings& strings, bool uiErrors, const std::wstring& detail)
{
    (void)strings;
    std::wcerr << L"[Error] launcher error\n";
    if (uiErrors) MessageBoxW(nullptr, detail.c_str(), L"Flare Engine", MB_ICONERROR | MB_OK);
}
void LayoutLauncher(LauncherState& s);
void SetNote(LauncherState& state, const std::wstring& text)
{
    if (!state.note) return;
    SetWindowTextW(state.note, text.c_str());
    const bool wantVisible = !text.empty();
    const bool wasVisible = (GetWindowLongPtrW(state.note, GWL_STYLE) & WS_VISIBLE) != 0;
    if (wantVisible != wasVisible)
    {
        ShowWindow(state.note, wantVisible ? SW_SHOWNA : SW_HIDE);
        LayoutLauncher(state);
    }
}
bool PackageProject(const LauncherState& state, const std::filesystem::path& requested)
{
    std::error_code error;
    return !requested.empty() && std::filesystem::is_regular_file(state.resourceDirectory / L"editor-package.json", error)
        && asset_io::Within(state.resourceDirectory, requested);
}
bool RejectPackageProject(const LauncherState& state, const std::filesystem::path& requested)
{
    if (!PackageProject(state, requested)) return false;
    const auto message = state.strings.Get("Editor.PackageProjectReadOnly",
        L"Projects inside the editor package are read-only. Use Open Sample to create a copy, or create/copy the project outside the package before opening it.");
    std::wcerr << L"[Error] Editor.PackageProjectReadOnly: " << message << L"\n";
    ShowError(state, message);
    return true;
}
std::filesystem::path PickFolder(HWND owner)
{
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.GetAddressOf())))) return {};
    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    if (FAILED(dialog->Show(owner))) return {};
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.GetAddressOf()))) return {};
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) return {};
    std::filesystem::path result(path);
    CoTaskMemFree(path);
    return result;
}

int SelectedRecentIndex(const LauncherState& state)
{
    if (!state.recentList) return -1;
    const int sel = static_cast<int>(SendMessageW(state.recentList, LB_GETCURSEL, 0, 0));
    if (sel < 0 || static_cast<std::size_t>(sel) >= state.filteredIndices.size()) return -1;
    const std::size_t actual = state.filteredIndices[static_cast<std::size_t>(sel)];
    return actual < state.workspace.Recent().size() ? static_cast<int>(actual) : -1;
}

void UpdateRecentActionButtons(LauncherState& state)
{
    const int index = SelectedRecentIndex(state);
    const bool hasSelection = index >= 0;
    const bool available = hasSelection && state.workspace.Recent()[static_cast<std::size_t>(index)].available;
    if (state.recentOpen) EnableWindow(state.recentOpen, hasSelection ? TRUE : FALSE);
    if (state.recentExplore) EnableWindow(state.recentExplore, available ? TRUE : FALSE);
    if (state.recentRelocate) EnableWindow(state.recentRelocate, hasSelection ? TRUE : FALSE);
    if (state.recentRemove) EnableWindow(state.recentRemove, hasSelection ? TRUE : FALSE);
}

void RefreshRecent(LauncherState& state)
{
    if (!state.recentList) return;
    std::string previousSelectedPath;
    if (const int prev = SelectedRecentIndex(state); prev >= 0)
        previousSelectedPath = state.workspace.Recent()[static_cast<std::size_t>(prev)].path;

    wchar_t queryBuffer[256]{};
    if (state.recentSearch) GetWindowTextW(state.recentSearch, queryBuffer, 256);
    const std::wstring_view query(queryBuffer);

    SendMessageW(state.recentList, WM_SETREDRAW, FALSE, 0);
    SendMessageW(state.recentList, LB_RESETCONTENT, 0, 0);
    state.filteredIndices.clear();

    const auto& recent = state.workspace.Recent();
    int selectListRow = -1;
    for (std::size_t i = 0; i < recent.size(); ++i)
    {
        const auto& project = recent[i];
        std::filesystem::path displayPathObj = asset_io::FromUtf8(project.path);
        if (displayPathObj.filename() == L"project.json") displayPathObj = displayPathObj.parent_path();
        const std::wstring wName = Wide(project.name.empty() ? project.path : project.name);
        const std::wstring wPath = displayPathObj.empty() ? Wide(project.path) : displayPathObj.wstring();
        if (!query.empty() && !ContainsCaseInsensitive(wName, query) && !ContainsCaseInsensitive(wPath, query))
            continue;

        const std::wstring wDate = project.lastOpenedUtc.empty()
            ? std::wstring{} : (state.strings.lastOpened + ShortDate(project.lastOpenedUtc));
        const std::wstring& wBadge = project.available ? state.strings.readyBadge : state.strings.missingBadge;
        const std::wstring encoded = wName + L"\t" + wPath + L"\t" + wDate + L"\t" + wBadge + L"\t" +
                                     (project.available ? L"1" : L"0");
        const int added = static_cast<int>(SendMessageW(state.recentList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(encoded.c_str())));
        if (added >= 0)
        {
            if (!previousSelectedPath.empty() && project.path == previousSelectedPath)
                selectListRow = added;
            state.filteredIndices.push_back(i);
        }
    }

    if (selectListRow < 0 && !state.filteredIndices.empty())
        selectListRow = 0;
    if (selectListRow >= 0)
        SendMessageW(state.recentList, LB_SETCURSEL, static_cast<WPARAM>(selectListRow), 0);

    SendMessageW(state.recentList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(state.recentList, nullptr, FALSE);
    UpdateRecentActionButtons(state);

    launcher_style::SetPlaceholder(state.recentList,
        recent.empty() ? state.strings.recentEmpty.c_str() :
        (state.filteredIndices.empty() ? state.strings.noMatch.c_str() : nullptr));
    if (state.safeMode)
        SetNote(state, state.strings.safeMode);
}

bool StartWorker(const LauncherState& state, const std::filesystem::path& projectRoot, const std::wstring& extraArguments, HANDLE& process, DWORD& pid)
{
    if (RejectPackageProject(state, projectRoot)) return false;
    SetEnvironmentVariableW(L"FLARE_EDITOR_SAFE_MODE", state.safeMode ? L"1" : nullptr);
    SetEnvironmentVariableW(L"SELFENGINE_EDITOR_SAFE_MODE", state.safeMode ? L"1" : nullptr);
    std::wstring command = L"\"" + state.executable.wstring() + L"\" --worker --project \"" + projectRoot.wstring() + L"\" --language " + Wide(state.locale);
    if (!extraArguments.empty()) command += L" " + extraArguments;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION information{};
    const auto workingDirectory = std::filesystem::is_regular_file(state.resourceDirectory / "editor-package.json")
        ? projectRoot : state.executable.parent_path();
    if (!CreateProcessW(state.executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, workingDirectory.c_str(), &startup, &information))
    {
        std::wcerr << L"[Error] cannot start editor worker, code=" << GetLastError() << L"\n";
        return false;
    }
    process = information.hProcess;
    pid = information.dwProcessId;
    CloseHandle(information.hThread);
    return true;
}

void WaitWorker(HANDLE process, DWORD pid, DWORD& exitCode)
{
    std::wcout << L"[Info] worker-spawned pid=" << pid << L"\n";
    WaitForSingleObject(process, INFINITE);
    GetExitCodeProcess(process, &exitCode);
    CloseHandle(process);
}

bool OpenProjectFlow(LauncherState& state, const std::filesystem::path& requested, const std::wstring& extraArguments = L"")
{
    if (RejectPackageProject(state, requested)) return false;
    const auto check = ProjectWorkspaceSession::Preflight(requested);
    if (!check.ok)
    {
        ShowError(state, state.strings.openFailed + L"\n" + state.strings.Error(check.errorKey) + L"\n" + Wide(check.detail));
        return false;
    }
    HANDLE process = nullptr;
    DWORD pid = 0;
    if (!StartWorker(state, check.root, extraArguments, process, pid))
    {
        ShowError(state, state.strings.workerFailed);
        return false;
    }
    if (state.smokeCreateParent.empty())
    {
        state.workspace.RecordOpened(check);
        std::string saveError;
        if (!state.workspace.Save(saveError)) SetNote(state, state.strings.preferenceFailed);
    }
    RefreshRecent(state);
    ShowWindow(state.window, SW_HIDE);
    DWORD exitCode = 0;
    WaitWorker(process, pid, exitCode);
    ShowWindow(state.window, SW_SHOW);
    SetForegroundWindow(state.window);
    RefreshRecent(state);
    if (exitCode == 6 || exitCode == 7)
    {
        SetNote(state, exitCode == 6 ? state.strings.leaseBusy : state.strings.Error("Editor.ProjectLockUnavailable"));
        return true;
    }
    if (exitCode != 0)
    {
        ShowError(state, state.strings.workerFailed + L" " + std::to_wstring(exitCode));
        return true;
    }
    return true;
}

const TemplateChoice* PickSample(const LauncherState& state)
{
    std::vector<launcher_style::PopupMenuItem> items;
    items.reserve(templateChoices.size());
    for (size_t index = 0; index < templateChoices.size(); ++index)
    {
        const auto& choice = templateChoices[index];
        if (!choice.sampleCopyName) continue;
        const auto label = state.strings.Get(ProjectTemplateNameKey(choice.type), choice.fallbackLabel);
        items.push_back(launcher_style::PopupMenuItem{
            static_cast<int>(index + 1), label, true, false, false
        });
    }
    if (items.empty()) { ShowError(state, state.strings.openFailed); return nullptr; }
    RECT button{};
    GetWindowRect(state.openSample, &button);
    const int minWidth = static_cast<int>(button.right - button.left);
    const int command = launcher_style::ShowPopupMenu(
        state.window, button.left, button.bottom + ScaleWindow(state.window, 4), minWidth, items, state.bodyFont);
    if (command <= 0 || static_cast<size_t>(command) > templateChoices.size()) return nullptr;
    return &templateChoices[static_cast<size_t>(command) - 1];
}

void OpenSampleCopy(LauncherState& state, const TemplateChoice& choice)
{
    const auto& parent = state.defaultProjectParent;
    if (parent.empty()) { ShowError(state, state.strings.Error("Project.ParentUnavailable")); return; }
    if (RejectPackageProject(state, parent)) return;
    std::error_code error;
    std::filesystem::create_directories(parent, error);
    if (error) { ShowError(state, state.strings.createFailed + L"\n" + state.strings.Error("Project.ParentUnavailable") + L"\n" + parent.wstring()); return; }
    const std::string baseName = choice.sampleCopyName;
    std::string name = baseName;
    for (std::uint64_t number = 2; !std::filesystem::create_directory(parent / name, error); ++number)
    {
        if (error && error != std::errc::file_exists) { ShowError(state, state.strings.createFailed + L"\n" + (parent / name).wstring()); return; }
        name = baseName + " " + std::to_string(number);
    }
    ProjectPreflight copied;
    std::string key, detail;
    if (!ProjectWorkspaceSession::CreateProject(choice.type, name, parent, state.templateRoot, copied, key, detail))
    {
        std::filesystem::remove(parent / name, error);
        ShowError(state, state.strings.createFailed + L"\n" + state.strings.Error(key) + L"\n" + Wide(detail));
        return;
    }
    OpenProjectFlow(state, copied.root);
}

std::wstring SuggestAvailableProjectName(const std::filesystem::path& parent)
{
    const std::wstring base = L"NewProject";
    std::error_code error;
    if (parent.empty() || !std::filesystem::exists(parent / base, error)) return base;
    for (int suffix = 2; suffix < 1000; ++suffix)
    {
        const std::wstring candidate = base + std::to_wstring(suffix);
        if (!std::filesystem::exists(parent / candidate, error)) return candidate;
    }
    return base;
}

struct NewProjectDialogState
{
    LauncherState* launcher = nullptr;
    HWND window = nullptr, combo = nullptr, name = nullptr, parent = nullptr, preview = nullptr, createBtn = nullptr;
    std::array<HWND, 4> cards{};
    int selectedTemplate = 0;
    bool previewWarning = false;
    ProjectPreflight created;
    bool accepted = false;
};

void SelectTemplateCard(NewProjectDialogState& dialog, int index)
{
    if (index < 0 || index >= static_cast<int>(templateChoices.size())) return;
    dialog.selectedTemplate = index;
    if (dialog.combo) SendMessageW(dialog.combo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
    for (int i = 0; i < static_cast<int>(dialog.cards.size()); ++i)
    {
        if (dialog.cards[i])
            launcher_style::SetButtonRole(dialog.cards[i], launcher_style::ButtonRole::Card, i == index);
    }
}

void UpdateNewProjectPreview(NewProjectDialogState& dialog)
{
    if (!dialog.window || !dialog.preview || !dialog.launcher) return;
    wchar_t name[256]{}, parent[1024]{};
    GetWindowTextW(dialog.name, name, 256);
    GetWindowTextW(dialog.parent, parent, 1024);

    std::string detail;
    const std::string utf8Name = Utf8(name);
    const bool validName = ProjectWorkspaceSession::ValidateProjectName(utf8Name, detail);
    std::filesystem::path target(parent);
    if (name[0]) target /= name;

    std::error_code ec;
    const bool exists = validName && !parent[0] ? false : (validName && std::filesystem::exists(target, ec));
    dialog.previewWarning = !validName || !parent[0] || exists;

    std::wstring statusText;
    if (!validName || !parent[0])
        statusText = dialog.launcher->strings.nameInvalid;
    else if (exists)
        statusText = dialog.launcher->strings.pathExists + L"\n" + target.wstring();
    else
        statusText = dialog.launcher->strings.pathReady + L"\n" + target.wstring();

    SetWindowTextW(dialog.preview, statusText.c_str());
    InvalidateRect(dialog.preview, nullptr, TRUE);
    if (dialog.createBtn) EnableWindow(dialog.createBtn, !dialog.previewWarning ? TRUE : FALSE);
}

void CreateNewProjectDialogControls(NewProjectDialogState& dialog, const LauncherState& launcher)
{
    const auto instance = GetModuleHandleW(nullptr);
    const auto s = [&](int dip) { return ScaleWindow(dialog.window, dip); };
    const auto make = [&](const wchar_t* type, const std::wstring& text, DWORD style,
                          int x, int y, int width, int height, int id, HWND& output, HFONT font = nullptr)
    {
        output = CreateWindowExW(0, type, text.c_str(), style | WS_CHILD | WS_VISIBLE,
                                 x, y, width, height, dialog.window,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        if (output)
        {
            SendMessageW(output, WM_SETFONT, reinterpret_cast<WPARAM>(font ? font : launcher.bodyFont), TRUE);
            launcher_style::ThemeControl(output, launcher.boldFont);
        }
        return output != nullptr;
    };

    const int margin = s(22);
    const int contentW = s(DialogWidthDip) - margin * 2;
    HWND label = nullptr;

    make(L"STATIC", launcher.strings.templateLabel, SS_LEFT, margin, s(16), contentW, s(24), -1, label, launcher.sectionFont);

    // 保留隐藏或同步的 ComboBox（ControlTemplate）以兼容自动化脚本与键盘选择
    dialog.combo = CreateWindowExW(0, L"COMBOBOX", L"",
        CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_CHILD,
        0, 0, 1, 1, dialog.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ControlTemplate)), instance, nullptr);
    for (const auto& choice : templateChoices)
    {
        const auto templateName = launcher.strings.Get(ProjectTemplateNameKey(choice.type), choice.fallbackLabel);
        const auto row = SendMessageW(dialog.combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(templateName.c_str()));
        if (row >= 0) SendMessageW(dialog.combo, CB_SETITEMDATA, row, static_cast<LPARAM>(choice.type));
    }

    const int gap = s(12);
    const int cardW = (contentW - gap) / 2;
    const int cardH = s(92);
    for (std::size_t i = 0; i < templateChoices.size(); ++i)
    {
        const auto& choice = templateChoices[i];
        const std::wstring cardTitle = launcher.strings.Get(ProjectTemplateNameKey(choice.type), choice.fallbackLabel);
        const std::wstring cardDesc = launcher.strings.Get(choice.descKey, choice.fallbackDesc);
        const int col = static_cast<int>(i % 2);
        const int row = static_cast<int>(i / 2);
        const int x = margin + col * (cardW + gap);
        const int y = s(46) + row * (cardH + gap);
        make(L"BUTTON", cardTitle + L"\n" + cardDesc, BS_OWNERDRAW | WS_TABSTOP,
             x, y, cardW, cardH, ControlTemplateCardBase + static_cast<int>(i), dialog.cards[i]);
    }

    const int formTop = s(46) + 2 * cardH + gap + s(18);
    const int labelW = s(100);
    const int fieldX = margin + labelW;
    const int fieldW = contentW - labelW;

    make(L"STATIC", launcher.strings.nameLabel, SS_LEFT | SS_CENTERIMAGE, margin, formTop, labelW - s(8), s(28), -1, label, launcher.boldFont);
    make(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, fieldX, formTop, fieldW, s(28), ControlName, dialog.name);

    const int parentTop = formTop + s(40);
    const int browseW = s(96);
    make(L"STATIC", launcher.strings.parentLabel, SS_LEFT | SS_CENTERIMAGE, margin, parentTop, labelW - s(8), s(28), -1, label, launcher.boldFont);
    make(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, fieldX, parentTop, fieldW - browseW - s(8), s(28), ControlParent, dialog.parent);
    HWND browseBtn = nullptr;
    make(L"BUTTON", launcher.strings.browse, BS_OWNERDRAW | WS_TABSTOP, fieldX + fieldW - browseW, parentTop, browseW, s(28), ControlBrowse, browseBtn);

    const int previewTop = parentTop + s(42);
    make(L"STATIC", L"", SS_LEFT | SS_PATHELLIPSIS, margin + s(10), previewTop + s(6), contentW - s(20), s(42), ControlPreview, dialog.preview);

    const int btnW = s(116), btnH = s(34);
    const int btnTop = s(DialogHeightDip) - margin - btnH;
    make(L"BUTTON", launcher.strings.create, BS_OWNERDRAW | WS_TABSTOP, margin + contentW - 2 * btnW - s(10), btnTop, btnW, btnH, ControlCreate, dialog.createBtn);
    launcher_style::SetButtonRole(dialog.createBtn, launcher_style::ButtonRole::Primary);
    HWND cancelBtn = nullptr;
    make(L"BUTTON", launcher.strings.cancel, BS_OWNERDRAW | WS_TABSTOP, margin + contentW - btnW, btnTop, btnW, btnH, ControlCancel, cancelBtn);

    SelectTemplateCard(dialog, 0);
    SetWindowTextW(dialog.parent, launcher.defaultProjectParent.c_str());
    SetWindowTextW(dialog.name, SuggestAvailableProjectName(launcher.defaultProjectParent).c_str());
    UpdateNewProjectPreview(dialog);
}

LRESULT CALLBACK NewProjectProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* dialog = reinterpret_cast<NewProjectDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message)
    {
    case WM_NCCREATE:
    {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(window, message, wParam, lParam);
    }
    case WM_ERASEBKGND:
    {
        RECT rect{};
        GetClientRect(window, &rect);
        launcher_style::Fill(reinterpret_cast<HDC>(wParam), rect, flare::Background);
        return 1;
    }
    case WM_PAINT:
    {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(window, &ps);
        RECT rect{};
        GetClientRect(window, &rect);
        launcher_style::Fill(dc, rect, flare::Background);
        if (dialog && dialog->preview)
        {
            RECT banner{};
            GetWindowRect(dialog->preview, &banner);
            MapWindowPoints(HWND_DESKTOP, window, reinterpret_cast<LPPOINT>(&banner), 2);
            InflateRect(&banner, ScaleWindow(window, 10), ScaleWindow(window, 6));
            launcher_style::FillRound(dc, banner, flare::Surface, ScaleWindow(window, 8));
            launcher_style::FrameRound(dc, banner, dialog->previewWarning ? flare::Warning : flare::Border, ScaleWindow(window, 8));
        }
        EndPaint(window, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    {
        const auto child = reinterpret_cast<HWND>(lParam);
        const bool onSurface = dialog && child == dialog->preview;
        const auto brush = launcher_style::ControlColors(message, reinterpret_cast<HDC>(wParam), false, onSurface);
        if (dialog && child == dialog->preview)
        {
            SetTextColor(reinterpret_cast<HDC>(wParam),
                         flare::Native(dialog->previewWarning ? flare::Gold : flare::Silver));
        }
        return brush;
    }
    case WM_MEASUREITEM:
        reinterpret_cast<MEASUREITEMSTRUCT*>(lParam)->itemHeight = static_cast<UINT>(ScaleWindow(window, 24));
        return TRUE;
    case WM_DRAWITEM:
        if (launcher_style::DrawControl(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam))) return TRUE;
        break;
    case WM_CREATE: return 0;
    case WM_COMMAND:
        if (!dialog || !dialog->launcher) break;
        if (LOWORD(wParam) >= ControlTemplateCardBase &&
            LOWORD(wParam) < ControlTemplateCardBase + static_cast<int>(templateChoices.size()))
        {
            SelectTemplateCard(*dialog, LOWORD(wParam) - ControlTemplateCardBase);
            return 0;
        }
        switch (LOWORD(wParam))
        {
        case ControlBrowse:
        {
            const auto folder = PickFolder(window);
            if (!folder.empty())
            {
                SetWindowTextW(dialog->parent, folder.wstring().c_str());
                UpdateNewProjectPreview(*dialog);
            }
            return 0;
        }
        case IDOK:
        case ControlCreate:
        {
            wchar_t nameBuffer[256]{}, parentBuffer[1024]{};
            GetWindowTextW(dialog->name, nameBuffer, 256);
            GetWindowTextW(dialog->parent, parentBuffer, 1024);
            if (RejectPackageProject(*dialog->launcher, std::filesystem::path(parentBuffer) / nameBuffer)) return 0;
            const auto comboSel = dialog->combo ? static_cast<int>(SendMessageW(dialog->combo, CB_GETCURSEL, 0, 0)) : dialog->selectedTemplate;
            const int templateIndex = (comboSel >= 0 && comboSel < static_cast<int>(templateChoices.size()))
                ? comboSel : dialog->selectedTemplate;
            const auto projectTemplate = templateChoices[static_cast<std::size_t>(templateIndex)].type;
            std::string errorKey, detail;
            if (!ProjectWorkspaceSession::CreateProject(projectTemplate, Utf8(nameBuffer), std::filesystem::path(parentBuffer),
                dialog->launcher->templateRoot, dialog->created, errorKey, detail))
            {
                const auto text = dialog->launcher->strings.createFailed + L"\n" + dialog->launcher->strings.Error(errorKey) + L"\n" + Wide(detail);
                if (dialog->launcher->uiErrors) MessageBoxW(window, text.c_str(), L"Flare Engine", MB_ICONERROR | MB_OK);
                return 0;
            }
            dialog->accepted = true;
            DestroyWindow(window);
            return 0;
        }
        case IDCANCEL:
        case ControlCancel:
            DestroyWindow(window);
            return 0;
        case ControlName:
        case ControlParent:
            if (HIWORD(wParam) == EN_CHANGE) { UpdateNewProjectPreview(*dialog); return 0; }
            break;
        case ControlTemplate:
            if (HIWORD(wParam) == CBN_SELCHANGE)
            {
                const int sel = static_cast<int>(SendMessageW(dialog->combo, CB_GETCURSEL, 0, 0));
                if (sel >= 0) SelectTemplateCard(*dialog, sel);
                UpdateNewProjectPreview(*dialog);
                return 0;
            }
            break;
        default: break;
        }
        break;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) { DestroyWindow(window); return 0; }
        break;
    case WM_CLOSE: DestroyWindow(window); return 0;
    default: break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool RunNewProjectDialog(LauncherState& launcher, bool autoCreate)
{
    const auto instance = GetModuleHandleW(nullptr);
    const wchar_t* className = L"FlareEngineEditorNewProject";
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = NewProjectProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = className;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    NewProjectDialogState dialog;
    dialog.launcher = &launcher;
    const UINT dpi = launcher.window ? GetDpiForWindow(launcher.window) : 96u;
    RECT rectangle{0, 0, ScaleDip(dpi, DialogWidthDip), ScaleDip(dpi, DialogHeightDip)};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectExForDpi(&rectangle, style, FALSE, WS_EX_DLGMODALFRAME, dpi);
    const int winW = rectangle.right - rectangle.left;
    const int winH = rectangle.bottom - rectangle.top;

    int posX = CW_USEDEFAULT, posY = CW_USEDEFAULT;
    if (launcher.window)
    {
        RECT ownerRect{};
        if (GetWindowRect(launcher.window, &ownerRect))
        {
            posX = ownerRect.left + ((ownerRect.right - ownerRect.left) - winW) / 2;
            posY = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - winH) / 2;
        }
    }

    EnableWindow(launcher.window, FALSE);
    dialog.window = CreateWindowExW(WS_EX_DLGMODALFRAME, className, launcher.strings.newTitle.c_str(), style,
        posX, posY, winW, winH, launcher.window, nullptr, instance, &dialog);
    if (!dialog.window) { EnableWindow(launcher.window, TRUE); return false; }

    ApplyAppIcon(dialog.window);
    flare::StyleTitleBar(dialog.window);
    CreateNewProjectDialogControls(dialog, launcher);
    ShowWindow(dialog.window, SW_SHOW);
    SetFocus(dialog.name);
    SendMessageW(dialog.name, EM_SETSEL, 0, -1);

    if (autoCreate)
    {
        SetWindowTextW(dialog.parent, launcher.smokeCreateParent.wstring().c_str());
        SetWindowTextW(dialog.name, (L"SmokeProject" + std::to_wstring(GetTickCount64())).c_str());
        SelectTemplateCard(dialog, 0);
        UpdateNewProjectPreview(dialog);
        PostMessageW(dialog.window, WM_COMMAND, MAKEWPARAM(ControlCreate, BN_CLICKED), 0);
    }

    MSG message{};
    while (IsWindow(dialog.window))
    {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) { DestroyWindow(dialog.window); break; }
            if (message.message == WM_KEYDOWN && message.wParam == VK_RETURN && dialog.createBtn && IsWindowEnabled(dialog.createBtn))
            {
                PostMessageW(dialog.window, WM_COMMAND, MAKEWPARAM(ControlCreate, BN_CLICKED), 0);
                continue;
            }
            if (!IsDialogMessageW(dialog.window, &message)) { TranslateMessage(&message); DispatchMessage(&message); }
        }
        Sleep(10);
    }
    EnableWindow(launcher.window, TRUE);
    SetForegroundWindow(launcher.window);
    if (!dialog.accepted) return !autoCreate;
    return OpenProjectFlow(launcher, dialog.created.root, autoCreate ? L"--hidden --frames 20" : L"");
}

void PaintLauncherBackground(const LauncherState& state, HDC dc)
{
    RECT client{};
    GetClientRect(state.window, &client);
    const int sidebarW = ScaleWindow(state.window, SidebarWidthDip);

    RECT sidebar{0, 0, std::min<int>(sidebarW, client.right), client.bottom};
    RECT mainArea{sidebar.right, 0, client.right, client.bottom};
    launcher_style::Fill(dc, sidebar, flare::Surface);
    launcher_style::Fill(dc, mainArea, flare::Background);

    // 品牌标志背后的日冕柔光，呼应图标里的金色星芒；标志自己画，避免静态控件的方形底色盖住光晕。
    const int markSize = ScaleWindow(state.window, 44);
    const int markX = ScaleWindow(state.window, 20), markY = ScaleWindow(state.window, 24);
    launcher_style::Glow(dc, markX + markSize / 2, markY + markSize / 2, ScaleWindow(state.window, 96), flare::Gold, 46);
    if (launcher_style::GdiplusReady()) DrawFlareMark(dc, markX + markSize / 2, markY + markSize / 2, markSize);
    else DrawIconEx(dc, markX, markY, AppIcon(48), markSize, markSize, 0, nullptr, DI_NORMAL);

    // 搜索框：去掉原生细边框，由父窗口画圆角底与焦点金边。
    if (state.recentSearch)
    {
        RECT field{};
        GetWindowRect(state.recentSearch, &field);
        MapWindowPoints(HWND_DESKTOP, state.window, reinterpret_cast<LPPOINT>(&field), 2);
        InflateRect(&field, ScaleWindow(state.window, 10), ScaleWindow(state.window, 5));
        const int radius = ScaleWindow(state.window, 7);
        launcher_style::FillRound(dc, field, flare::Surface, radius);
        const bool focused = GetFocus() == state.recentSearch;
        launcher_style::FrameRound(dc, field, focused ? flare::Gold : flare::Border, radius, 1.0f, focused ? 200 : 255);
    }

    // 顶部渐隐金线与侧栏右分隔线
    RECT accentTop{0, 0, client.right, ScaleWindow(state.window, 2)};
    launcher_style::FadeLine(dc, accentTop, flare::Gold, 220);
    RECT divider{sidebar.right - 1, 0, sidebar.right, client.bottom};
    launcher_style::Fill(dc, divider, flare::Border);

    const int contentLeft = sidebar.right + ScaleWindow(state.window, 26);
    const int contentRight = client.right - ScaleWindow(state.window, 26);
    if (contentRight > contentLeft)
    {
        const int headerLineY = ScaleWindow(state.window, 62);
        RECT headerDivider{contentLeft, headerLineY, contentRight, headerLineY + 1};
        launcher_style::FadeLine(dc, headerDivider, flare::Silver, 70);
    }
}

void LayoutLauncher(LauncherState& s)
{
    if (!s.window) return;
    RECT r{};
    GetClientRect(s.window, &r);
    const auto sc = [&](int dip) { return ScaleWindow(s.window, dip); };

    const int sidebarW = sc(SidebarWidthDip);
    const int sidePad = sc(20);
    const int sideBtnW = sidebarW - sidePad * 2;
    const int sideBtnH = sc(38);

    const auto move = [](HWND h, int x, int y, int w, int height) {
        if (h) MoveWindow(h, x, y, std::max(1, w), std::max(1, height), TRUE);
    };

    // 左侧品牌与主操作区
    move(s.brandMark, sidePad, sc(24), sc(44), sc(44));
    move(s.title, sidePad + sc(54), sc(22), sidebarW - sidePad - sc(62), sc(28));
    move(s.subtitle, sidePad + sc(54), sc(50), sidebarW - sidePad - sc(62), sc(20));

    const int bottomBtnY = std::max(sc(260), static_cast<int>(r.bottom) - sidePad - sc(34));
    move(s.newProject, sidePad, sc(96), sideBtnW, sc(40));
    move(s.openProject, sidePad, sc(146), sideBtnW, sideBtnH);
    move(s.openSample, sidePad, sc(194), sideBtnW, sideBtnH);
    move(s.quit, sidePad, bottomBtnY, sideBtnW, sc(34));

    // 右侧最近项目主工作区
    const int mainLeft = sidebarW + sc(26);
    const int mainWidth = std::max(sc(320), static_cast<int>(r.right) - mainLeft - sc(26));
    const int searchW = std::min(sc(260), mainWidth / 2);

    move(s.recentLabel, mainLeft, sc(22), mainWidth - searchW - sc(12), sc(28));
    move(s.recentSearch, mainLeft + mainWidth - searchW + sc(10), sc(27), searchW - sc(20), sc(18));

    const int listTop = sc(74);
    const int actionsY = std::max(listTop + sc(140), bottomBtnY);
    const bool noteVisible = s.note && (GetWindowLongPtrW(s.note, GWL_STYLE) & WS_VISIBLE) != 0;
    const int noteReserve = noteVisible ? sc(28) : 0;
    move(s.recentList, mainLeft, listTop, mainWidth, actionsY - listTop - sc(14) - noteReserve);
    if (s.note)
        move(s.note, mainLeft, actionsY - sc(28), mainWidth, sc(22));

    const int btnH = sc(34), gap = sc(8);
    move(s.recentOpen, mainLeft, actionsY, sc(104), btnH);
    move(s.recentExplore, mainLeft + sc(104) + gap, actionsY, sc(128), btnH);
    move(s.recentRelocate, mainLeft + sc(232) + 2 * gap, actionsY, sc(116), btnH);
    move(s.recentRemove, mainLeft + sc(348) + 3 * gap, actionsY, sc(112), btnH);

    InvalidateRect(s.window, nullptr, FALSE);
}

void ShowSelectedInExplorer(const LauncherState& state)
{
    const int index = SelectedRecentIndex(state);
    if (index < 0) return;
    std::filesystem::path target = asset_io::FromUtf8(state.workspace.Recent()[static_cast<std::size_t>(index)].path);
    std::error_code ec;
    if (std::filesystem::is_regular_file(target, ec)) target = target.parent_path();
    if (std::filesystem::is_directory(target, ec))
        ShellExecuteW(state.window, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void ShowRecentContextMenu(LauncherState& state, int screenX, int screenY)
{
    if (!state.recentList) return;
    POINT clientPt{screenX, screenY};
    if (screenX == -1 && screenY == -1)
    {
        const int cur = static_cast<int>(SendMessageW(state.recentList, LB_GETCURSEL, 0, 0));
        if (cur < 0) return;
        RECT itemRect{};
        SendMessageW(state.recentList, LB_GETITEMRECT, static_cast<WPARAM>(cur), reinterpret_cast<LPARAM>(&itemRect));
        clientPt = {itemRect.left + 24, itemRect.bottom};
        ClientToScreen(state.recentList, &clientPt);
    }
    else
    {
        POINT listPt = clientPt;
        ScreenToClient(state.recentList, &listPt);
        const LRESULT hit = SendMessageW(state.recentList, LB_ITEMFROMPOINT, 0, MAKELPARAM(listPt.x, listPt.y));
        if (HIWORD(hit) == 0)
        {
            SendMessageW(state.recentList, LB_SETCURSEL, LOWORD(hit), 0);
            UpdateRecentActionButtons(state);
        }
    }

    const int index = SelectedRecentIndex(state);
    if (index < 0) return;
    const bool available = state.workspace.Recent()[static_cast<std::size_t>(index)].available;

    const std::vector<launcher_style::PopupMenuItem> items{
        {ControlRecentOpen, state.strings.open, true, false, false},
        {ControlRecentExplore, state.strings.showInExplorer, available, false, false},
        {0, L"", false, true, false},
        {ControlRecentRelocate, state.strings.relocate, true, false, false},
        {ControlRecentRemove, state.strings.remove, true, false, true},
    };
    const int cmd = launcher_style::ShowPopupMenu(
        state.window, clientPt.x, clientPt.y, ScaleWindow(state.window, 168), items, state.bodyFont);
    if (cmd > 0) PostMessageW(state.window, WM_COMMAND, MAKEWPARAM(cmd, BN_CLICKED), 0);
}

LRESULT CALLBACK LauncherProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* state = reinterpret_cast<LauncherState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message)
    {
    case WM_NCCREATE:
    {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(window, message, wParam, lParam);
    }
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = {
            ScaleWindow(window, 760), ScaleWindow(window, 480)
        };
        return 0;
    case WM_DPICHANGED:
        if (state)
        {
            const UINT dpi = HIWORD(wParam);
            state->RebuildFonts(dpi);
            if (state->recentList) SendMessageW(state->recentList, LB_SETITEMHEIGHT, 0, ScaleDip(dpi, 58));
            for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
            {
                HFONT f = state->bodyFont;
                if (child == state->title) f = state->titleFont;
                else if (child == state->recentLabel) f = state->sectionFont;
                SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(f), FALSE);
                launcher_style::ThemeControl(child, state->boldFont);
            }
            if (lParam)
            {
                const auto& b = *reinterpret_cast<const RECT*>(lParam);
                SetWindowPos(window, nullptr, b.left, b.top, b.right - b.left, b.bottom - b.top, SWP_NOZORDER | SWP_NOACTIVATE);
            }
            LayoutLauncher(*state);
        }
        return 0;
    case WM_TIMER:
        if (state && state->intro.Active() && wParam == LauncherIntro::Timer) { state->intro.Tick(window); return 0; }
        break;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_KEYDOWN:
        if (state && state->intro.Active())
        {
            state->intro.Finish(window);
            return 0;
        }
        if (message == WM_KEYDOWN && wParam == VK_RETURN && state && GetFocus() == state->recentList)
        {
            PostMessageW(window, WM_COMMAND, MAKEWPARAM(ControlRecentOpen, BN_CLICKED), 0);
            return 0;
        }
        break;
    case WM_PRINTCLIENT:
        if (state && state->intro.Active()) { state->intro.Paint(reinterpret_cast<HDC>(wParam)); return 0; }
        if (state) { PaintLauncherBackground(*state, reinterpret_cast<HDC>(wParam)); return 0; }
        break;
    case WM_PAINT:
        if (state && state->intro.Active())
        {
            PAINTSTRUCT ps{};
            const auto dc = BeginPaint(window, &ps);
            state->intro.Paint(dc);
            EndPaint(window, &ps);
            return 0;
        }
        if (state)
        {
            PAINTSTRUCT ps{};
            const auto dc = BeginPaint(window, &ps);
            RECT client{};
            GetClientRect(window, &client);
            const int w = client.right - client.left, h = client.bottom - client.top;
            const HDC memDc = (w > 0 && h > 0) ? CreateCompatibleDC(dc) : nullptr;
            const HBITMAP memBmp = memDc ? CreateCompatibleBitmap(dc, w, h) : nullptr;
            if (memDc && memBmp)
            {
                const auto oldBmp = SelectObject(memDc, memBmp);
                PaintLauncherBackground(*state, memDc);
                BitBlt(dc, 0, 0, w, h, memDc, 0, 0, SRCCOPY);
                SelectObject(memDc, oldBmp);
            }
            else
            {
                PaintLauncherBackground(*state, dc);
            }
            if (memBmp) DeleteObject(memBmp);
            if (memDc) DeleteDC(memDc);
            EndPaint(window, &ps);
            return 0;
        }
        break;
    case WM_SIZE:
        if (state && state->intro.Active() && !state->intro.Fits(LOWORD(lParam), HIWORD(lParam)))
            state->intro.Finish(window);
        if (state && state->note && wParam != SIZE_MINIMIZED) LayoutLauncher(*state);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    {
        const auto child = reinterpret_cast<HWND>(lParam);
        const bool onSidebar = state && (child == state->title || child == state->subtitle || child == state->brandMark);
        const bool muted = state && (child == state->note || child == state->subtitle);
        const auto brush = launcher_style::ControlColors(message, reinterpret_cast<HDC>(wParam), muted, onSidebar);
        if (state && (child == state->title || child == state->recentLabel))
            SetTextColor(reinterpret_cast<HDC>(wParam), flare::Native(flare::Heading));
        // 最近项目列表与主区同底，卡片自己承担层次。
        if (state && child == state->recentList)
        {
            SetBkColor(reinterpret_cast<HDC>(wParam), flare::Native(flare::Background));
            SetDCBrushColor(reinterpret_cast<HDC>(wParam), flare::Native(flare::Background));
            return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
        }
        // 品牌标题叠在光晕上：透明底，文字不变所以无需擦除旧字。
        if (state && (child == state->title || child == state->subtitle))
        {
            SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
        }
        return brush;
    }
    case WM_MEASUREITEM:
        reinterpret_cast<MEASUREITEMSTRUCT*>(lParam)->itemHeight = static_cast<UINT>(ScaleWindow(window, 58));
        return TRUE;
    case WM_DRAWITEM:
        if (launcher_style::DrawControl(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam))) return TRUE;
        break;
    case WM_CONTEXTMENU:
        if (state && !state->intro.Active() && reinterpret_cast<HWND>(wParam) == state->recentList)
        {
            ShowRecentContextMenu(*state, static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)));
            return 0;
        }
        break;
    case WM_CREATE: return 0;
    case WM_COMMAND:
        if (!state || state->intro.Active()) return 0;
        switch (LOWORD(wParam))
        {
        case ControlNewProject:
            if (RunNewProjectDialog(*state, false)) RefreshRecent(*state);
            return 0;
        case ControlOpenProject:
        {
            const auto folder = PickFolder(window);
            if (!folder.empty()) OpenProjectFlow(*state, folder);
            return 0;
        }
        case ControlOpenSample:
            if (const auto* choice = PickSample(*state)) OpenSampleCopy(*state, *choice);
            return 0;
        case ControlRecentSearch:
            if (HIWORD(wParam) == EN_CHANGE) RefreshRecent(*state);
            if (HIWORD(wParam) == EN_SETFOCUS || HIWORD(wParam) == EN_KILLFOCUS)
            {
                // 重画父窗口上的圆角焦点边。
                RECT field{};
                GetWindowRect(state->recentSearch, &field);
                MapWindowPoints(HWND_DESKTOP, window, reinterpret_cast<LPPOINT>(&field), 2);
                InflateRect(&field, ScaleWindow(window, 12), ScaleWindow(window, 7));
                RedrawWindow(window, &field, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
            }
            return 0;
        case ControlRecentList:
            if (HIWORD(wParam) == LBN_SELCHANGE)
            {
                UpdateRecentActionButtons(*state);
                return 0;
            }
            if (HIWORD(wParam) != LBN_DBLCLK) break;
            [[fallthrough]];
        case IDOK:
        case ControlRecentOpen:
        {
            if (LOWORD(wParam) == IDOK && GetFocus() == state->recentSearch) return 0;
            const int index = SelectedRecentIndex(*state);
            if (index < 0) return 0;
            OpenProjectFlow(*state, Wide(state->workspace.Recent()[static_cast<size_t>(index)].path));
            return 0;
        }
        case ControlRecentExplore:
            ShowSelectedInExplorer(*state);
            return 0;
        case ControlRecentRemove:
        {
            const int index = SelectedRecentIndex(*state);
            if (index < 0) return 0;
            state->workspace.RemoveRecent(state->workspace.Recent()[static_cast<size_t>(index)].path);
            std::string error;
            state->workspace.Save(error);
            RefreshRecent(*state);
            return 0;
        }
        case ControlRecentRelocate:
        {
            const int index = SelectedRecentIndex(*state);
            if (index < 0) return 0;
            const auto folder = PickFolder(window);
            if (folder.empty()) return 0;
            if (RejectPackageProject(*state, folder)) return 0;
            const auto check = ProjectWorkspaceSession::Preflight(folder);
            if (!check.ok) { ShowError(*state, state->strings.openFailed + L"\n" + state->strings.Error(check.errorKey)); return 0; }
            state->workspace.RemoveRecent(state->workspace.Recent()[static_cast<size_t>(index)].path);
            OpenProjectFlow(*state, folder);
            return 0;
        }
        case ControlQuit: PostMessageW(window, WM_CLOSE, 0, 0); return 0;
        default: break;
        }
        break;
    case WM_CLOSE: DestroyWindow(window); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool CreateControls(LauncherState& state)
{
    const auto instance = GetModuleHandleW(nullptr);
    const auto make = [&](const wchar_t* type, const std::wstring& text, DWORD style,
                          int x, int y, int width, int height, int id, HWND& output, HFONT font = nullptr)
    {
        output = CreateWindowExW(0, type, text.c_str(), style | WS_CHILD | WS_VISIBLE,
                                 x, y, width, height, state.window,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        if (output)
        {
            SendMessageW(output, WM_SETFONT, reinterpret_cast<WPARAM>(font ? font : state.bodyFont), TRUE);
            launcher_style::ThemeControl(output, state.boldFont);
        }
        return output != nullptr;
    };
    const auto button = [&](const std::wstring& text, int id, bool enabled, HWND& output,
                            launcher_style::ButtonRole role = launcher_style::ButtonRole::Normal)
    {
        if (!make(L"BUTTON", text, BS_OWNERDRAW | WS_TABSTOP, 0, 0, 80, 32, id, output)) return false;
        launcher_style::SetButtonRole(output, role);
        EnableWindow(output, enabled ? TRUE : FALSE);
        return true;
    };

    const int iconSize = ScaleWindow(state.window, 44);
    if (!make(L"STATIC", L"", SS_ICON | SS_REALSIZEIMAGE, 20, 24, iconSize, iconSize, -1, state.brandMark)) return false;
    SendMessageW(state.brandMark, STM_SETICON, reinterpret_cast<WPARAM>(AppIcon(iconSize >= 48 ? 48 : 32)), 0);
    ShowWindow(state.brandMark, SW_HIDE); // 由 PaintLauncherBackground 连同光晕一起绘制。

    if (!make(L"STATIC", state.strings.title, SS_LEFT | SS_NOPREFIX, 0, 0, 160, 28, -1, state.title, state.titleFont)) return false;
    if (!make(L"STATIC", state.strings.subtitle, SS_LEFT | SS_NOPREFIX, 0, 0, 160, 20, -1, state.subtitle, state.bodyFont)) return false;

    if (!button(state.strings.newProject, ControlNewProject, true, state.newProject, launcher_style::ButtonRole::Primary)) return false;
    if (!button(state.strings.openProject, ControlOpenProject, true, state.openProject)) return false;
    if (!button(state.strings.openSample, ControlOpenSample, true, state.openSample)) return false;
    if (!button(state.strings.quit, ControlQuit, true, state.quit)) return false;

    if (!make(L"STATIC", state.strings.recentTitle, SS_LEFT | SS_CENTERIMAGE, 0, 0, 220, 28, -1, state.recentLabel, state.sectionFont)) return false;
    if (!make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 220, 28, ControlRecentSearch, state.recentSearch)) return false;
    launcher_style::SetPlaceholder(state.recentSearch, state.strings.searchHint.c_str());

    if (!make(L"LISTBOX", L"", WS_VSCROLL | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_TABSTOP,
              0, 0, 600, 320, ControlRecentList, state.recentList)) return false;
    SendMessageW(state.recentList, LB_SETITEMHEIGHT, 0, ScaleWindow(state.window, 58));

    if (!button(state.strings.open, ControlRecentOpen, false, state.recentOpen, launcher_style::ButtonRole::Primary)) return false;
    if (!button(state.strings.showInExplorer, ControlRecentExplore, false, state.recentExplore)) return false;
    if (!button(state.strings.relocate, ControlRecentRelocate, false, state.recentRelocate)) return false;
    if (!button(state.strings.remove, ControlRecentRemove, false, state.recentRemove, launcher_style::ButtonRole::Danger)) return false;

    if (!make(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 0, 0, 600, 22, -1, state.note)) return false;
    ShowWindow(state.note, SW_HIDE);

    RefreshRecent(state);
    LayoutLauncher(state);
    return true;
}

bool CaptureWindow(HWND window, const std::filesystem::path& path)
{
    RECT client{};
    if (!GetClientRect(window, &client)) return false;
    const int width = client.right - client.left, height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return false;
    const HDC windowDc = GetDC(window);
    if (!windowDc) return false;
    const HDC memoryDc = CreateCompatibleDC(windowDc);
    const HBITMAP bitmap = CreateCompatibleBitmap(windowDc, width, height);
    const HGDIOBJ previous = SelectObject(memoryDc, bitmap);
    const BOOL captured = PrintWindow(window, memoryDc, PW_CLIENTONLY);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    std::vector<std::uint8_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    const int rows = GetDIBits(windowDc, bitmap, 0, static_cast<UINT>(height), pixels.data(), &info, DIB_RGB_COLORS);
    SelectObject(memoryDc, previous);
    DeleteObject(bitmap);
    DeleteDC(memoryDc);
    ReleaseDC(window, windowDc);
    if (!captured || rows != height) return false;
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER header = info.bmiHeader;
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    file.bfSize = file.bfOffBits + static_cast<DWORD>(pixels.size());
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(&file), sizeof(file));
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    return bool(output);
}

int RunLauncherWindow(LauncherState& state, const EditorLaunchOptions& options, std::uint64_t smokeMilliseconds, const std::filesystem::path& smokeProject)
{
    EnableEngineDpiAwareness();
    const auto instance = GetModuleHandleW(nullptr);
    const wchar_t* className = L"FlareEngineEditorLauncher";
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = LauncherProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = className;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 5;

    const HDC screenDc = GetDC(nullptr);
    const UINT screenDpi = screenDc ? static_cast<UINT>(GetDeviceCaps(screenDc, LOGPIXELSX)) : 96u;
    if (screenDc) ReleaseDC(nullptr, screenDc);

    RECT rectangle{0, 0, ScaleDip(screenDpi, WindowWidthDip), ScaleDip(screenDpi, WindowHeightDip)};
    AdjustWindowRectExForDpi(&rectangle, WS_OVERLAPPEDWINDOW, FALSE, 0, screenDpi);
    const int winW = rectangle.right - rectangle.left;
    const int winH = rectangle.bottom - rectangle.top;

    int posX = CW_USEDEFAULT, posY = CW_USEDEFAULT;
    RECT workArea{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0))
    {
        posX = workArea.left + std::max(0L, ((workArea.right - workArea.left) - winW) / 2);
        posY = workArea.top + std::max(0L, ((workArea.bottom - workArea.top) - winH) / 2);
    }

    state.window = CreateWindowExW(0, className, state.strings.title.c_str(), WS_OVERLAPPEDWINDOW,
        posX, posY, winW, winH, nullptr, nullptr, instance, &state);
    if (!state.window) return 5;

    ApplyAppIcon(state.window);
    flare::StyleTitleBar(state.window);
    state.RebuildFonts(GetDpiForWindow(state.window));
    if (!CreateControls(state)) { state.ReleaseFonts(); DestroyWindow(state.window); return 5; }
    if (state.safeMode) SetNote(state, state.strings.safeMode);
    if (options.captureLauncher.empty() && !smokeMilliseconds && smokeProject.empty() && options.smokeCreateParent.empty())
        state.intro.Start(state.window);

    ShowWindow(state.window, SW_SHOW);
    UpdateWindow(state.window);

    FILETIME processCreated{}, exited{}, kernel{}, user{}, now{};
    if (GetProcessTimes(GetCurrentProcess(), &processCreated, &exited, &kernel, &user))
    {
        GetSystemTimeAsFileTime(&now);
        const auto ticks = [](FILETIME value) { return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime; };
        if (ticks(now) >= ticks(processCreated)) std::wcout << L"[Info] launcher-interactive-ms=" << (ticks(now) - ticks(processCreated)) / 10000 << L"\n";
    }
    if (!options.captureLauncher.empty())
    {
        MSG paint{};
        for (int index = 0; index < 20; ++index)
        {
            while (PeekMessageW(&paint, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&paint); DispatchMessageW(&paint); }
            Sleep(10);
        }
        if (CaptureWindow(state.window, options.captureLauncher)) std::wcout << L"[Info] launcher-capture=" << options.captureLauncher.wstring() << L"\n";
        else std::wcerr << L"[Error] launcher capture failed\n";
    }
    std::wcout << L"[Info] launcher-ready buttons=8 recent=" << state.workspace.Recent().size() << L"\n";

    int exitCode = 0;
    std::uint64_t loopMilliseconds = smokeMilliseconds;
    if (!options.smokeCreateParent.empty())
    {
        ShowWindow(state.window, SW_HIDE);
        const bool created = RunNewProjectDialog(state, true);
        std::wcout << L"[Info] smoke-create=" << (created ? 1u : 0u) << L"\n";
        ShowWindow(state.window, SW_SHOW);
        if (!created) exitCode = 4;
        loopMilliseconds = 100;
    }
    if (!smokeProject.empty())
    {
        const auto check = RejectPackageProject(state, smokeProject) ? ProjectPreflight{} : ProjectWorkspaceSession::Preflight(smokeProject);
        if (!check.ok) { std::wcerr << L"[Error] smoke project invalid\n"; exitCode = 3; }
        else
        {
            ShowWindow(state.window, SW_HIDE);
            HANDLE process = nullptr;
            DWORD pid = 0;
            DWORD workerExit = 0;
            const bool started = StartWorker(state, check.root, L"--hidden --frames 20", process, pid);
            if (started) WaitWorker(process, pid, workerExit);
            std::wcout << L"[Info] smoke-worker-exit=" << (started ? workerExit : 1u) << L"\n";
            ShowWindow(state.window, SW_SHOW);
            if (!started || workerExit != 0) exitCode = 4;
        }
    }

    const DWORD start = GetTickCount();
    MSG message{};
    while (true)
    {
        if (loopMilliseconds)
        {
            if (GetTickCount() - start >= loopMilliseconds) break;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                if (message.message == WM_QUIT) { exitCode = 0; goto finished; }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            Sleep(10);
        }
        else
        {
            if (GetMessageW(&message, nullptr, 0, 0) <= 0) break;
            if (state.intro.Active() || !IsDialogMessageW(state.window, &message))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
    }
finished:
    if (IsWindow(state.window)) DestroyWindow(state.window);
    state.ReleaseFonts();
    std::wcout << L"[Info] launcher-exit=" << exitCode << L"\n";
    return exitCode;
}

bool ParseUnsigned(const std::wstring& value, std::uint64_t maximum, std::uint64_t& output)
{
    if (value.empty() || value.find_first_not_of(L"0123456789") != std::wstring::npos) return false;
    try
    {
        std::size_t consumed = 0;
        output = std::stoull(value, &consumed);
        return consumed == value.size() && output <= maximum;
    }
    catch (...) { return false; }
}
} // namespace

bool ParseEditorLaunchOptions(int argc, wchar_t** argv, EditorLaunchOptions& options, std::string& error)
{
    for (int index = 1; index < argc; ++index)
    {
        const std::wstring argument = argv[index];
        const auto next = [&](std::wstring& output) -> bool { if (index + 1 >= argc) return false; output = argv[++index]; return true; };
        std::wstring value;
        if (argument == L"--project") { if (!next(value)) { error = "--project requires a value"; return false; } options.project = value; }
        else if (argument == L"--language")
        {
            if (!next(value)) { error = "--language requires a value"; return false; }
            if (value == L"zh-CN") options.locale = "zh-CN";
            else if (value == L"en-US") options.locale = "en-US";
            else { error = "--language accepts zh-CN or en-US"; return false; }
        }
        else if (argument == L"--editor-ui")
        {
            if (!next(value)) { error = "--editor-ui requires toolkit"; return false; }
            if (value == L"toolkit") options.editorUi = "toolkit";
            else { error = "--editor-ui only accepts toolkit"; return false; }
        }
        else if (argument.rfind(L"--editor-ui=", 0) == 0)
        {
            const std::wstring inlineValue = argument.substr(12);
            if (inlineValue == L"toolkit") options.editorUi = "toolkit";
            else { error = "--editor-ui only accepts toolkit"; return false; }
        }
        else if (argument == L"--safe-mode") options.safeMode = true;
        else if (argument == L"--worker") options.worker = true;
        else if (argument == L"--hidden") options.hidden = true;
        else if (argument == L"--demo") options.demo = true;
        else if (argument == L"--ui-errors") options.uiErrors = true;
        else if (argument == L"--no-ui-errors") options.uiErrors = false;
        else if (argument == L"--scene") { if (!next(value)) { error = "--scene requires a value"; return false; } options.scene = value; }
        else if (argument == L"--capture") { if (!next(value)) { error = "--capture requires a value"; return false; } options.capture = value; }
        else if (argument == L"--capture-launcher") { if (!next(value)) { error = "--capture-launcher requires a value"; return false; } options.captureLauncher = value; }
        else if (argument == L"--frames")
        {
            if (!next(value) || !ParseUnsigned(value, 1'000'000, options.frames) || options.frames == 0) { error = "--frames requires a positive count"; return false; }
        }
        else if (argument == L"--smoke-launcher")
        {
            if (!next(value) || !ParseUnsigned(value, 60'000, options.smokeLauncherMs)) { error = "--smoke-launcher requires milliseconds"; return false; }
        }
        else if (argument == L"--smoke-open") { if (!next(value)) { error = "--smoke-open requires a project path"; return false; } options.smokeOpenProject = value; }
        else if (argument == L"--smoke-create") { if (!next(value)) { error = "--smoke-create requires a parent path"; return false; } options.smokeCreateParent = value; }
        else if (argument == L"--smoke-export") { if (!next(value)) { error = "--smoke-export requires a name"; return false; } options.smokeExportName = Utf8(value); }
        else if (argument == L"--smoke-report") { if (!next(value)) { error = "--smoke-report requires a file path"; return false; } options.smokeExportReport = value; }
        else if (argument == L"--smoke-playback") options.smokePlayback = true;
        else if (argument == L"--smoke-workflow") { if (!next(value)) { error = "--smoke-workflow requires a file path"; return false; } options.smokeWorkflow = value; }
        else if (argument == L"--smoke-cancel-ms")
        {
            if (!next(value) || !ParseUnsigned(value, 600'000, options.smokeCancelMilliseconds)) { error = "--smoke-cancel-ms requires milliseconds"; return false; }
        }
        else { error = "unknown argument"; return false; }
    }
    if (options.worker && options.project.empty()) { error = "--worker requires --project"; return false; }
    if (options.hidden && options.frames == 0 && options.smokeExportReport.empty() && options.smokeWorkflow.empty()) { error = "--hidden requires --frames"; return false; }
    if (!options.smokeExportName.empty() && options.smokeExportReport.empty()) { error = "--smoke-export requires --smoke-report"; return false; }
    if (!options.smokeExportReport.empty() && options.smokeExportName.empty() && !options.smokePlayback) { error = "--smoke-report requires --smoke-export or --smoke-playback"; return false; }
    if (!options.smokeExportReport.empty() && !options.worker) { error = "--smoke-export requires --worker"; return false; }
    if (options.smokeCancelMilliseconds > 0 && options.smokeExportReport.empty()) { error = "--smoke-cancel-ms requires --smoke-export"; return false; }
    if (options.smokePlayback && options.smokeExportReport.empty()) { error = "--smoke-playback requires --smoke-report"; return false; }
    if (!options.smokeWorkflow.empty() && (!options.worker || options.frames || options.demo || !options.smokeExportReport.empty() || options.smokePlayback))
    {
        error = "--smoke-workflow requires --worker and cannot be combined with other smoke/frame/demo modes";
        return false;
    }
    if (options.demo && !options.scene.empty()) { error = "--demo and --scene cannot be combined"; return false; }
    return true;
}

int RunEditorLauncher(const EditorLaunchOptions& options)
{
    const auto executable = GetExecutableDirectory().wstring();
    if (executable.empty()) { ShowError(LauncherStrings::Load({}, options.locale), options.uiErrors, L"cannot resolve executable directory"); return 5; }
    LauncherState state;
    wchar_t executableFile[32768]{};
    const DWORD executableLength = GetModuleFileNameW(nullptr, executableFile, static_cast<DWORD>(std::size(executableFile)));
    if (!executableLength || executableLength >= std::size(executableFile)) return 5;
    state.executable = executableFile;
    if (SUCCEEDED(PathCchStripToRoot(executableFile, std::size(executableFile))))
        state.defaultProjectParent = std::filesystem::path(executableFile) / L"Flare_Project";
    state.resourceDirectory = std::filesystem::path(executable);
    state.locale = options.locale;
    state.safeMode = options.safeMode;
    state.uiErrors = options.uiErrors;
    state.strings = LauncherStrings::Load(state.resourceDirectory, options.locale);
    state.templateRoot = state.resourceDirectory / L"Resources/Templates";
    state.smokeCreateParent = options.smokeCreateParent;
    const auto engineRoot = ProjectWorkspaceSession::EngineRootFromExecutable();
    std::string loadError;
    state.workspace.Load(ProjectWorkspaceSession::UserDataDirectory(engineRoot), loadError);
    if (!options.project.empty())
    {
        if (RejectPackageProject(state, options.project)) return 3;
        const auto check = ProjectWorkspaceSession::Preflight(options.project);
        if (!check.ok)
        {
            ShowError(state.strings, options.uiErrors, state.strings.openFailed + L"\n" + state.strings.Error(check.errorKey) + L"\n" + Wide(check.detail));
            return 3;
        }
        HANDLE process = nullptr;
        DWORD pid = 0;
        DWORD exitCode = 0;
        std::wstring extra;
        if (options.hidden) extra += L"--hidden ";
        if (options.frames) extra += L"--frames " + std::to_wstring(options.frames) + L" ";
        if (!options.scene.empty()) extra += L"--scene \"" + options.scene.wstring() + L"\" ";
        if (!options.capture.empty()) extra += L"--capture \"" + options.capture.wstring() + L"\" ";
        if (options.demo) extra += L"--demo ";
        if (!options.smokeExportName.empty()) extra += L"--smoke-export \"" + Wide(options.smokeExportName) + L"\" --smoke-report \"" + options.smokeExportReport.wstring() + L"\" ";
        if (options.smokeCancelMilliseconds) extra += L"--smoke-cancel-ms " + std::to_wstring(options.smokeCancelMilliseconds) + L" ";
        if (!options.editorUi.empty()) extra += L"--editor-ui=" + Wide(options.editorUi) + L" ";
        if (!StartWorker(state, check.root, extra, process, pid)) { ShowError(state.strings, options.uiErrors, state.strings.workerFailed); return 4; }
        state.workspace.RecordOpened(check);
        std::string saveError;
        state.workspace.Save(saveError);
        WaitWorker(process, pid, exitCode);
        return static_cast<int>(exitCode);
    }
    struct Apartment
    {
        HRESULT value;
        ~Apartment() { if (value == S_OK || value == S_FALSE) CoUninitialize(); }
    } apartment{CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)};
    if (FAILED(apartment.value) && apartment.value != RPC_E_CHANGED_MODE) return 5;
    return RunLauncherWindow(state, options, options.smokeLauncherMs, options.smokeOpenProject);
}
} // namespace se
