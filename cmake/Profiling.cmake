if(DY_ENABLE_RENDERDOC)
    if(APPLE)
        message(FATAL_ERROR "RenderDoc does not support Metal; use Xcode GPU Capture.")
    endif()
    set(DY_RENDERDOC_ROOT "" CACHE PATH "RenderDoc installation or source containing renderdoc_app.h")
    find_path(DY_RENDERDOC_INCLUDE_DIR NAMES renderdoc_app.h
        HINTS "${DY_RENDERDOC_ROOT}" "$ENV{RENDERDOC_ROOT}" "$ENV{ProgramFiles}/RenderDoc"
        PATH_SUFFIXES "" include include/renderdoc renderdoc/api/app REQUIRED)
    target_include_directories(${PROJECT_NAME} PRIVATE "${DY_RENDERDOC_INCLUDE_DIR}")
    target_compile_definitions(${PROJECT_NAME} PRIVATE DY_RENDERDOC_ENABLED=1)
    if(UNIX AND NOT APPLE)
        target_link_libraries(${PROJECT_NAME} PRIVATE ${CMAKE_DL_LIBS})
    endif()
endif()

if(USE_D3D12 AND WIN32 AND DY_ENABLE_PIX)
    if(NOT MSVC)
        message(FATAL_ERROR "DY_ENABLE_PIX requires the Microsoft C++ compiler supported by WinPixEventRuntime. Use MSVC, or DY_ENABLE_PIX=OFF for D3D12 rendering/timestamps without PIX events.")
    endif()
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
    target_link_libraries(${PROJECT_NAME} PRIVATE WinPixEventRuntime::WinPixEventRuntime)

    # Schedule at the outermost directory so applications declared after
    # add_subdirectory(dy_engine) also receive the required runtime DLL.
    function(dy_copy_pix_runtime directory)
        get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
        foreach(target IN LISTS targets)
            get_target_property(kind ${target} TYPE)
            if(kind STREQUAL "EXECUTABLE")
                add_custom_target(${target}_pix_runtime
                    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "$<TARGET_FILE:WinPixEventRuntime>" "$<TARGET_FILE_DIR:${target}>"
                    VERBATIM)
                add_dependencies(${target} ${target}_pix_runtime)
            endif()
        endforeach()
        get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
        foreach(child IN LISTS children)
            dy_copy_pix_runtime("${child}")
        endforeach()
    endfunction()
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL dy_copy_pix_runtime "${CMAKE_SOURCE_DIR}")
endif()
