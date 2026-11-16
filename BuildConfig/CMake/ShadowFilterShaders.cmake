# RG104-04：过滤变体使用独立布局34..37与b3，原CSM/集群产物名称不变。
file(GLOB _shadow_filter_dependencies CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/*.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Shaders/*.hlsli")
set(_shadow_filter_sources
    PcssCascadedShadowedForward.hlsl
    PcssCascadedEnvironmentForward.hlsl
    PcssCascadedBasicLit.hlsl
    PcssCascadedSkinnedForward.hlsl
    PcssCascadedSkinnedEnvironmentForward.hlsl
    PcssClusteredCascadedShadowedForward.hlsl
    PcssClusteredCascadedEnvironmentForward.hlsl
    PcssClusteredCascadedBasicLit.hlsl
    PcssClusteredCascadedSkinnedForward.hlsl
    PcssClusteredCascadedSkinnedEnvironmentForward.hlsl
)
set(_shadow_filter_modes
    pcss-cascaded-shadowed
    pcss-cascaded-environment
    pcss-cascaded-basiclit
    pcss-cascaded-skinned-forward
    pcss-cascaded-skinned-environment
    pcss-clustered-cascaded-shadowed
    pcss-clustered-cascaded-environment
    pcss-clustered-cascaded-basiclit
    pcss-clustered-cascaded-skinned-forward
    pcss-clustered-cascaded-skinned-environment
 )
list(LENGTH _shadow_filter_sources _shadow_filter_count)
math(EXPR _shadow_filter_last "${_shadow_filter_count}-1")
foreach(_shadow_filter_index RANGE ${_shadow_filter_last})
    list(GET _shadow_filter_sources ${_shadow_filter_index} _shadow_filter_source)
    list(GET _shadow_filter_modes ${_shadow_filter_index} _shadow_filter_mode)
    set(_shadow_filter_input "${CMAKE_SOURCE_DIR}/Resources/Shaders/${_shadow_filter_source}")
    set(_shadow_filter_output "${CMAKE_BINARY_DIR}/Resources/Shaders/${_shadow_filter_source}.sebin")
    add_custom_command(OUTPUT "${_shadow_filter_output}" "${_shadow_filter_output}.json"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/Resources/Shaders"
        COMMAND $<TARGET_FILE:ShaderBuilder> "${_shadow_filter_mode}" "${_shadow_filter_input}" "${_shadow_filter_output}"
        DEPENDS ShaderBuilder ${_shadow_filter_dependencies}
        VERBATIM)
    list(APPEND _shadow_filter_outputs "${_shadow_filter_output}" "${_shadow_filter_output}.json")
endforeach()
add_custom_target(BuildShadowFilterShaders DEPENDS ${_shadow_filter_outputs})
add_dependencies(BuildShaders BuildShadowFilterShaders)
unset(_shadow_filter_outputs)
unset(_shadow_filter_sources)
unset(_shadow_filter_modes)
