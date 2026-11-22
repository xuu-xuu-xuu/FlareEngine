# cl 使用输出代码页；它可能与 CMake AUTO 采用的输入代码页不同。
# 先以同一进程环境探测输出代码页，再显式解码为 CMake 的 UTF-8。
# NONE 会把后台 ANSI 输出误当 UTF-8，甚至生成不完整的 rules.ninja。
if(MSVC AND CMAKE_GENERATOR MATCHES "^Ninja")
    set(_flare_probe_dir "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/flareengine-msvc-includes")
    file(MAKE_DIRECTORY "${_flare_probe_dir}")
    execute_process(COMMAND "${CMAKE_CXX_COMPILER}" /nologo /MD /utf-8
        "${CMAKE_CURRENT_LIST_DIR}/MsvcOutputEncoding.cpp"
        "/Fo${_flare_probe_dir}/encoding.obj" "/Fe${_flare_probe_dir}/encoding.exe" /link kernel32.lib
        RESULT_VARIABLE _flare_encoding_build OUTPUT_QUIET ERROR_QUIET)
    if(NOT _flare_encoding_build EQUAL 0)
        message(FATAL_ERROR "Cannot compile MSVC output encoding probe: ${_flare_encoding_build}")
    endif()
    execute_process(COMMAND "${_flare_probe_dir}/encoding.exe"
        RESULT_VARIABLE _flare_encoding_status OUTPUT_VARIABLE _flare_output_encoding
        OUTPUT_STRIP_TRAILING_WHITESPACE ENCODING UTF-8)
    if(NOT _flare_encoding_status EQUAL 0 OR NOT _flare_output_encoding MATCHES "^(UTF-8|ANSI|OEM)$")
        message(FATAL_ERROR "Cannot determine MSVC output encoding: ${_flare_output_encoding}")
    endif()
    file(WRITE "${_flare_probe_dir}/encoding.txt" "${_flare_output_encoding}\n")
    file(WRITE "${_flare_probe_dir}/flareengine_msvc_probe.h" "#pragma once\n")
    file(WRITE "${_flare_probe_dir}/probe.cpp" "#include \"flareengine_msvc_probe.h\"\nint flareengine_msvc_probe = 0;\n")
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" /nologo /c /showIncludes /utf-8
            "${_flare_probe_dir}/probe.cpp" "/Fo${_flare_probe_dir}/probe.obj"
        RESULT_VARIABLE _flare_probe_status
        OUTPUT_VARIABLE _flare_probe_stdout
        ERROR_VARIABLE _flare_probe_stderr
        ENCODING ${_flare_output_encoding})
    # 此处保存解码后的 UTF-8，prefix.hex 同样是内部 UTF-8 的字节。
    file(WRITE "${_flare_probe_dir}/stdout.utf8.txt" "${_flare_probe_stdout}")
    file(WRITE "${_flare_probe_dir}/stderr.utf8.txt" "${_flare_probe_stderr}")
    if(NOT _flare_probe_status EQUAL 0)
        message(FATAL_ERROR "MSVC /showIncludes probe failed: ${_flare_probe_status}; see ${_flare_probe_dir}")
    endif()
    string(REPLACE "\r" "" _flare_probe_output "${_flare_probe_stdout}\n${_flare_probe_stderr}")
    string(REGEX MATCH "(^|\n)([^\n]*:)[ \t]+[^\n]*flareengine_msvc_probe[.]h" _flare_probe_match "${_flare_probe_output}")
    if(NOT _flare_probe_match)
        message(FATAL_ERROR "Cannot identify MSVC include prefix; see ${_flare_probe_dir}/stdout.utf8.txt")
    endif()
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "${CMAKE_MATCH_2}")
    string(HEX "${CMAKE_CL_SHOWINCLUDES_PREFIX}" _flare_prefix_hex)
    file(WRITE "${_flare_probe_dir}/prefix.hex" "${_flare_prefix_hex}\n")
    message(STATUS "FlareEngine: decoded MSVC include prefix using its console code page")
    # FlareEngine 也可以作为模板工程的子目录，父工程中的 Main.cpp 使用同一编译器。
    if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(CMAKE_CL_SHOWINCLUDES_PREFIX "${CMAKE_CL_SHOWINCLUDES_PREFIX}" PARENT_SCOPE)
    endif()
endif()
