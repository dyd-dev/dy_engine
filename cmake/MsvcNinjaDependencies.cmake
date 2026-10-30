# Decode localized /showIncludes using MSVC's Windows ANSI encoding, not the
# console's potentially different encoding, so Ninja can track included headers.
if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja" AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    set(dy_include_probe "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/dy_include_probe")
    file(MAKE_DIRECTORY "${dy_include_probe}")
    file(WRITE "${dy_include_probe}/dy_msvc_probe.h" "\n")
    file(WRITE "${dy_include_probe}/probe.cpp" "#include \"dy_msvc_probe.h\"\n")
    execute_process(COMMAND "${CMAKE_CXX_COMPILER}" /nologo /showIncludes /EP "${dy_include_probe}/probe.cpp"
        RESULT_VARIABLE dy_probe_result OUTPUT_VARIABLE dy_probe_output ERROR_VARIABLE dy_probe_error ENCODING ANSI)
    string(REGEX MATCH "([^\r\n]*:[ \t]+)[A-Za-z]:[/\\\\][^\r\n]*dy_msvc_probe.h" dy_probe_match "${dy_probe_output}${dy_probe_error}")
    if(NOT dy_probe_result EQUAL 0 OR NOT dy_probe_match)
        message(FATAL_ERROR "Cannot verify MSVC include dependency prefix; use a working compiler environment or Visual Studio generator.")
    endif()
    string(REGEX REPLACE "[ \t]+$" " " CMAKE_CL_SHOWINCLUDES_PREFIX "${CMAKE_MATCH_1}")
    set(CMAKE_CXX_CL_SHOWINCLUDES_PREFIX "${CMAKE_CL_SHOWINCLUDES_PREFIX}")
    set(CMAKE_C_CL_SHOWINCLUDES_PREFIX "${CMAKE_CL_SHOWINCLUDES_PREFIX}")
endif()
