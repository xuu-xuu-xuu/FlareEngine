add_library(EngineCore STATIC
    Source/CoreLayer/Log.cpp
    Source/CoreLayer/Localization.cpp
    Source/CoreLayer/Utf.cpp
    Source/CoreLayer/TaskQueue.cpp
    Source/CoreLayer/Profiler.cpp
    Source/CoreLayer/Hash.cpp
    Source/CoreLayer/Math/SphericalHarmonics.cpp
    Source/CoreLayer/Spatial/BoundsIndex.cpp)
flare_target(EngineCore)
target_link_libraries(EngineCore PRIVATE bcrypt)
# UI 核心只依赖 EngineCore。

add_library(EngineUiCore STATIC
    Source/FunctionLayer/UI/Core/UiElement.cpp
    Source/FunctionLayer/UI/Core/UiDocument.cpp
    Source/FunctionLayer/UI/Core/UiDocumentLoader.cpp
    Source/FunctionLayer/UI/Core/UiBinding.cpp
    Source/FunctionLayer/UI/Core/UiLayout.cpp
    Source/FunctionLayer/UI/Core/UiStyle.cpp
    Source/FunctionLayer/UI/Core/UiInput.cpp)
flare_target(EngineUiCore)
target_link_libraries(EngineUiCore PUBLIC EngineCore)

add_library(EngineUiRender STATIC Source/FunctionLayer/Rendering/UiPaint.cpp)
flare_target(EngineUiRender)
target_link_libraries(EngineUiRender PUBLIC EngineUiCore EngineCore)

add_library(EngineUiWidgets STATIC
    Source/FunctionLayer/UI/Widgets/UiTextEdit.cpp
    Source/FunctionLayer/UI/Widgets/UiWidgets.cpp)
flare_target(EngineUiWidgets)
target_link_libraries(EngineUiWidgets PUBLIC EngineUiCore EngineCore)

# 色彩 LUT 的 CPU 实现同时被后处理（EngineSandboxRenderer）与编辑器 LUT 面板使用，
# 单独成库避免同一符号被两个静态库重复链接。

