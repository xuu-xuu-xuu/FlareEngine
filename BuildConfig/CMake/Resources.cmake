add_custom_target(CopyLocalizationResources ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:FlareEditor>/Resources/Localization"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Localization/zh-CN.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Localization/en-US.txt"
        "$<TARGET_FILE_DIR:FlareEditor>/Resources/Localization" VERBATIM)
add_dependencies(FlareEditor CopyLocalizationResources CopyRendererResources)
add_custom_target(CopyFlareBranding ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/Resources/Branding"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Branding/Flare.ico"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Branding/FlareMark.png"
        "${CMAKE_BINARY_DIR}/Resources/Branding"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Branding/FlareMark-README.png"
        "${CMAKE_BINARY_DIR}/Resources/Branding/FlareMarkTransparent.png" VERBATIM)
add_dependencies(FlareEditor CopyFlareBranding)
add_custom_target(CopyEditorTemplates ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/Resources"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Templates"
        "${CMAKE_BINARY_DIR}/Resources/Templates" VERBATIM)
add_dependencies(FlareEditor CopyEditorTemplates)
add_dependencies(EngineNative CopyLocalizationResources)
add_custom_target(CopyRendererResources ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:FlareEditor>/Resources/Shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders"
        "$<TARGET_FILE_DIR:FlareEditor>/Resources/Shaders"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:FlareEditor>/Resources/Textures"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Textures"
        "$<TARGET_FILE_DIR:FlareEditor>/Resources/Textures" VERBATIM)
add_dependencies(EngineNative CopyRendererResources)

add_executable(ShaderBuilder Source/ToolLayer/Tools/ShaderBuilder.cpp)
flare_target(ShaderBuilder)
target_link_libraries(ShaderBuilder PRIVATE EngineGraphics EnginePlatform)
add_dependencies(ShaderBuilder CopyLocalizationResources)

add_executable(EnvironmentCooker Source/ToolLayer/Tools/EnvironmentCooker.cpp)
flare_target(EnvironmentCooker)
target_link_libraries(EnvironmentCooker PRIVATE EngineAssets EnginePlatform)
add_dependencies(EnvironmentCooker CopyLocalizationResources)
add_executable(DiffuseProbeCooker Source/ToolLayer/Tools/DiffuseProbeCooker.cpp)
flare_target(DiffuseProbeCooker)
target_link_libraries(DiffuseProbeCooker PRIVATE EngineDiffuseProbes EngineProject)
add_dependencies(DiffuseProbeCooker CopyLocalizationResources)

add_executable(NavigationCooker Source/ToolLayer/Tools/NavigationCooker.cpp)
flare_target(NavigationCooker)
target_link_libraries(NavigationCooker PRIVATE EngineSceneNavigation EngineProject EnginePlatform)
add_dependencies(NavigationCooker CopyLocalizationResources)

set(environment_outputs "${CMAKE_BINARY_DIR}/Resources/Environments")
foreach(environment_kind soft contrast)
    add_custom_command(OUTPUT "${environment_outputs}/${environment_kind}.stamp"
        COMMAND EnvironmentCooker sample ${environment_kind} "${environment_outputs}/${environment_kind}.hdr"
        COMMAND EnvironmentCooker cook "${environment_outputs}/${environment_kind}.hdr" "${environment_outputs}/${environment_kind}.seenv"
        COMMAND ${CMAKE_COMMAND} -E touch "${environment_outputs}/${environment_kind}.stamp"
        DEPENDS EnvironmentCooker
        BYPRODUCTS "${environment_outputs}/${environment_kind}.hdr" "${environment_outputs}/${environment_kind}.seenv"
            "${environment_outputs}/${environment_kind}.seenv.json" "${environment_outputs}/${environment_kind}.seenv.asset.json" VERBATIM)
