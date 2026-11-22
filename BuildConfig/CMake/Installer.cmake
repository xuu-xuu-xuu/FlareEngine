# 零外部运行时依赖的原生 Windows 安装与卸载向导程序（静态链接 CRT，支持干净机器直接双击安装）。
add_executable(FlareInstaller WIN32 Source/ToolLayer/Tools/FlareInstaller.cpp "${CMAKE_CURRENT_BINARY_DIR}/FlareResources.rc")
flare_target(FlareInstaller)
set_property(TARGET FlareInstaller PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_compile_definitions(FlareInstaller PRIVATE UNICODE _UNICODE FLARE_INSTALLER_APP)
target_link_libraries(FlareInstaller PRIVATE user32 gdi32 comctl32 ole32 shell32 shlwapi advapi32 dwmapi uxtheme cabinet bcrypt)
