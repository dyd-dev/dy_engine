# Only Windows offers a backend choice; both options OFF selects Null.
if(WIN32)
    option(USE_D3D12 "Build the Direct3D 12 backend." OFF)
    option(USE_VULKAN "Build the Vulkan backend." OFF)
    if(USE_METAL)
        message(FATAL_ERROR "Metal is not supported on Windows.")
    endif()
    set(USE_METAL OFF CACHE INTERNAL "Resolved Metal backend selection." FORCE)
    if(USE_D3D12 AND USE_VULKAN)
        message(FATAL_ERROR "Select either USE_D3D12 or USE_VULKAN on Windows.")
    endif()
elseif(APPLE)
    set(USE_D3D12 OFF CACHE INTERNAL "Resolved D3D12 backend selection." FORCE)
    set(USE_METAL ON CACHE INTERNAL "Resolved Metal backend selection." FORCE)
    set(USE_VULKAN OFF CACHE INTERNAL "Resolved Vulkan backend selection." FORCE)
else()
    set(USE_D3D12 OFF CACHE INTERNAL "Resolved D3D12 backend selection." FORCE)
    set(USE_METAL OFF CACHE INTERNAL "Resolved Metal backend selection." FORCE)
    set(USE_VULKAN ON CACHE INTERNAL "Resolved Vulkan backend selection." FORCE)
endif()

# Engine features and diagnostics.
option(DY_ENABLE_SIMD "Enable dy::Math SIMD code paths when supported by the target CPU." ON)
option(DY_ENABLE_TRACY "Embed Tracy profiling instrumentation in engine applications." ON)
option(DY_ENABLE_LIVE_PROFILING "Collect live execution diagnostics in engine applications." OFF)
option(DY_ENABLE_RENDERDOC "Enable RenderDoc capture controls and D3D12 debug events and markers." OFF)

# Optional extensions.
option(DY_EXTEND_MODEL "Build the optional Model extension." OFF)
option(DY_EXTEND_IMGUI "Build the optional Dear ImGui extension." OFF)
option(DY_EXTEND_ADVANCED_RENDERING "Build the optional AdvancedRendering extension." OFF)

# Examples.
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    option(DY_BUILD_EXAMPLES "Build the self-contained dyf and RHI examples." OFF)
endif()

# Platform and dependency settings.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT TARGET glfw)
    set(DY_LINUX_WINDOW_SYSTEM "AUTO" CACHE STRING "Linux window system for GLFW: AUTO, X11, WAYLAND")
    set_property(CACHE DY_LINUX_WINDOW_SYSTEM PROPERTY STRINGS AUTO X11 WAYLAND)
endif()
if(DY_ENABLE_RENDERDOC)
    set(DY_RENDERDOC_ROOT "" CACHE PATH "RenderDoc installation or source containing renderdoc_app.h")
endif()

# Reject incompatible options before downloading dependencies.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT TARGET glfw
   AND NOT DY_LINUX_WINDOW_SYSTEM MATCHES "^(AUTO|X11|WAYLAND)$")
    message(FATAL_ERROR "DY_LINUX_WINDOW_SYSTEM must be AUTO, X11, or WAYLAND.")
endif()
if(DY_ENABLE_RENDERDOC)
    if(APPLE)
        message(FATAL_ERROR "RenderDoc does not support Metal; use Xcode GPU Capture.")
    endif()
    if(USE_D3D12 AND NOT MSVC)
        message(FATAL_ERROR "DY_ENABLE_RENDERDOC with D3D12 requires MSVC for WinPixEventRuntime debug events and markers.")
    endif()
endif()
if(DY_EXTEND_IMGUI
   AND NOT (WIN32 OR CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$"))
    message(FATAL_ERROR "The ImGui extension requires a GLFW desktop window backend.")
endif()
