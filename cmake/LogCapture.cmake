# Build and deploy the optional native observer; file logging remains platform-independent.
set(dy_log_monitor_supported OFF)
if(WIN32 AND MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8
   AND NOT CMAKE_GENERATOR_PLATFORM MATCHES "[Aa][Rr][Mm]64"
   AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
    set(dy_log_monitor_supported ON)
endif()
option(DY_LOG_CRASH_MONITOR "Build and deploy the Windows x64 crash monitor." ${dy_log_monitor_supported})
if(DY_LOG_CRASH_MONITOR)
    if(NOT dy_log_monitor_supported)
        message(FATAL_ERROR "DY_LOG_CRASH_MONITOR requires Windows x64 and MSVC.")
    endif()
    add_executable(dy_log_monitor "${CMAKE_CURRENT_LIST_DIR}/../src/Platform/WindowsLogMonitor.cpp")
    set_target_properties(dy_log_monitor PROPERTIES OUTPUT_NAME windows_monitor)
    target_compile_features(dy_log_monitor PRIVATE cxx_std_17)
    target_compile_definitions(dy_log_monitor PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(dy_log_monitor PRIVATE /W4 /utf-8)
    target_link_libraries(dy_log_monitor PRIVATE dbghelp)

    # Defer until consumer applications declared after add_subdirectory(dy_engine) exist.
    # A copy target also refreshes the helper when only its source changed.
    function(dy_copy_log_monitor directory)
        get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
        foreach(target IN LISTS targets)
            get_target_property(kind "${target}" TYPE)
            if(kind STREQUAL "EXECUTABLE" AND NOT target STREQUAL "dy_log_monitor")
                # Resolve consumer paths outside the custom target command so
                # CMP0112 OLD cannot add a reverse dependency on the consumer.
                set(copy_script "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${target}_log_runtime-$<CONFIG>.cmake")
                file(GENERATE OUTPUT "${copy_script}" CONTENT
"file(MAKE_DIRECTORY [==[$<TARGET_FILE_DIR:${target}>]==])
execute_process(COMMAND [==[${CMAKE_COMMAND}]==] -E copy_if_different
    [==[$<TARGET_FILE:dy_log_monitor>]==] [==[$<TARGET_FILE_DIR:${target}>]==]
    COMMAND_ERROR_IS_FATAL ANY)
")
                add_custom_target(${target}_log_runtime
                    COMMAND "${CMAKE_COMMAND}" -P "${copy_script}"
                    VERBATIM)
                add_dependencies(${target}_log_runtime dy_log_monitor)
                add_dependencies(${target} ${target}_log_runtime)
            endif()
        endforeach()
        get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
        foreach(child IN LISTS children)
            dy_copy_log_monitor("${child}")
        endforeach()
    endfunction()
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL dy_copy_log_monitor "${CMAKE_SOURCE_DIR}")
endif()
