#pragma once

#include "FunctionLayer/UI/Core/UiDocument.h"
#include "FunctionLayer/UI/Core/UiLayout.h"
#include "FunctionLayer/UI/Core/UiStyle.h"
#include "FunctionLayer/UI/Widgets/UiWidgets.h"
#include "FunctionLayer/UI/Widgets/UiTextEdit.h"
#include "ToolLayer/Editor/EditorUi/EditorChrome.h"
#include "ToolLayer/Editor/EditorUi/EditorCommand.h"
#include "ToolLayer/Editor/EditorUi/EditorCommandRouter.h"
#include "ToolLayer/Editor/EditorUi/EditorDock.h"
#include "ToolLayer/Editor/EditorUi/EditorPanels.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
class Localization;
namespace se::ui::editor
{
enum class EditorPropertyEditPhase { Begin, Commit, Cancel };
struct EditorFormPanel;
struct EditorInspectorDrag;
struct EditorInspectorColor;
struct EditorAssetDragState;

// 编辑器 Toolkit 组合根的 CPU 侧：文档、Chrome、Dock、面板与命令。
// 仍由 EditorApplication/EditorRenderer 提供窗口和 RHI；本类不拥有 GPU 资源。
struct EditorDesignerNode
{
    std::uint64_t id = 0;
    UiRect rect{};
    std::string text;
    UiColor color{};
    bool selected = false;
    bool visible = true;

    bool operator==(const EditorDesignerNode& other) const
    {
        return id == other.id && rect.x == other.rect.x && rect.y == other.rect.y &&
            rect.width == other.rect.width && rect.height == other.rect.height &&
            text == other.text && color == other.color && selected == other.selected &&
            visible == other.visible;
    }
};

class EditorShell
{
public:
    explicit EditorShell(UiTextService* textService = nullptr);
    ~EditorShell();

    EditorShell(const EditorShell&) = delete;
    EditorShell& operator=(const EditorShell&) = delete;

    // 必须在 Initialize 前设置，Chrome/面板标签会在初始化时取当前语言。
    void SetLocalization(const Localization* text) noexcept;
    bool Initialize(float width, float height);
    void SetDpiScale(float scale);
    [[nodiscard]] float DpiScale() const noexcept;
    void Resize(float width, float height);
    void Frame();

    // .editor-ui.json；空路径表示不持久化。Initialize 后设置会立即尝试读取。
    void SetLayoutPath(std::filesystem::path path);
    [[nodiscard]] const std::filesystem::path& LayoutPath() const noexcept;
    bool SaveLayout();
    bool LoadLayout();
    std::uint32_t FloatPanel(EditorPanelId panel,int x,int y,int width=640,int height=480);
    void CloseFloatingWindow(std::uint32_t id);
    void SetFloatingBounds(std::uint32_t id,int x,int y,int width,int height);
    void SetSurfaceSize(std::uint32_t id,float width,float height);
    void SetInputSurface(std::uint32_t id);
    [[nodiscard]] UiElementHandle SurfaceRoot(std::uint32_t id) const;
    [[nodiscard]] UiElementHandle CurrentInputRoot() const {return SurfaceRoot(inputSurface_);}
    [[nodiscard]] std::uint32_t SurfaceForElement(UiElementHandle element) const;
    [[nodiscard]] UiRect SurfaceDockRect(std::uint32_t id) const;
    [[nodiscard]] std::optional<DockAxis> SplitterCursorAt(float x, float y) const;
    [[nodiscard]] EditorPanelId DraggingPanel() const noexcept {return dragTabPanel_;}
    void SetFloatRequestHandler(std::function<void(EditorPanelId)> handler){floatRequest_=std::move(handler);}
    [[nodiscard]] std::string WindowLabel(std::uint32_t id) const;

