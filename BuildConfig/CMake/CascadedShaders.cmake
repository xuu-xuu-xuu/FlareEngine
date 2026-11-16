# CSM 是新增技术；旧单图产物保持其原布局和文件名。
file(GLOB csm_shader_includes CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/*.hlsli")
set(csm_outputs)
foreach(csm_pair "CascadedShadowedForward|cascaded-shadowed" "CascadedEnvironmentForward|cascaded-environment"
    "CascadedBasicLit|cascaded-basiclit" "CascadedSkinnedForward|cascaded-skinned-forward"
    "CascadedShadowOutput|cascaded-output" "CascadedSkinnedEnvironmentForward|cascaded-skinned-environment")
    string(REPLACE "|" ";" csm_parts "${csm_pair}")
    list(GET csm_parts 0 csm_file)
    list(GET csm_parts 1 csm_mode)
    set(csm_output "${shader_artifacts}/${csm_file}.hlsl.sebin")
    add_custom_command(OUTPUT "${csm_output}" "${csm_output}.json"
        COMMAND ShaderBuilder "${csm_mode}" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/${csm_file}.hlsl" "${csm_output}"
        DEPENDS ShaderBuilder "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/${csm_file}.hlsl"
            "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Pbr.hlsl"
            "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/BasicLit.hlsl"
            "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/FinalOutput.hlsl"
            ${csm_shader_includes} VERBATIM)
    list(APPEND csm_outputs "${csm_output}")
endforeach()
add_custom_target(BuildCascadedShaders DEPENDS ${csm_outputs})
add_dependencies(BuildShaders BuildCascadedShaders)
