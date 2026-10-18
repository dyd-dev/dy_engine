# C++ 빌드 SDK와 셰이더 실행 플랫폼을 맞춘다. macOS용 metallib를 iOS에 재사용하지 않는다.
include_guard(GLOBAL)

find_program(DY_XCRUN NAMES xcrun REQUIRED)
set(metal_sdk macosx)
set(DY_METAL_COMPILE_ARGS)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(metal_sdk iphoneos)
    set(metal_target "air64-apple-ios${CMAKE_OSX_DEPLOYMENT_TARGET}")
    if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
        set(metal_sdk iphonesimulator)
        string(APPEND metal_target "-simulator")
    endif()
    # SDK 검색 경로와 별도로 AIR 바이너리의 실행 플랫폼도 지정한다.
    set(DY_METAL_COMPILE_ARGS -target "${metal_target}")
endif()
set(DY_METAL_XCRUN_ARGS -sdk "${metal_sdk}")

# 설치된 Metal 도구 체인을 명시적으로 선택하면서 위에서 정한 SDK도 유지한다.
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
        set(DY_METAL_XCRUN_ARGS --toolchain "${metal_toolchain_identifier}" -sdk "${metal_sdk}")
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