    void AppendConsoleLine(std::string line);
    void ClearConsole();
    bool ApplyLayoutPreset(std::string_view preset);
    void SetHierarchy(std::vector<EditorHierarchyNode> nodes);
    void SetAssets(std::vector<EditorAssetEntry> entries);
    void SetInspector(std::uint64_t id, std::string label, std::vector<EditorProperty> properties);
    void SetSelectionHandler(std::function<void(std::uint64_t)> handler);
    void SetPropertyHandler(std::function<bool(std::uint64_t, std::string, std::string)> handler);
    void SetPropertyChoices(std::function<std::string(std::uint64_t,EditorProperty&)> handler){propertyChoices_=std::move(handler);}
    void SetPropertyEditHandler(std::function<bool(std::uint64_t, EditorPropertyEditPhase)> handler);
    [[nodiscard]] bool InspectorNumericCursorAt(float x, float y) const;
    [[nodiscard]] bool ColorPickerOpen() const noexcept { return inspectorColor_ != nullptr; }
    [[nodiscard]] UiElementHandle ColorPickerDialog() const noexcept;
    [[nodiscard]] UiElementHandle ColorPickerSquare() const noexcept;
    [[nodiscard]] UiElementHandle ColorPickerHue() const noexcept;
    [[nodiscard]] UiElementHandle ColorPickerChannel(std::size_t index) const noexcept;
    [[nodiscard]] UiElementHandle ColorPickerApplyButton() const noexcept;
    void CancelPropertyInteraction();
    void SetReparentHandler(std::function<bool(std::uint64_t, std::uint64_t)> handler);
    void SetAssetSelectionHandler(std::function<void(std::string)> handler);
    void SetAssetActivateHandler(std::function<void(std::string)> handler) { assetActivateHandler_ = std::move(handler); }
    void SetAssetImportHandler(std::function<void()> handler);
    void SetAssetDragHandler(std::function<void(const std::string&, float, float, bool)> handler);
    void SetAssetDropHandler(std::function<bool(const std::string&, float, float)> handler);
    void SetAssetEntityDropHandler(std::function<bool(const std::string&, std::uint64_t, EditorPanelId)> handler)
    {
        assetEntityDropHandler_ = std::move(handler);
    }
    void SetScriptsDirty(bool dirty) noexcept { scriptsDirty_ = dirty; }
    void SetViewportImage(std::string image);
    void SetGizmo(float x, float y, bool visible);
    void SetGizmoDragHandler(std::function<void(int, float, float)> handler);
    void SetViewportActionHandler(std::function<void(std::string_view)> handler){viewportAction_=std::move(handler);}
    void SetViewportState(int tool,bool local,bool center,bool sky,bool grid,bool gizmos,bool playing,bool paused=false);

    // 状态栏与面板开关；Chrome/菜单/快捷键共用同一命令总线。
    void SetStatus(std::string status);
    void SetDirty(bool dirty);
    void SetSelectionLabel(std::string label);
    void TogglePanel(EditorPanelId panel);
    void FocusPanel(EditorPanelId panel);

