# W08：独立命令行视频编码器（Windows Media Foundation H.264/MP4）。
# 不链接任何引擎目标，只使用系统 COM / Media Foundation，因此引擎外的项目也能直接
# 调用生成的 VideoEncoder.exe。对用户交付的程序名是 VideoEncoder.exe。
add_executable(FlareVideoEncoder
    Tools/VideoEncoder/VideoEncoder.cpp
    Tools/VideoEncoder/VideoEncoderMp4.cpp
    Tools/VideoEncoder/VideoEncoderPpm.cpp
    Tools/VideoEncoder/VideoEncoderSupport.cpp)
se_target(FlareVideoEncoder)
target_link_libraries(FlareVideoEncoder PRIVATE mfplat mfreadwrite mfuuid ole32 wmcodecdspuuid)
set_target_properties(FlareVideoEncoder PROPERTIES OUTPUT_NAME VideoEncoder)
# 构建入口别名：允许 `cmake --build <dir> --target VideoEncoder` 直接构建交付程序，
# 而 CMake 目标本身仍沿用 FlareVideoEncoder 命名。
add_custom_target(VideoEncoder DEPENDS FlareVideoEncoder)
add_dependencies(SelfEngineEditor FlareVideoEncoder)
