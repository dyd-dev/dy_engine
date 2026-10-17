# ===== ===== Language (legacy) ===== =====
# set(CMAKE_CXX_STANDARD 20)
# set(CMAKE_CXX_STANDARD_REQUIRED ON)

# abstract target for engine options
add_library(Engine_Options INTERFACE)

# ===== ===== Language ===== =====
target_compile_features(Engine_Options INTERFACE cxx_std_17)

# ===== ===== Include ===== =====
target_include_directories(Engine_Options INTERFACE
	${PROJECT_SOURCE_DIR}/src/Public
)

# ===== ===== SIMD ===== =====
option(DY_ENABLE_SIMD "Enable dy::Math SIMD code paths when supported by the target CPU." ON)
option(DY_ENABLE_TRACY "Embed Tracy profiling instrumentation in engine applications." ON)
option(DY_ENABLE_LIVE_PROFILING "Collect live execution diagnostics in engine applications." OFF)
option(DY_ENABLE_LIVE_CONNECTION "Link the optional live connection for application-owned sessions." OFF)
option(DY_BUILD_EXECUTION_VISUALIZER "Build the independent Windows execution visualizer." OFF)
if(DY_ENABLE_LIVE_PROFILING)
    target_compile_definitions(Engine_Options INTERFACE DY_LIVE_PROFILING_ENABLED=1)
endif()
if(DY_ENABLE_LIVE_CONNECTION)
    target_compile_definitions(Engine_Options INTERFACE DY_LIVE_CONNECTION_ENABLED=1)
endif()
option(DY_TRACY_RAW_PLOTS "Publish per-frame Tracy plots in addition to 5 Hz averages." OFF)
option(DY_ENABLE_RENDERDOC "Enable capture controls for an already injected RenderDoc API." OFF)
option(DY_ENABLE_PIX "Enable D3D12 PIX events using WinPixEventRuntime (MSVC)." ${MSVC})

if(DY_ENABLE_SIMD)
	target_compile_definitions(Engine_Options INTERFACE DY_SIMD_ENABLED=1)

	if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86|i[3-6]86)$")
		if(MSVC)
			target_compile_options(Engine_Options INTERFACE /arch:SSE2)
		elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
			target_compile_options(Engine_Options INTERFACE -msse2)
		endif()
	endif()
endif()

# ===== ===== Compile options ===== =====
if(MSVC)
	target_compile_options(Engine_Options INTERFACE /W4 /utf-8 /wd4201)
else()
	target_compile_options(Engine_Options INTERFACE -Wall -Wextra)
endif()