    // 通用属性表单（Material / ProjectSettings / BuildExport）：只保存行/回调，
    // 具体读写仍由 EditorApplication 与对应服务负责。
    void SetFormProperties(EditorPanelId panel, std::vector<EditorProperty> properties);
    void SetFormHandlers(EditorPanelId panel, std::function<bool(std::string, std::string)> commit,
                         std::function<bool()> save);
    void SetFormAction(EditorPanelId panel, std::string label, std::function<bool()> action);
    void SetFormActionLabel(EditorPanelId panel, std::string label);
    void SetFormStatus(EditorPanelId panel, std::string status);
    void SetFormPreview(EditorPanelId panel,std::string image,float aspect=1);
    void SetFormPrimaryLabel(EditorPanelId panel,std::string label);
    void SetFormManagedStatus(EditorPanelId panel,bool managed);
    // About/Diagnostics 等只读面板；文本直接写在 content 元素上。
    void SetPanelText(EditorPanelId panel, std::string text);
    // Assets 底部预览文本（材质摘要等）；空字符串恢复“Preview: path”。
    void SetAssetsPreviewText(std::string text);
    // 程序化选择资产（测试/恢复用）；会同步列表、预览与选择回调。
    void SelectAsset(const std::string& path);
    void OpenCreateMenu(EditorPanelId panel, float x, float y, std::uint64_t parent = 0);
    // 资产图片预览；空 image 隐藏。width/height 为源图尺寸（仅用于记录）。
    void SetAssetPreviewImage(std::string image,float width,float height);
    // F2：聚焦 Inspector 的 name 行并进入编辑；无选中/不可编辑返回 false。
    bool BeginRename();
    [[nodiscard]] UiElementHandle FormRow(EditorPanelId panel, std::size_t index) const noexcept;
    [[nodiscard]] UiElementHandle FormSaveButton(EditorPanelId panel) const noexcept;
    [[nodiscard]] UiElementHandle FormActionButton(EditorPanelId panel) const noexcept;
    [[nodiscard]] UiElementHandle FormListContent(EditorPanelId panel) const noexcept;
    [[nodiscard]] UiElementHandle ViewportToolButton(std::size_t index) const noexcept { return index < viewportToolButtons_.size() ? viewportToolButtons_[index] : UiElementHandle{}; }
    [[nodiscard]] UiRect InspectorFieldRect(std::string_view key) const;
    [[nodiscard]] UiElementHandle PanelContent(EditorPanelId panel) const noexcept;
    [[nodiscard]] UiElementHandle ScrollThumb(EditorPanelId panel) const noexcept;
    // 通用确认框（恢复/外部变更等），与关闭保护模态独立。
    void ShowConfirm(std::string title, std::string message, std::string primaryLabel,
                     std::string secondaryLabel, std::string cancelLabel,
                     std::function<void()> onPrimary, std::function<void()> onSecondary,
                     bool primaryEnabled = true);
    [[nodiscard]] bool ConfirmOpen() const noexcept { return confirmOpen_; }
    [[nodiscard]] UiElementHandle ConfirmDialog() const noexcept { return confirmDialog_; }
    [[nodiscard]] UiElementHandle ConfirmPrimaryButton() const noexcept { return confirmPrimary_; }
    [[nodiscard]] UiElementHandle ConfirmSecondaryButton() const noexcept { return confirmSecondary_; }
    [[nodiscard]] UiElementHandle ConfirmCancelButton() const noexcept { return confirmCancel_; }
    [[nodiscard]] UiElementHandle AssetsPreview() const noexcept { return assetsPreview_; }
    [[nodiscard]] UiElementHandle AssetsPreviewImage() const noexcept { return assetsPreviewImageElement_; }
    [[nodiscard]] UiElementHandle FormComboPopup(EditorPanelId panel) const noexcept;
    [[nodiscard]] UiElementHandle FormComboOption(EditorPanelId panel, std::size_t index) const noexcept;

    // UI Designer：节点矩形相对 Designer 内容；手势回调使用窗口逻辑坐标。
    void SetDesignerNodes(std::vector<EditorDesignerNode> nodes);
    void SetDesignerHandlers(std::function<void(float, float)> down,
                             std::function<void(float, float)> move,
                             std::function<void(float, float)> up);
    [[nodiscard]] UiElementHandle UiDesignerContent() const noexcept;
    [[nodiscard]] UiElementHandle DesignerNodeElement(std::size_t index) const noexcept;

    // 关闭保护：脏文档先弹保存/丢弃/取消模态；干净时直接允许关闭。
    bool RequestClose();
    [[nodiscard]] bool CloseApproved() const noexcept;
    [[nodiscard]] bool CloseModalOpen() const noexcept;
    [[nodiscard]] UiElementHandle CloseModalDialog() const noexcept;
    [[nodiscard]] UiElementHandle CloseModalSaveButton() const noexcept { return modalSave_; }
    [[nodiscard]] UiElementHandle CloseModalDiscardButton() const noexcept { return modalDiscard_; }
    [[nodiscard]] UiElementHandle CloseModalCancelButton() const noexcept { return modalCancel_; }
    [[nodiscard]] bool ConsumeCloseRequest() noexcept;
    [[nodiscard]] bool TextFieldFocused() const noexcept;
    bool RouteShortcut(const UiInputEvent& event, bool textFieldFocused);

