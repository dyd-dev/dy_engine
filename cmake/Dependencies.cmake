include(FetchContent)

# Desktop windows use GLFW. Android and iOS use their native window APIs.
if(WIN32 OR CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$")
    if(NOT TARGET glfw)
        set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
        set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            set(dy_glfw_window_system "${DY_LINUX_WINDOW_SYSTEM}")
            if(dy_glfw_window_system STREQUAL "AUTO")
                find_package(X11 QUIET)
                if(X11_FOUND)
                    set(dy_glfw_window_system X11)
                else()
                    set(dy_glfw_window_system WAYLAND)
                endif()
            endif()

            if(dy_glfw_window_system STREQUAL "X11")
                if(DY_LINUX_WINDOW_SYSTEM STREQUAL "X11")
                    find_package(X11 REQUIRED)
                endif()
                set(GLFW_BUILD_X11 ON CACHE BOOL "" FORCE)
                set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
            else()
                find_package(PkgConfig REQUIRED)
                pkg_check_modules(WAYLAND_DEPS REQUIRED
                    wayland-client wayland-cursor wayland-egl wayland-protocols xkbcommon)
                find_program(WAYLAND_SCANNER_EXECUTABLE NAMES wayland-scanner
                    NO_CMAKE_FIND_ROOT_PATH REQUIRED)
                set(GLFW_BUILD_X11 OFF CACHE BOOL "" FORCE)
                set(GLFW_BUILD_WAYLAND ON CACHE BOOL "" FORCE)
                # ImGui's pinned GLFW backend also calls GLFW's native X11 API.
                if(DY_EXTEND_IMGUI)
                    find_package(X11 REQUIRED)
                    set(GLFW_BUILD_X11 ON CACHE BOOL "" FORCE)
                endif()
            endif()
            message(STATUS "GLFW: using ${dy_glfw_window_system}")
        endif()

        FetchContent_Declare(glfw
            GIT_REPOSITORY "https://github.com/glfw/glfw.git"
            GIT_TAG "3.4")
        FetchContent_MakeAvailable(glfw)

        function(dy_patch_glfw_win32)
            if(NOT WIN32 OR NOT GLFW_BUILD_WIN32)
                return()
            endif()
            get_target_property(source_dir glfw SOURCE_DIR)
            set(original "${source_dir}/win32_window.c")
            file(READ "${original}" contents)
            set(lookup [=[        window = GetPropW(handle, L"GLFW");]=])
            string(FIND "${contents}" "${lookup}" first)
            string(FIND "${contents}" "${lookup}" last REVERSE)
            if(first LESS 0 OR NOT first EQUAL last)
                message(FATAL_ERROR "GLFW active-window lookup changed; review the Win32 ownership patch")
            endif()
            set(owned_lookup [=[        // Attached input queues can expose another GLFW process or instance.
                window = _glfw.windowListHead;
                while (window && window->win32.handle != handle)
                    window = window->next;]=])
            string(REPLACE "${lookup}" "${owned_lookup}" patched "${contents}")
            set(output_dir "${CMAKE_CURRENT_BINARY_DIR}/glfw-win32-owned")
            set(output "${output_dir}/win32_window.c")
            file(MAKE_DIRECTORY "${output_dir}")
            set(existing "")
            if(EXISTS "${output}")
                file(READ "${output}" existing)
            endif()
            if(NOT existing STREQUAL patched)
                file(WRITE "${output}" "${patched}")
            endif()
            get_target_property(sources glfw SOURCES)
            set(replaced 0)
            set(guarded_sources "")
            foreach(source IN LISTS sources)
                get_filename_component(absolute "${source}" ABSOLUTE BASE_DIR "${source_dir}")
                if(absolute STREQUAL original)
                    list(APPEND guarded_sources "${output}")
                    math(EXPR replaced "${replaced}+1")
                else()
                    list(APPEND guarded_sources "${source}")
                endif()
            endforeach()
            if(NOT replaced EQUAL 1)
                message(FATAL_ERROR "Expected exactly one GLFW Win32 window source to guard")
            endif()
            set_property(TARGET glfw PROPERTY SOURCES "${guarded_sources}")
        endfunction()
        dy_patch_glfw_win32()
    endif()
endif()

# Image loading and DDS decoding are used by the core renderer.
FetchContent_Declare(stb
    GIT_REPOSITORY "https://github.com/nothings/stb.git"
    GIT_TAG "31c1ad37456438565541f4919958214b6e762fb4")
FetchContent_MakeAvailable(stb)

FetchContent_Declare(bcdec
    URL "https://raw.githubusercontent.com/iOrange/bcdec/80859ed3b7afb1c527a2a99d70c61457bea72d0c/bcdec.h"
    URL_HASH "SHA256=134520764d96f70a27db814173616c89ad85db95fbd3328417c7a8c77e7ca189"
    DOWNLOAD_NO_EXTRACT TRUE)
FetchContent_MakeAvailable(bcdec)

# Initial-upload mesh simplification is independent of the graphics backend.
if(NOT TARGET meshoptimizer)
    FetchContent_Declare(meshoptimizer
        GIT_REPOSITORY "https://github.com/zeux/meshoptimizer.git"
        GIT_TAG "v0.25"
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(meshoptimizer)
endif()

# External adapters configure dependencies only when enabled.
if(DY_ENABLE_TRACY)
    if(NOT TARGET Tracy::TracyClient)
        set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
        set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
        set(TRACY_NO_FRAME_IMAGE ON CACHE BOOL "" FORCE)
        FetchContent_Declare(tracy
            GIT_REPOSITORY "https://github.com/wolfpld/tracy.git"
            GIT_TAG "v0.13.1"
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(tracy)
    endif()
endif()

if(DY_ENABLE_RENDERDOC)
    find_path(DY_RENDERDOC_INCLUDE_DIR NAMES renderdoc_app.h
        HINTS "${DY_RENDERDOC_ROOT}" "$ENV{RENDERDOC_ROOT}" "$ENV{ProgramFiles}/RenderDoc"
        PATH_SUFFIXES "" include include/renderdoc renderdoc/api/app REQUIRED)

    if(USE_D3D12 AND NOT TARGET WinPixEventRuntime::WinPixEventRuntime)
        FetchContent_Declare(winpixeventruntime
            URL "https://www.nuget.org/api/v2/package/WinPixEventRuntime/1.0.240308001"
            URL_HASH "SHA256=726acc93d6968e2146261a1e415521747d50ad69894c2b42b5d0d4c29fd66ec4")
        FetchContent_MakeAvailable(winpixeventruntime)
        if(CMAKE_GENERATOR_PLATFORM STREQUAL "ARM64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
            set(DY_WINPIX_ARCH ARM64)
        else()
            set(DY_WINPIX_ARCH x64)
        endif()
        add_library(WinPixEventRuntime SHARED IMPORTED GLOBAL)
        set_target_properties(WinPixEventRuntime PROPERTIES
            IMPORTED_LOCATION "${winpixeventruntime_SOURCE_DIR}/bin/${DY_WINPIX_ARCH}/WinPixEventRuntime.dll"
            IMPORTED_IMPLIB "${winpixeventruntime_SOURCE_DIR}/bin/${DY_WINPIX_ARCH}/WinPixEventRuntime.lib"
            INTERFACE_INCLUDE_DIRECTORIES "${winpixeventruntime_SOURCE_DIR}/Include/WinPixEventRuntime")
        add_library(WinPixEventRuntime::WinPixEventRuntime ALIAS WinPixEventRuntime)
    endif()
endif()

# Deploy the enabled D3D12 marker runtime after consumer executables exist.
if(DY_ENABLE_RENDERDOC AND USE_D3D12)
    function(dy_deploy_runtime)
        set(runtime_files "$<TARGET_FILE:WinPixEventRuntime::WinPixEventRuntime>")
        set(copy_script "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/dy_runtime-$<CONFIG>.cmake")
        add_custom_target(dy_runtime COMMAND "${CMAKE_COMMAND}" -P "${copy_script}" VERBATIM)
        set(directories "${CMAKE_SOURCE_DIR}")
        set(destinations)
        while(directories)
            list(POP_FRONT directories directory)
            get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
            foreach(target IN LISTS targets)
                get_target_property(kind "${target}" TYPE)
                if(kind STREQUAL "EXECUTABLE")
                    add_dependencies(${target} dy_runtime)
                    list(APPEND destinations "$<TARGET_FILE_DIR:${target}>")
                endif()
            endforeach()
            get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
            list(APPEND directories ${children})
        endwhile()
        # Resolve paths here so CMP0112 OLD cannot add consumer dependencies.
        # The shared target refreshes runtime changes without relinking consumers.
        file(GENERATE OUTPUT "${copy_script}" CONTENT
    "set(files [==[${runtime_files}]==])
    set(directories [==[${destinations}]==])
    list(REMOVE_DUPLICATES directories)
    foreach(directory IN LISTS directories)
        file(MAKE_DIRECTORY \"\${directory}\")
        execute_process(COMMAND [==[${CMAKE_COMMAND}]==] -E copy_if_different
            \${files} \"\${directory}\" COMMAND_ERROR_IS_FATAL ANY)
    endforeach()
    ")
    endfunction()
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL dy_deploy_runtime)
endif()
