# RG104-05：同一动态b4覆盖程序供Mask/Blend两种PSO共享；基底帧布局不变。
file(GLOB surface_shader_includes CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/*.hlsli")
set(surface_outputs)
set(material_surface_programs
    "Pbr|pbr"
    "Forward|forward"
    "ShadowedForward|shadowed"
    "EnvironmentForward|environment"
    "BasicLit|basiclit"
    "SkinnedForward|skinned-forward"
    "SkinnedEnvironmentForward|skinned-environment"
    "ShadowDepth|shadowdepth"
    "SkinnedShadowDepth|skinned-shadowdepth"
    "CascadedShadowedForward|cascaded-shadowed"
    "CascadedEnvironmentForward|cascaded-environment"
    "CascadedBasicLit|cascaded-basiclit"
    "CascadedSkinnedForward|cascaded-skinned-forward"
    "CascadedSkinnedEnvironmentForward|cascaded-skinned-environment"
    "ClusteredCascadedShadowedForward|clustered-cascaded-shadowed"
    "ClusteredCascadedEnvironmentForward|clustered-cascaded-environment"
    "ClusteredCascadedBasicLit|clustered-cascaded-basiclit"
    "ClusteredCascadedSkinnedForward|clustered-cascaded-skinned-forward"
    "ClusteredCascadedSkinnedEnvironmentForward|clustered-cascaded-skinned-environment"
    "ClusteredForward|clustered-forward"
    "ClusteredShadowedForward|clustered-shadowed"
    "ClusteredEnvironmentForward|clustered-environment"
    "ClusteredBasicLit|clustered-basiclit"
    "ClusteredSkinnedForward|clustered-skinned-forward"
    "ClusteredSkinnedShadowedForward|clustered-skinned-shadowed"
    "ClusteredSkinnedEnvironmentForward|clustered-skinned-environment"
    "PcssCascadedShadowedForward|pcss-cascaded-shadowed"
    "PcssCascadedEnvironmentForward|pcss-cascaded-environment"
    "PcssCascadedBasicLit|pcss-cascaded-basiclit"
    "PcssCascadedSkinnedForward|pcss-cascaded-skinned-forward"
    "PcssCascadedSkinnedEnvironmentForward|pcss-cascaded-skinned-environment"
    "PcssClusteredCascadedShadowedForward|pcss-clustered-cascaded-shadowed"
    "PcssClusteredCascadedEnvironmentForward|pcss-clustered-cascaded-environment"
    "PcssClusteredCascadedBasicLit|pcss-clustered-cascaded-basiclit"
    "PcssClusteredCascadedSkinnedForward|pcss-clustered-cascaded-skinned-forward"
    "PcssClusteredCascadedSkinnedEnvironmentForward|pcss-clustered-cascaded-skinned-environment"
)
foreach(surface_pair IN LISTS material_surface_programs)
    string(REPLACE "|" ";" surface_parts "${surface_pair}")
    list(GET surface_parts 0 surface_file)
    list(GET surface_parts 1 surface_mode)
    set(surface_output "${CMAKE_CURRENT_BINARY_DIR}/Resources/Shaders/${surface_file}.hlsl.surface.sebin")
    add_custom_command(OUTPUT "${surface_output}" "${surface_output}.json"
        COMMAND ShaderBuilder "surface-${surface_mode}" "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/${surface_file}.hlsl" "${surface_output}"
        DEPENDS ShaderBuilder "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/${surface_file}.hlsl"
            "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Pbr.hlsl"
            "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/BasicLit.hlsl"
            "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/ShadowDepth.hlsl"
            "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/SkinnedShadowDepth.hlsl"
            ${surface_shader_includes} VERBATIM)
    list(APPEND surface_outputs "${surface_output}" "${surface_output}.json")
endforeach()
set(particle_shader_output "${CMAKE_CURRENT_BINARY_DIR}/Resources/Shaders/ParticleBillboard.hlsl.sebin")
add_custom_command(OUTPUT "${particle_shader_output}" "${particle_shader_output}.json"
    COMMAND ShaderBuilder particle "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/ParticleBillboard.hlsl" "${particle_shader_output}"
    DEPENDS ShaderBuilder
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/ParticleBillboard.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/ParticleBindings.hlsli" VERBATIM)
list(APPEND surface_outputs "${particle_shader_output}" "${particle_shader_output}.json")
set(meshlet_as_output "${CMAKE_CURRENT_BINARY_DIR}/Resources/Shaders/MeshletCull.as.hlsl.sebin")
add_custom_command(OUTPUT "${meshlet_as_output}" "${meshlet_as_output}.json"
    COMMAND ShaderBuilder meshlet-ms "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/MeshletCull.as.hlsl" "${meshlet_as_output}"
    DEPENDS ShaderBuilder
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/MeshletCull.as.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/MeshletBindings.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/VisibilityPredicate.hlsli" VERBATIM)
set(meshlet_ms_output "${CMAKE_CURRENT_BINARY_DIR}/Resources/Shaders/MeshletDraw.ms.hlsl.sebin")
add_custom_command(OUTPUT "${meshlet_ms_output}" "${meshlet_ms_output}.json"
    COMMAND ShaderBuilder meshlet-ms "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/MeshletDraw.ms.hlsl" "${meshlet_ms_output}"
    DEPENDS ShaderBuilder
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/MeshletDraw.ms.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/MeshletBindings.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Pbr.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/Forward.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/ShadowedForward.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/EnvironmentForward.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/BasicLit.hlsl"
        ${surface_shader_includes} VERBATIM)
list(APPEND surface_outputs "${meshlet_as_output}" "${meshlet_as_output}.json" "${meshlet_ms_output}" "${meshlet_ms_output}.json")
add_custom_target(BuildMaterialSurfaceShaders DEPENDS ${surface_outputs})
add_dependencies(BuildShaders BuildMaterialSurfaceShaders)