    [[nodiscard]] UiDocument& Document() noexcept;
    [[nodiscard]] UiLayoutEngine& Layout() noexcept;
    [[nodiscard]] const UiLayoutEngine& Layout() const noexcept;
    [[nodiscard]] UiDispatcher& Dispatcher() noexcept;
    [[nodiscard]] UiTextService& TextService() noexcept;
    [[nodiscard]] UiListView& ConsoleList() noexcept;
    [[nodiscard]] UiListView& HierarchyList() noexcept;
    [[nodiscard]] UiListView& AssetsList() noexcept;
    [[nodiscard]] UiListView& InspectorList() noexcept;
    [[nodiscard]] EditorDock& Dock() noexcept;
    [[nodiscard]] EditorChrome& Chrome() noexcept;
    [[nodiscard]] EditorCommandBus& Commands() noexcept;
    [[nodiscard]] EditorCommandRouter& Router() noexcept;

    [[nodiscard]] UiElementHandle Root() const noexcept;
    [[nodiscard]] UiElementHandle ConsolePanel() const noexcept;
    [[nodiscard]] EditorPanelId ActivePanel() const noexcept;
    [[nodiscard]] UiElementHandle HierarchyContent() const noexcept;
    [[nodiscard]] UiElementHandle AssetsContent() const noexcept;
    [[nodiscard]] UiElementHandle ViewportPanel() const noexcept;
    [[nodiscard]] UiElementHandle InspectorContent() const noexcept;
    [[nodiscard]] UiElementHandle InspectorEdit() const noexcept;
    [[nodiscard]] UiElementHandle MenuBar() const noexcept;
    [[nodiscard]] UiElementHandle Toolbar() const noexcept;
    [[nodiscard]] UiElementHandle StatusBar() const noexcept;
    [[nodiscard]] UiElementHandle Panel(EditorPanelId panel) const noexcept;
    [[nodiscard]] UiElementHandle TabBar(EditorPanelId panel) const noexcept;
    [[nodiscard]] UiElementHandle TabButton(EditorPanelId panel) const noexcept;
    [[nodiscard]] UiElementHandle DockPreview() const noexcept {return dropHighlight_;}
    [[nodiscard]] UiElementHandle MenuDropdown() const noexcept;
    [[nodiscard]] bool MenuOpen() const noexcept;
    [[nodiscard]] const std::vector<std::string>& ConsoleLines() const noexcept;
    [[nodiscard]] float Width() const noexcept;
    [[nodiscard]] float Height() const noexcept;

private:
    struct PanelSlot
    {
        EditorPanelId id = EditorPanelId::None;
        UiElementHandle panel{};
        UiElementHandle content{};
        EditorPanelId openNear = EditorPanelId::Console;
    };

    struct GroupView
    {
        std::uint32_t surface=0;
        std::vector<EditorPanelId> tabs;
        EditorPanelId active = EditorPanelId::None;
        UiElementHandle tabBar{};
        UiElementHandle focusLine{};
        UiElementHandle previousTab{}, nextTab{};
        std::vector<UiElementHandle> tabButtons;
        std::vector<UiElementHandle> closeButtons;
        std::vector<float> widths;
        UiRect rect{};
    };

    struct SplitHandle
    {
        UiElementHandle line{};
        std::uint32_t surface=0;
        std::vector<std::size_t> path;
        DockAxis axis = DockAxis::Horizontal;
        UiRect rect{};
        float ratio = 0.5f;
        float total = 0.0f;
    };

    using FormPanel = EditorFormPanel;
    using InspectorDrag = EditorInspectorDrag;
    using InspectorColor = EditorInspectorColor;

    static constexpr std::size_t kPanelCount = 14;

    PanelSlot& Slot(EditorPanelId panel);
    const PanelSlot& Slot(EditorPanelId panel) const;