endforeach()
add_custom_target(BuildEnvironments ALL DEPENDS "${environment_outputs}/soft.stamp" "${environment_outputs}/contrast.stamp")
add_dependencies(CopyRendererResources BuildEnvironments)
set(shader_artifacts "${CMAKE_BINARY_DIR}/Resources/Shaders")
add_custom_command(OUTPUT "${shader_artifacts}/Triangle.hlsl.sebin" "${shader_artifacts}/Triangle.hlsl.sebin.json"
    COMMAND ShaderBuilder triangle "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Triangle.hlsl" "${shader_artifacts}/Triangle.hlsl.sebin"
    DEPENDS ShaderBuilder "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Triangle.hlsl" VERBATIM)
add_custom_command(OUTPUT "${shader_artifacts}/TexturedMesh.hlsl.sebin" "${shader_artifacts}/TexturedMesh.hlsl.sebin.json"
    COMMAND ShaderBuilder mesh "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/TexturedMesh.hlsl" "${shader_artifacts}/TexturedMesh.hlsl.sebin"
    DEPENDS ShaderBuilder "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/TexturedMesh.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/MeshBindings.hlsli" VERBATIM)
add_custom_command(OUTPUT "${shader_artifacts}/Pbr.hlsl.sebin" "${shader_artifacts}/Pbr.hlsl.sebin.json"
    COMMAND ShaderBuilder pbr "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Pbr.hlsl" "${shader_artifacts}/Pbr.hlsl.sebin"
    DEPENDS ShaderBuilder "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Pbr.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/PbrBindings.hlsli" VERBATIM)
add_custom_command(OUTPUT "${shader_artifacts}/PbrInstanced.hlsl.sebin" "${shader_artifacts}/PbrInstanced.hlsl.sebin.json"
    COMMAND ShaderBuilder pbr-instanced "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/PbrInstanced.hlsl" "${shader_artifacts}/PbrInstanced.hlsl.sebin"
    DEPENDS ShaderBuilder "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/PbrInstanced.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/InstancingBindings.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Pbr.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/PbrBindings.hlsli" VERBATIM)
