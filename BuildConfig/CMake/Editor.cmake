add_library(EngineEditorUi STATIC
    Source/ToolLayer/Editor/EditorUi/EditorCommand.cpp
    Source/ToolLayer/Editor/EditorUi/EditorCommandRouter.cpp
    Source/ToolLayer/Editor/EditorUi/EditorDock.cpp
    Source/ToolLayer/Editor/EditorUi/EditorDockState.cpp
    Source/ToolLayer/Editor/EditorUi/EditorChrome.cpp
    Source/ToolLayer/Editor/EditorUi/EditorInputAdapter.cpp
    Source/ToolLayer/Editor/EditorUi/EditorShell.cpp
    Source/ToolLayer/Editor/EditorUi/EditorShellChrome.cpp
    Source/ToolLayer/Editor/EditorUi/EditorShellDock.cpp
    Source/ToolLayer/Editor/EditorUi/EditorShellForms.cpp
    Source/ToolLayer/Editor/EditorUi/EditorShellModals.cpp
    Source/ToolLayer/Editor/EditorUi/EditorShellPanels.cpp
    Source/ToolLayer/Editor/EditorUi/EditorDesignerSurface.cpp
    Source/ToolLayer/Editor/EditorUi/EditorInspector.cpp
    Source/ToolLayer/Editor/EditorUi/EditorColorPicker.cpp
    Source/ToolLayer/Editor/EditorUi/EditorPanels.cpp)
flare_target(EngineEditorUi)
target_link_libraries(EngineEditorUi PUBLIC EngineUiCore EngineUiWidgets EngineCore PRIVATE user32 imm32)

add_library(EngineEditor STATIC
    Source/ToolLayer/Editor/MaterialThumbnail.cpp
    Source/ToolLayer/Editor/ExternalChangeMonitor.cpp
    Source/ToolLayer/Editor/BuildRestartRequest.cpp
    Source/ToolLayer/Editor/RecoveryApply.cpp
    Source/ToolLayer/Editor/EditorSession.cpp
    Source/ToolLayer/Editor/EditorLayout.cpp
    Source/ToolLayer/Editor/EditorCommand.cpp
    Source/ToolLayer/Editor/AssetImportService.cpp
    Source/ToolLayer/Editor/EditorPlayback.cpp
    Source/ToolLayer/Editor/EditorSessionStore.cpp
    Source/ToolLayer/Editor/ExportOperation.cpp
    Source/ToolLayer/Editor/ProjectSettingsService.cpp
    Source/ToolLayer/Editor/ExportService.cpp
    Source/ToolLayer/Editor/ExportJob.cpp
    Source/ToolLayer/Editor/EditorProcessRunner.cpp
    Source/ToolLayer/Editor/TODPreview.cpp
    Source/ToolLayer/Editor/ExternalProcessBroker.cpp
    Source/ToolLayer/Editor/EditorAssetActions.cpp
    Source/ToolLayer/Editor/EditorScriptTools.cpp
    Source/ToolLayer/Editor/EditorSettingsDocuments.cpp
    Source/ToolLayer/Editor/EditorViewportCamera.cpp
    Source/ToolLayer/Editor/EditorGizmo.cpp
    Source/ToolLayer/Editor/MaterialEditSession.cpp
    Source/ToolLayer/Editor/SceneFileWorkflow.cpp
    Source/ToolLayer/Editor/SceneDefaults.cpp
    Source/ToolLayer/Editor/UiDocumentWorkflow.cpp
    Source/ToolLayer/Editor/EditorMaterialWorkflow.cpp
    Source/ToolLayer/Editor/EditorWorkspaceHost.cpp
    Source/ToolLayer/Editor/NavigationBakeService.cpp
    Source/ToolLayer/Editor/PrefabWorkflow.cpp
    Source/ToolLayer/Editor/UICanvasEditor.cpp)
flare_target(EngineEditor)
target_link_libraries(EngineEditor PUBLIC EngineRuntime EngineScenePhysics EngineAudio EngineSceneAudio EngineParticles EngineSceneParticles EngineNavigation EngineSceneNavigation EngineAssets EngineGameUI EnginePlatform EngineProjectSession EngineColorGrading)
target_link_libraries(EngineEditor PRIVATE comdlg32 shell32 shlwapi)

add_library(EngineEditorApp STATIC
    Source/ToolLayer/Editor/EditorApplication.cpp
    Source/ToolLayer/Editor/EditorToolkitForms.cpp
    Source/ToolLayer/Editor/EditorToolkitSession.cpp
    Source/ToolLayer/Editor/SceneInspector.cpp
    Source/ToolLayer/Editor/FeatureInspector.cpp
    Source/ToolLayer/Editor/UiInspector.cpp
    Source/ToolLayer/Editor/TodInspector.cpp
    Source/ToolLayer/Editor/EditorFeatureTools.cpp
    Source/ToolLayer/Editor/EditorCreation.cpp
    Source/ToolLayer/Editor/ViewportDropWorkflow.cpp
    Source/ToolLayer/Editor/EditorClipPreview.cpp
    Source/ToolLayer/Editor/EditorScriptWorkflow.cpp
    Source/ToolLayer/Editor/EditorPlayInput.cpp
    Source/ToolLayer/Editor/EditorViewportController.cpp
    Source/ToolLayer/Editor/EditorSceneGizmos.cpp
    Source/ToolLayer/Editor/EditorFloatingWindows.cpp)
flare_target(EngineEditorApp)
target_link_libraries(EngineEditorApp PUBLIC EngineEditor EngineScenePhysics EngineSandboxHost EngineEditorUi EngineUiRender EnginePlatform EngineSandboxRenderer PRIVATE dwmapi)

add_library(EngineEditorProduct STATIC
    Source/ToolLayer/Editor/EditorLauncher.cpp
    Source/ToolLayer/Editor/LauncherIntro.cpp
    Source/ToolLayer/Editor/LauncherStyle.cpp
    Source/ToolLayer/Editor/EditorWorker.cpp
    Source/ToolLayer/Editor/EditorLoadingScreen.cpp
    Source/ToolLayer/Editor/LoadingRibbon.cpp)
flare_target(EngineEditorProduct)

add_library(EngineEditorRenderer STATIC Source/ToolLayer/Editor/EditorRenderer.cpp)
flare_target(EngineEditorRenderer)
target_link_libraries(EngineEditorRenderer PUBLIC EngineSandboxRenderer)
target_link_libraries(EngineEditorProduct PUBLIC EngineEditorApp EngineEditorRenderer EngineSandboxRenderer EngineAssets EngineProjectSession ole32 uuid shell32 comdlg32 gdi32 PRIVATE dwmapi uxtheme comctl32 gdiplus pathcch)

add_executable(FlareEditor WIN32 Source/ToolLayer/Tools/FlareEditor.cpp)
flare_target(FlareEditor)
target_link_libraries(FlareEditor PRIVATE EngineEditorProduct)
foreach(editor_target FlareEditor EngineNative)
    target_sources(${editor_target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/FlareResources.rc")
endforeach()
target_compile_definitions(EngineNative PRIVATE FLARE_NATIVE_LIBRARY)
# FlareEditor 为正式编辑器 CMake 目标名与交付程序名（FlareEditor.exe）。