    FormPanel& Form(EditorPanelId panel);
    const FormPanel& Form(EditorPanelId panel) const;
    void BuildPanels();
    void BuildPanelContent(PanelSlot& slot);
    void BuildInspector(PanelSlot& slot);
    void BuildInspectorRow(int index, UiElementHandle element);
    void RefreshInspectorRows();
    void LayoutInspector(float width);
    void ActivateInspectorRow(UiInputEvent& event);
    bool BeginInspectorEdit(std::size_t index);
    void EndInspectorEdit(bool commit);
    void PaintInspectorEdit(float width);
    void ShowInspectorCombo(std::size_t index);
    void ShowInspectorPage(std::shared_ptr<const EditorProperty> field,std::uint64_t target,float x,float y,std::size_t first);
    std::optional<std::size_t> InspectorNumericAt(float x, float y) const;
    void BeginInspectorDrag(std::size_t index, const UiInputEvent& event);
    void UpdateInspectorDrag(const UiInputEvent& event);
    void EndInspectorDrag(bool commit);
    void OpenInspectorColor(std::size_t index);
    void CloseInspectorColor(bool commit);
    void LayoutInspectorColor();
    void RefreshInspectorColor(int editingChannel = -1);
    void MoveInspectorColor(const UiInputEvent& event);
    void ApplyInspectorColorChannel(std::size_t index);
    void BuildFormPanel(PanelSlot& slot);
    void RefreshForm(FormPanel& form);
    void ShowFormCombo(EditorPanelId panel, FormPanel& form, int row);
    void HideFormCombo(FormPanel& form);
    void UpdateFormRects(EditorPanelId panel);
    void BuildDesignerPanel(PanelSlot& slot);
    void RefreshDesignerNodes();
    void BuildChrome();
    void BindRootInputHandlers();
    void RegisterDefaultShortcuts();
    void UpdateTransport();
    void BuildChromeMenus();
    void RefreshStatus();
    void RebuildGroupViews();
    void UpdateDockRects();
    void LayoutDockNode(const DockNode& node, const UiRect& rect, std::vector<std::size_t>& path);
    void RebuildTabBar(GroupView& group);
    void LayoutTabBar(GroupView& group);
    void SetActivePanel(EditorPanelId panel);
    void FocusClickedContent(UiElementHandle target);
    void UpdatePanelContentRects();
    void MarkDockDirty(bool structural, bool save);
    void OpenMenu(const std::string& menu);
    void OpenLayoutMenu(float x, float y);
    void OpenAssetTypeMenu(float x, float y);
    void ShowMenu(UiElementHandle surface, float x, float y, float width,
                  const std::vector<EditorMenuItem>& items, std::uint64_t target = 0);
    void CloseMenu();
    void ShowCloseModal();
    void HideCloseModal();
    void SelectPanel(EditorPanelId panel, bool save);
    void HandleTabDown(EditorPanelId panel, UiInputEvent& event);
    void HandleTabMove(UiInputEvent& event);
    void HandleTabUp(UiInputEvent& event);
    void ApplyTabDrop();
    [[nodiscard]] bool ApplyTabDrop(EditorDock& dock) const;
    void CancelTabDrag();
    void BeginSplitter(float x, float y);
    void DragSplitter(float x, float y);
    void EndSplitter(bool commit);
    [[nodiscard]] const SplitHandle* FindSplitter(float x, float y) const;
    void SaveLayoutIfConfigured();
    [[nodiscard]] std::string Text(std::string_view key, std::string_view fallback) const;
    [[nodiscard]] std::string PanelLabel(EditorPanelId panel) const;
    [[nodiscard]] float MeasuredWidth(const std::string& text, float fontSize) const;
    [[nodiscard]] float TabWidth(const std::string& label) const;
    [[nodiscard]] float MenuWidth(const std::string& label) const;
    [[nodiscard]] float ToolbarWidth(const std::string& label) const;

