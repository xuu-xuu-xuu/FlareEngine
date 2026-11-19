# schema 40 与覆盖38正交；显式构建普通/裁切两组扩展程序，旧产物保留。
set(shading_outputs)
foreach(shading_pair IN LISTS material_surface_programs)
    string(REPLACE "|" ";" shading_parts "${shading_pair}")
    list(GET shading_parts 0 shading_file)
    list(GET shading_parts 1 shading_mode)
    if(shading_mode STREQUAL "pbr" OR shading_mode MATCHES "shadowdepth")
        continue()
    endif()
    foreach(shading_surface IN ITEMS FALSE TRUE)
        if(shading_surface)
            set(shading_prefix "surface-shading-")
            set(shading_suffix ".surface.shading.sebin")
        else()
            set(shading_prefix "shading-")
            set(shading_suffix ".shading.sebin")
        endif()
        set(shading_output "${CMAKE_CURRENT_BINARY_DIR}/Resources/Shaders/${shading_file}.hlsl${shading_suffix}")
        add_custom_command(OUTPUT "${shading_output}" "${shading_output}.json"
            COMMAND ShaderBuilder "${shading_prefix}${shading_mode}"
                "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/${shading_file}.hlsl" "${shading_output}"
            DEPENDS ShaderBuilder "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/${shading_file}.hlsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Pbr.hlsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/BasicLit.hlsl" ${surface_shader_includes} VERBATIM)
        list(APPEND shading_outputs "${shading_output}" "${shading_output}.json")
    endforeach()
endforeach()
add_custom_target(BuildShadingExtensionShaders DEPENDS ${shading_outputs})
add_dependencies(BuildShaders BuildShadingExtensionShaders)
set(occlusion_source "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/AmbientOcclusion.hlsl")
set(occlusion_output "${CMAKE_CURRENT_BINARY_DIR}/Resources/Shaders/AmbientOcclusion.hlsl")
add_custom_command(OUTPUT "${occlusion_output}.GtaoHalf.seao" "${occlusion_output}.GtaoHalf.seao.json"
    "${occlusion_output}.ResolveOcclusion.seao" "${occlusion_output}.ResolveOcclusion.seao.json"
    COMMAND ShaderBuilder ao-compute "${occlusion_source}" "${occlusion_output}"
    DEPENDS ShaderBuilder "${occlusion_source}" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/AmbientOcclusionBindings.hlsli" VERBATIM)
add_custom_target(BuildOcclusionShaders DEPENDS "${occlusion_output}.GtaoHalf.seao" "${occlusion_output}.ResolveOcclusion.seao")
add_dependencies(BuildShaders BuildOcclusionShaders)
set(raytracing_source "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/RayTracing.hlsl")
set(raytracing_output "${CMAKE_CURRENT_BINARY_DIR}/Resources/Shaders/RayTracing.hlsl.sebin")
add_custom_command(OUTPUT "${raytracing_output}" "${raytracing_output}.json"
    COMMAND ShaderBuilder raytracing "${raytracing_source}" "${raytracing_output}"
    DEPENDS ShaderBuilder "${raytracing_source}" VERBATIM)
add_custom_target(BuildRayTracingShaders DEPENDS "${raytracing_output}" "${raytracing_output}.json")
add_dependencies(BuildShaders BuildRayTracingShaders)
add_dependencies(SelfEngineEditor BuildShaders)
add_dependencies(EngineNative BuildShaders)

# 生产LUT由独立离线工具生成，渲染线程只加载；不会在打开材质时阻塞烘焙。
add_executable(RenderingCooker Source/ToolLayer/Tools/RenderingCooker.cpp)
se_target(RenderingCooker)
target_link_libraries(RenderingCooker PRIVATE EngineAssets EngineCore EnginePlatform)
add_dependencies(RenderingCooker CopyLocalizationResources)
set(shading_assets)
foreach(shading_asset direct-energy.sebrdf rectangle.seltc sheen.sesheen skin.seskin)
    list(APPEND shading_assets "${CMAKE_CURRENT_BINARY_DIR}/Resources/Lighting/${shading_asset}")
endforeach()
add_custom_command(OUTPUT ${shading_assets}
    COMMAND RenderingCooker defaults "${CMAKE_CURRENT_BINARY_DIR}/Resources/Lighting"
    DEPENDS RenderingCooker VERBATIM)
add_custom_target(BuildShadingResources ALL DEPENDS ${shading_assets})
add_dependencies(SelfEngineEditor BuildShadingResources)
add_dependencies(EngineNative BuildShadingResources)