add_library(EngineColorGrading STATIC Source/FunctionLayer/Rendering/ColorGrading.cpp Source/FunctionLayer/Rendering/ColorLutFile.cpp)
flare_target(EngineColorGrading)
target_include_directories(EngineColorGrading PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Source")
target_link_libraries(EngineColorGrading PUBLIC EngineCore)

add_library(EngineEnvironmentAssets STATIC Source/ResourceLayer/Environment/TODProfile.cpp)
flare_target(EngineEnvironmentAssets)
target_link_libraries(EngineEnvironmentAssets PUBLIC EngineCore)

add_library(EngineRuntime STATIC
    Source/FunctionLayer/Runtime/ObjectHeap.cpp
    Source/FunctionLayer/Runtime/World.cpp
    Source/FunctionLayer/Runtime/WorldComponents.cpp
    Source/FunctionLayer/Runtime/Environment.cpp
    Source/ResourceLayer/Scenes/SceneSerializer.cpp
    Source/ResourceLayer/Scenes/PrefabDocument.cpp
    Source/FunctionLayer/Runtime/PrefabInstantiator.cpp
    Source/FunctionLayer/Runtime/ComponentMetadata.cpp
    Source/FunctionLayer/Runtime/FeatureComponents.cpp
    Source/FunctionLayer/Runtime/Camera.cpp
    Source/FunctionLayer/Runtime/TickScheduler.cpp
    Source/FunctionLayer/Runtime/EventBus.cpp
    Source/FunctionLayer/Runtime/WorldSceneCommit.cpp
    Source/FunctionLayer/Runtime/SceneManager.cpp
    Source/FunctionLayer/Runtime/SceneSpatialIndex.cpp
    Source/FunctionLayer/Runtime/TODSystem.cpp
    Source/FunctionLayer/Runtime/SkeletalAnimation.cpp)
flare_target(EngineRuntime)
target_link_libraries(EngineRuntime PUBLIC EngineCore EngineEnvironmentAssets)

add_library(EnginePhysics STATIC
    Source/FunctionLayer/Physics/CollisionShapes.cpp
    Source/FunctionLayer/Physics/TriggerTracker.cpp
    Source/FunctionLayer/Physics/PhysicsWorld.cpp
    Source/FunctionLayer/Physics/CharacterController.cpp)
flare_target(EnginePhysics)
target_link_libraries(EnginePhysics PUBLIC EngineCore)

add_library(EngineScenePhysics STATIC Source/FunctionLayer/Physics/ScenePhysics.cpp)
flare_target(EngineScenePhysics)
target_link_libraries(EngineScenePhysics PUBLIC EngineRuntime EnginePhysics)

add_library(ufbx STATIC ThirdParty/ufbx/ufbx.c)
set_target_properties(ufbx PROPERTIES C_STANDARD 99)
target_include_directories(ufbx PUBLIC ThirdParty/ufbx)

add_library(stb_vorbis STATIC ThirdParty/stb/stb_vorbis.c)
set_target_properties(stb_vorbis PROPERTIES C_STANDARD 99)
target_include_directories(stb_vorbis PUBLIC ThirdParty)
target_compile_definitions(stb_vorbis PRIVATE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(stb_vorbis PRIVATE /W0)
endif()

add_library(EngineAssets STATIC
    Source/ResourceLayer/Assets/AssetIO.cpp
    Source/ResourceLayer/Assets/AudioClip.cpp
    Source/ResourceLayer/Assets/DrWav.c
    Source/ResourceLayer/Assets/ModelImporter.cpp
    Source/ResourceLayer/Assets/AssetStore.cpp
    Source/ResourceLayer/Assets/MaterialDocument.cpp
    Source/ResourceLayer/Assets/AssetTasks.cpp
    Source/ResourceLayer/Assets/Primitives.cpp
    Source/ResourceLayer/Assets/HdrImage.cpp
    Source/ResourceLayer/Assets/EnvironmentBake.cpp
    Source/ResourceLayer/Assets/DiffuseProbeBake.cpp
    Source/ResourceLayer/Assets/DiffuseProbeAsset.cpp
    Source/ResourceLayer/Assets/BrdfEnergyBake.cpp
    Source/ResourceLayer/Assets/LtcBake.cpp
    Source/ResourceLayer/Assets/SheenEnergyBake.cpp
    Source/ResourceLayer/Assets/SkinDiffuseBake.cpp
    Source/ResourceLayer/Assets/EnvironmentIO.cpp
    Source/ResourceLayer/Assets/ImageWriter.cpp
    Source/ResourceLayer/Assets/VertexCompression.cpp
    Source/ResourceLayer/Assets/TextureCompression.cpp
    Source/ResourceLayer/Assets/Bc7Compression.cpp
    Source/ResourceLayer/Assets/Cgltf.c
    Source/ResourceLayer/Assets/AsyncAssetLoader.cpp
    Source/ResourceLayer/Assets/StaticModelPreparation.cpp
    Source/ResourceLayer/Assets/MaterialMetadata.cpp
    Source/ResourceLayer/Assets/SurfaceShader.cpp
    Source/ResourceLayer/Assets/ShaderGraph.cpp
    Source/ResourceLayer/Assets/TextureBake.cpp
    Source/ResourceLayer/Assets/LodBake.cpp
    Source/ResourceLayer/Assets/HlodBake.cpp
    Source/ResourceLayer/Assets/MeshletBake.cpp
    Source/ResourceLayer/Assets/ImpostorBake.cpp)
flare_target(EngineAssets)
if(MSVC)
    set_source_files_properties(Source/ResourceLayer/Assets/DrWav.c PROPERTIES COMPILE_OPTIONS "/W0")
endif()
target_include_directories(EngineAssets SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/LtcReference/glm/headers")
target_link_libraries(EngineAssets PUBLIC EngineCore EngineEnvironmentAssets PRIVATE ufbx stb_vorbis bcrypt ole32 windowscodecs)

# 离线场景辐照准备只依赖CPU世界和资产，不拉入RHI。
add_library(EngineDiffuseProbes STATIC Source/FunctionLayer/Rendering/DiffuseProbePreparation.cpp)
flare_target(EngineDiffuseProbes)
target_link_libraries(EngineDiffuseProbes PUBLIC EngineRuntime EngineAssets)

add_library(EngineGameUI STATIC
    Source/FunctionLayer/GameUI/UiTree.cpp
    Source/ResourceLayer/Fonts/FontCache.cpp
    Source/FunctionLayer/GameUI/ControlsGallery.cpp
    Source/FunctionLayer/GameUI/UiEventSystem.cpp
    Source/FunctionLayer/GameUI/UiTextPresentation.cpp
    Source/FunctionLayer/GameUI/UiControlGeometry.cpp
    Source/FunctionLayer/GameUI/UiTransitionDemo.cpp)
flare_target(EngineGameUI)
target_link_libraries(EngineGameUI PUBLIC EngineCore PRIVATE dwrite)

add_library(EngineInput STATIC
    Source/FunctionLayer/Input/InputActionAsset.cpp
    Source/FunctionLayer/Input/InputActionMap.cpp)
flare_target(EngineInput)
target_link_libraries(EngineInput PUBLIC EngineCore)

add_library(EngineAudio STATIC Source/FunctionLayer/Audio/AudioWorld.cpp)
flare_target(EngineAudio)
target_link_libraries(EngineAudio PUBLIC EngineCore)

add_library(EngineSceneAudio STATIC Source/FunctionLayer/Audio/SceneAudio.cpp)
flare_target(EngineSceneAudio)
target_link_libraries(EngineSceneAudio PUBLIC EngineRuntime EngineAudio EngineAssets)

add_library(EngineParticles STATIC Source/FunctionLayer/Particles/ParticleSimulator.cpp)
flare_target(EngineParticles)
target_link_libraries(EngineParticles PUBLIC EngineCore)

add_library(EngineSceneParticles STATIC Source/FunctionLayer/Particles/SceneParticles.cpp)
flare_target(EngineSceneParticles)
target_link_libraries(EngineSceneParticles PUBLIC EngineRuntime EngineParticles)

add_library(EngineNavigation STATIC
    Source/FunctionLayer/Navigation/NavMeshData.cpp
    Source/FunctionLayer/Navigation/NavMeshBaker.cpp
    Source/FunctionLayer/Navigation/NavPathfinder.cpp)
flare_target(EngineNavigation)
target_link_libraries(EngineNavigation PUBLIC EngineCore)

add_library(EngineSceneNavigation STATIC Source/FunctionLayer/Navigation/SceneNavigation.cpp)
flare_target(EngineSceneNavigation)
target_link_libraries(EngineSceneNavigation PUBLIC EngineRuntime EngineNavigation EngineScenePhysics EngineAssets)

add_library(EngineNative SHARED Source/FunctionLayer/Scripting/NativeBridge/EngineNative.cpp Source/FunctionLayer/Scripting/NativeBridge/WindowProfile.cpp
    Source/FunctionLayer/Scripting/NativeBridge/PlayerQualityMenu.cpp)
flare_target(EngineNative)
target_link_libraries(EngineNative PRIVATE EngineRuntime EnginePhysics EngineScenePhysics EngineAudio EngineSceneAudio EngineParticles EngineSceneParticles EngineNavigation EngineSceneNavigation EngineGameUI EngineInput EngineSandboxHost EngineSandboxRenderer EngineEditorApp EngineEditorRenderer)

add_library(EngineApplication STATIC Source/FunctionLayer/Application/FrameData.cpp Source/FunctionLayer/Application/FeatureFrameData.cpp Source/FunctionLayer/Application/FreeCameraInput.cpp
    Source/FunctionLayer/Application/PlayerQuality.cpp)
flare_target(EngineApplication)
target_link_libraries(EngineApplication PUBLIC EngineRuntime EnginePhysics EngineSceneParticles EngineGameUI EngineAssets)

add_library(EnginePlatform STATIC
    Source/PlatformLayer/Windows/ExecutablePath.cpp
    Source/PlatformLayer/Windows/StartupProgress.cpp
    Source/PlatformLayer/Windows/AppBranding.cpp
    Source/PlatformLayer/Windows/ProcessDiagnostics.cpp
    Source/PlatformLayer/Windows/HResult.cpp
    Source/PlatformLayer/Windows/Window.cpp
    Source/PlatformLayer/Windows/WindowsGamepad.cpp
    Source/PlatformLayer/Windows/WindowsInput.cpp
    Source/PlatformLayer/Windows/WindowsUiInput.cpp
    Source/PlatformLayer/Windows/FrameTimer.cpp
    Source/PlatformLayer/Windows/UiDirectWriteTextService.cpp
    Source/PlatformLayer/Windows/Win32UiPlatformServices.cpp
    Source/PlatformLayer/Windows/XAudio2Backend.cpp)
flare_target(EnginePlatform)
target_link_libraries(EnginePlatform PUBLIC EngineCore EngineGameUI PRIVATE user32 imm32 shell32 xaudio2)

add_library(EngineSandboxHost STATIC Source/FunctionLayer/Application/SandboxApp.cpp Source/FunctionLayer/Application/SandboxDiagnostics.cpp)
flare_target(EngineSandboxHost)
target_link_libraries(EngineSandboxHost PUBLIC EnginePlatform EngineApplication)

add_library(EngineProject STATIC Source/ToolLayer/Tools/Project.cpp)
flare_target(EngineProject)
target_link_libraries(EngineProject PUBLIC EngineRuntime EngineAssets EngineGameUI EnginePhysics EngineInput EngineNavigation EngineCore)

add_library(EngineProjectSession STATIC Source/ToolLayer/Editor/ProjectWorkspaceSession.cpp)
flare_target(EngineProjectSession)
target_link_libraries(EngineProjectSession PUBLIC EngineProject EnginePlatform)

add_executable(ProjectTool Source/ToolLayer/Tools/ProjectTool.cpp)
flare_target(ProjectTool)
target_link_libraries(ProjectTool PRIVATE EngineProject)