    const Localization* localization_ = nullptr;
    UiDocument document_;
    UiStubTextService stub_;
    UiTextService* text_ = &stub_;
    UiLayoutEngine layout_;
    UiStyleEngine style_;
    UiDispatcher dispatcher_;
    EditorCommandBus commands_;
    EditorCommandRouter router_;
    EditorChrome chrome_;
    EditorDock dock_;
    struct SurfaceView {std::uint32_t id=0;UiElementHandle root;float width=640,height=440;};
    std::vector<SurfaceView> surfaces_;
    std::uint32_t inputSurface_=0,splitterSurface_=0;
    std::function<void(EditorPanelId)> floatRequest_;
    std::array<PanelSlot, kPanelCount> slots_{};
    std::vector<GroupView> groups_;
    UiElementHandle viewportTools_{};
    UiElementHandle transformTools_{};
    std::vector<UiElementHandle> viewportToolButtons_;
    std::function<void(std::string_view)> viewportAction_;
    std::vector<SplitHandle> splitHandles_;
    std::vector<UiElementHandle> pendingDestroy_;
    std::array<std::unique_ptr<FormPanel>, 5> forms_{};
    std::string assetsPreviewText_;
    std::string assetsPreviewImage_;
    UiElementHandle assetsPreviewImageElement_{};
    float assetsPreviewImageWidth_ = 0.0f, assetsPreviewImageHeight_ = 0.0f;
    std::array<UiElementHandle, kPanelCount> scrollThumbs_{};
    EditorPanelId scrollDragPanel_ = EditorPanelId::None;
    bool scrollDragging_ = false;
    float scrollDragStartY_ = 0.0f;
    float scrollDragStartOffset_ = 0.0f;
    [[nodiscard]] UiListView* ListFor(EditorPanelId panel) const;
    void BuildScrollThumb(EditorPanelId panel);
    void UpdateScrollThumbs();
    UiElementHandle designerCanvas_{};
    UiElementHandle designerPlaceholder_{};
    std::vector<UiElementHandle> designerNodeElements_;
    std::vector<EditorDesignerNode> designerNodes_;
    std::function<void(float, float)> designerDown_;
    std::function<void(float, float)> designerMove_;
    std::function<void(float, float)> designerUp_;
    bool designerDragging_ = false;
    std::unique_ptr<UiListView> console_;
    std::unique_ptr<UiListView> hierarchyList_;
    std::unique_ptr<UiListView> assetsList_;
    std::unique_ptr<UiListView> inspectorList_;
    EditorHierarchyModel hierarchyModel_;
    EditorAssetsModel assetsModel_;
    EditorInspectorModel inspectorModel_;
    struct InspectorRow
    {
        std::string title;
        bool header = false;
        bool closed = false;
        std::vector<std::size_t> fields;
    };
    std::vector<InspectorRow> inspectorRows_;
    std::unordered_map<std::string, bool> inspectorFolds_;
    float inspectorWidth_ = 0.0f;
    std::function<void(std::uint64_t)> selectionHandler_;
    std::function<bool(std::uint64_t, std::string, std::string)> propertyHandler_;
    std::function<std::string(std::uint64_t,EditorProperty&)> propertyChoices_;
    std::function<bool(std::uint64_t, EditorPropertyEditPhase)> propertyEditHandler_;
    std::unique_ptr<InspectorDrag> inspectorDrag_;
    std::unique_ptr<InspectorColor> inspectorColor_;
    std::function<bool(std::uint64_t, std::uint64_t)> reparentHandler_;
    std::function<void(std::string)> assetSelectionHandler_;
    std::function<void(std::string)> assetActivateHandler_;
    std::function<void()> assetImportHandler_;
    std::function<bool(const std::string&, std::uint64_t, EditorPanelId)> assetEntityDropHandler_;
    UiElementHandle assetsTypeElement_{};
    UiElementHandle assetsCreateElement_{};
    UiElementHandle assetsImportElement_{};
    UiElementHandle assetsPreview_{};
    UiElementHandle consoleClearButton_{};
    void CancelAssetDrag();
    std::uint64_t dragHierarchyId_ = 0;
    float hierarchyDragStartX_ = 0.0f, hierarchyDragStartY_ = 0.0f;
    bool draggingHierarchy_ = false;
    int hoveredHierarchyRow_ = -1;
    int hoveredAssetRow_ = -1;
    std::uint64_t lastHierarchyClickId_ = 0;
    std::uint64_t lastHierarchyClickTick_ = 0;
    std::string lastAssetClickPath_;
    std::uint64_t lastAssetClickTick_ = 0;
    std::unique_ptr<EditorAssetDragState> assetDrag_;
    std::unique_ptr<UiTextEdit> inspectorEdit_;
    UiElementHandle inspectorEditElement_{};
    UiElementHandle inspectorEditSelection_{};
    float inspectorEditScroll_ = 0;
    std::unique_ptr<UiTextEdit> assetsFilter_;
    UiElementHandle assetsFilterElement_{};
    UiElementHandle dragGhost_{};
    UiElementHandle dropHighlight_{};
    UiElementHandle tabInsertMarker_{};
    UiElementHandle modalBackdrop_{};
    UiElementHandle modalDialog_{};
    UiElementHandle modalSave_{};
    UiElementHandle modalDiscard_{};
    UiElementHandle modalCancel_{};
    UiElementHandle confirmBackdrop_{}, confirmDialog_{}, confirmPrimary_{}, confirmSecondary_{}, confirmCancel_{};
    bool confirmOpen_ = false;
    std::function<void()> confirmPrimaryAction_, confirmSecondaryAction_;
    void HideConfirm();
    bool closeModalOpen_ = false;
    bool closeApproved_ = false;
    bool closeRequested_ = false;
    UiElementHandle gizmo_{};
    std::vector<UiElementHandle> gizmoAxes_;
    std::function<void(int, float, float)> gizmoDragHandler_;
    int gizmoDragAxis_ = 0;
    float gizmoLastX_ = 0.0f;
    float gizmoLastY_ = 0.0f;
    std::string editingKey_;
    std::string viewportImage_;
    std::vector<std::string> lines_;