add_custom_command(OUTPUT "${shader_artifacts}/PbrForwardInstanced.hlsl.sebin" "${shader_artifacts}/PbrForwardInstanced.hlsl.sebin.json"
    COMMAND ShaderBuilder pbr-forward-instanced "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/PbrForwardInstanced.hlsl" "${shader_artifacts}/PbrForwardInstanced.hlsl.sebin"
    DEPENDS ShaderBuilder "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/PbrForwardInstanced.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/InstancingBindings.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/ForwardBindings.hlsli" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Pbr.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/PbrBindings.hlsli" VERBATIM)
add_custom_command(OUTPUT "${shader_artifacts}/Forward.hlsl.sebin" "${shader_artifacts}/Forward.hlsl.sebin.json"
    COMMAND ShaderBuilder forward "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Forward.hlsl" "${shader_artifacts}/Forward.hlsl.sebin"
    DEPENDS ShaderBuilder "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Forward.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/ForwardBindings.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Pbr.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/PbrBindings.hlsli" VERBATIM)
add_custom_command(OUTPUT "${shader_artifacts}/FinalOutput.hlsl.sebin" "${shader_artifacts}/FinalOutput.hlsl.sebin.json"
    COMMAND ShaderBuilder output "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/FinalOutput.hlsl" "${shader_artifacts}/FinalOutput.hlsl.sebin"
    DEPENDS ShaderBuilder "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/FinalOutput.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Fullscreen.hlsli" VERBATIM)
add_custom_command(OUTPUT "${shader_artifacts}/Sky.hlsl.sebin" "${shader_artifacts}/Sky.hlsl.sebin.json"
    COMMAND ShaderBuilder sky "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Sky.hlsl" "${shader_artifacts}/Sky.hlsl.sebin"
    DEPENDS ShaderBuilder "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Sky.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Fullscreen.hlsli" VERBATIM)
add_custom_target(BuildShaders ALL DEPENDS "${shader_artifacts}/Triangle.hlsl.sebin" "${shader_artifacts}/TexturedMesh.hlsl.sebin" "${shader_artifacts}/Pbr.hlsl.sebin" "${shader_artifacts}/PbrInstanced.hlsl.sebin"
    "${shader_artifacts}/PbrForwardInstanced.hlsl.sebin"
    "${shader_artifacts}/Forward.hlsl.sebin" "${shader_artifacts}/FinalOutput.hlsl.sebin" "${shader_artifacts}/Sky.hlsl.sebin"
    "${shader_artifacts}/ShadowedForward.hlsl.sebin" "${shader_artifacts}/ShadowDepth.hlsl.sebin" "${shader_artifacts}/ShadowOutput.hlsl.sebin"
    "${shader_artifacts}/BasicLit.hlsl.sebin"
    "${shader_artifacts}/EnvironmentForward.hlsl.sebin" "${shader_artifacts}/EnvironmentSky.hlsl.sebin"
    "${shader_artifacts}/SkinnedForward.hlsl.sebin" "${shader_artifacts}/SkinnedShadowDepth.hlsl.sebin"
    "${shader_artifacts}/SkinnedEnvironmentForward.hlsl.sebin"
    "${shader_artifacts}/BloomExtract.hlsl.sebin" "${shader_artifacts}/BloomBlur.hlsl.sebin" "${shader_artifacts}/ToneMap.hlsl.sebin"
    "${shader_artifacts}/Fxaa.hlsl.sebin" "${shader_artifacts}/UiOverlay.hlsl.sebin")
add_custom_command(OUTPUT "${shader_artifacts}/ShadowedForward.hlsl.sebin" "${shader_artifacts}/ShadowedForward.hlsl.sebin.json"
    COMMAND ShaderBuilder shadowed "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/ShadowedForward.hlsl" "${shader_artifacts}/ShadowedForward.hlsl.sebin"
    DEPENDS ShaderBuilder "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/ShadowedForward.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/ShadowBindings.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Forward.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/ForwardBindings.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Pbr.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/PbrBindings.hlsli" VERBATIM)
add_custom_command(OUTPUT "${shader_artifacts}/BasicLit.hlsl.sebin" "${shader_artifacts}/BasicLit.hlsl.sebin.json"
    COMMAND ShaderBuilder basiclit "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/BasicLit.hlsl" "${shader_artifacts}/BasicLit.hlsl.sebin"
    DEPENDS ShaderBuilder "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/BasicLit.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/PbrBindings.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/ForwardBindings.hlsli" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/ShadowBindings.hlsli" VERBATIM)
add_custom_command(OUTPUT "${shader_artifacts}/ShadowDepth.hlsl.sebin" "${shader_artifacts}/ShadowDepth.hlsl.sebin.json"
    COMMAND ShaderBuilder shadowdepth "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/ShadowDepth.hlsl" "${shader_artifacts}/ShadowDepth.hlsl.sebin"
    DEPENDS ShaderBuilder "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/ShadowDepth.hlsl" VERBATIM)
add_custom_command(OUTPUT "${shader_artifacts}/ShadowOutput.hlsl.sebin" "${shader_artifacts}/ShadowOutput.hlsl.sebin.json"
    COMMAND ShaderBuilder shadowoutput "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/ShadowOutput.hlsl" "${shader_artifacts}/ShadowOutput.hlsl.sebin"
    DEPENDS ShaderBuilder "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/ShadowOutput.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/FinalOutput.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Fullscreen.hlsli" VERBATIM)
add_custom_command(OUTPUT "${shader_artifacts}/EnvironmentForward.hlsl.sebin" "${shader_artifacts}/EnvironmentForward.hlsl.sebin.json"
    COMMAND ShaderBuilder environment "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/EnvironmentForward.hlsl" "${shader_artifacts}/EnvironmentForward.hlsl.sebin"
    DEPENDS ShaderBuilder "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/EnvironmentForward.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/EnvironmentBindings.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/ShadowedForward.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/ShadowBindings.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Forward.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/ForwardBindings.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Pbr.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/PbrBindings.hlsli" VERBATIM)
add_custom_command(OUTPUT "${shader_artifacts}/EnvironmentSky.hlsl.sebin" "${shader_artifacts}/EnvironmentSky.hlsl.sebin.json"
    COMMAND ShaderBuilder environmentsky "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/EnvironmentSky.hlsl" "${shader_artifacts}/EnvironmentSky.hlsl.sebin"
    DEPENDS ShaderBuilder "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/EnvironmentSky.hlsl" VERBATIM)
# W03：蒙皮变体（31 号绑定布局 + v3 顶点布局 + SE_SKINNED）。pass 名与 ShaderBuilder.cpp 一致。
add_custom_command(OUTPUT "${shader_artifacts}/SkinnedForward.hlsl.sebin" "${shader_artifacts}/SkinnedForward.hlsl.sebin.json"
    COMMAND ShaderBuilder skinned-forward "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/SkinnedForward.hlsl" "${shader_artifacts}/SkinnedForward.hlsl.sebin"
    DEPENDS ShaderBuilder "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/SkinnedForward.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Skinning.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/ShadowedForward.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/ShadowBindings.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Forward.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/ForwardBindings.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Pbr.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/PbrBindings.hlsli" VERBATIM)
file(GLOB skinned_environment_includes CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/*.hlsli")
add_custom_command(OUTPUT "${shader_artifacts}/SkinnedEnvironmentForward.hlsl.sebin" "${shader_artifacts}/SkinnedEnvironmentForward.hlsl.sebin.json"
    COMMAND ShaderBuilder skinned-environment "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/SkinnedEnvironmentForward.hlsl" "${shader_artifacts}/SkinnedEnvironmentForward.hlsl.sebin"
    DEPENDS ShaderBuilder "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/SkinnedEnvironmentForward.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/EnvironmentForward.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/ShadowedForward.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Forward.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Pbr.hlsl" ${skinned_environment_includes} VERBATIM)
add_custom_command(OUTPUT "${shader_artifacts}/SkinnedShadowDepth.hlsl.sebin" "${shader_artifacts}/SkinnedShadowDepth.hlsl.sebin.json"
    COMMAND ShaderBuilder skinned-shadowdepth "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/SkinnedShadowDepth.hlsl" "${shader_artifacts}/SkinnedShadowDepth.hlsl.sebin"
    DEPENDS ShaderBuilder "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/SkinnedShadowDepth.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Skinning.hlsli" VERBATIM)
add_dependencies(CopyRendererResources BuildShaders)
add_custom_command(OUTPUT "${shader_artifacts}/Gui.hlsl.sebin" "${shader_artifacts}/Gui.hlsl.sebin.json"
    COMMAND ShaderBuilder gui "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Gui.hlsl" "${shader_artifacts}/Gui.hlsl.sebin"
    DEPENDS ShaderBuilder "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Gui.hlsl" VERBATIM)
add_custom_target(CopyGuiResources
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Gui.hlsl" "${shader_artifacts}/Gui.hlsl"
    DEPENDS "${shader_artifacts}/Gui.hlsl.sebin" VERBATIM)
add_dependencies(CopyRendererResources CopyGuiResources)
foreach(post_shader BloomExtract BloomBlur ToneMap Fxaa UiOverlay)
    string(TOLOWER "${post_shader}" post_kind)
    if(post_shader STREQUAL "UiOverlay")
        set(post_kind overlay)
    endif()
    add_custom_command(OUTPUT "${shader_artifacts}/${post_shader}.hlsl.sebin" "${shader_artifacts}/${post_shader}.hlsl.sebin.json"
        COMMAND ShaderBuilder ${post_kind} "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/${post_shader}.hlsl" "${shader_artifacts}/${post_shader}.hlsl.sebin"
        DEPENDS ShaderBuilder "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/${post_shader}.hlsl" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/PostBindings.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Fullscreen.hlsli" VERBATIM)
endforeach()
