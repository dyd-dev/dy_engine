include_guard(GLOBAL)

find_program(DY_XCRUN NAMES xcrun REQUIRED)
set(DY_METAL_XCRUN_ARGS -sdk macosx)

# Xcode 26 and newer ship the Metal compiler as a downloadable toolchain.
# When an SDK is selected, xcrun does not always honor TOOLCHAINS, so pass the
# installed toolchain identifier explicitly.
find_program(DY_XCODEBUILD NAMES xcodebuild)
if(DY_XCODEBUILD)
    execute_process(
        COMMAND "${DY_XCODEBUILD}" -showComponent MetalToolchain
        RESULT_VARIABLE metal_component_result
        OUTPUT_VARIABLE metal_component_info
        ERROR_QUIET)
    if(metal_component_result EQUAL 0 AND
       metal_component_info MATCHES "Toolchain Identifier:[ \t]*([^\r\n]+)")
        string(STRIP "${CMAKE_MATCH_1}" metal_toolchain_identifier)
        set(DY_METAL_XCRUN_ARGS --toolchain "${metal_toolchain_identifier}")
    endif()
endif()

execute_process(
    COMMAND "${DY_XCRUN}" ${DY_METAL_XCRUN_ARGS} metal --version
    RESULT_VARIABLE metal_compiler_result
    OUTPUT_QUIET
    ERROR_VARIABLE metal_compiler_error)
if(NOT metal_compiler_result EQUAL 0)
    string(STRIP "${metal_compiler_error}" metal_compiler_error)
    message(FATAL_ERROR "Metal compiler is unavailable: ${metal_compiler_error}")
endif()