    UiElementHandle root_{};
    UiElementHandle menuBar_{};
    UiElementHandle toolbar_{};
    UiElementHandle statusBar_{};
    UiElementHandle menuDropdown_{};
    struct QuickToolButton
    {
        UiElementHandle button{};
        std::string action;
        std::string label;
        EditorCommandId command = EditorCommandId::Save;
        std::string argument;
        bool rightAligned = false;
    };
    std::vector<UiElementHandle> menuButtons_;
    std::vector<UiElementHandle> toolbarButtons_;
    std::vector<QuickToolButton> quickButtons_;
    UiElementHandle hoveredChrome_;
    int hoveredSplitterIndex_ = -1;
    UiElementHandle pressedTransport_;
    bool previewPlaying_ = false;
    bool previewPaused_ = false;
    std::vector<std::string> menuNames_;
    std::vector<std::size_t> menuFirstItem_;
    std::string openMenu_;

    std::string status_ = "Ready";
    std::string selectionLabel_;
    std::string lastStatusText_;
    bool dirty_ = false;
    bool scriptsDirty_ = false;
    std::filesystem::path layoutPath_;

    float width_ = 1280.0f;
    float height_ = 720.0f;
    float dpiScale_ = 1.0f;
    bool initialized_ = false;
    float menuHeight_ = 24.0f;
    float toolbarHeight_ = 32.0f;
    float statusHeight_ = 24.0f;
    UiRect dockRect_{};
    EditorPanelId activePanel_ = EditorPanelId::Hierarchy;

    std::uint64_t dockRevision_ = 0;
    std::uint64_t builtDockRevision_ = ~std::uint64_t{0};
    bool splitterActive_ = false;
    float splitterOriginalRatio_ = 0.5f;
    std::vector<std::size_t> splitterPath_;
    DockAxis splitterAxis_ = DockAxis::Horizontal;
    float splitterStartRatio_ = 0.5f;
    float splitterStartCoordinate_ = 0.0f;
    float splitterTotal_ = 1.0f;

    EditorPanelId dragTabPanel_ = EditorPanelId::None;
    bool tabDragging_ = false;
    float tabDragStartX_ = 0.0f;
    float tabDragStartY_ = 0.0f;
    EditorPanelId dropTarget_ = EditorPanelId::None;
    bool dropBefore_ = false;
    enum class DockZone { Merge, Left, Right, Top, Bottom };
    DockZone dropZone_ = DockZone::Merge;
    EditorPanelId dropGroup_ = EditorPanelId::None;
    bool dropOnTabBar_ = false;
    [[nodiscard]] static DockZone ContentDropZone(const UiRect& group, float x, float y, DockZone previous);
};

} // namespace se::ui::editor
