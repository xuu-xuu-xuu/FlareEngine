# Forward+ 独立产物，旧前向/CSM二进制布局保持可回退。
file(GLOB cluster_shader_includes CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/*.hlsli")
set(cluster_outputs)
foreach(cluster_pair "ClusteredForward|clustered-forward" "ClusteredShadowedForward|clustered-shadowed"
    "ClusteredEnvironmentForward|clustered-environment" "ClusteredBasicLit|clustered-basiclit"
    "ClusteredCascadedShadowedForward|clustered-cascaded-shadowed"
    "ClusteredCascadedEnvironmentForward|clustered-cascaded-environment"
    "ClusteredCascadedBasicLit|clustered-cascaded-basiclit"
    "ClusteredSkinnedForward|clustered-skinned-forward"
    "ClusteredSkinnedShadowedForward|clustered-skinned-shadowed"
    "ClusteredSkinnedEnvironmentForward|clustered-skinned-environment"
    "ClusteredCascadedSkinnedForward|clustered-cascaded-skinned-forward"
    "ClusteredCascadedSkinnedEnvironmentForward|clustered-cascaded-skinned-environment")
    string(REPLACE "|" ";" cluster_parts "${cluster_pair}")
    list(GET cluster_parts 0 cluster_file)
    list(GET cluster_parts 1 cluster_mode)
    set(cluster_output "${shader_artifacts}/${cluster_file}.hlsl.sebin")
    add_custom_command(OUTPUT "${cluster_output}" "${cluster_output}.json"
        COMMAND ShaderBuilder "${cluster_mode}" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/${cluster_file}.hlsl" "${cluster_output}"
        DEPENDS ShaderBuilder "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/${cluster_file}.hlsl"
            "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Pbr.hlsl"
            "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/BasicLit.hlsl"
            ${cluster_shader_includes} VERBATIM)
    list(APPEND cluster_outputs "${cluster_output}")
endforeach()
add_custom_target(BuildClusteredShaders DEPENDS ${cluster_outputs})
add_dependencies(BuildShaders BuildClusteredShaders)
