# ===== ===== Language (legacy) ===== =====
# set(CMAKE_CXX_STANDARD 20)
# set(CMAKE_CXX_STANDARD_REQUIRED ON)

# abstract target for engine options
add_library(Engine_Options INTERFACE)

# ===== ===== Language ===== =====
target_compile_features(Engine_Options INTERFACE cxx_std_17)

# ===== ===== Include ===== =====
target_include_directories(Engine_Options INTERFACE
	${CMAKE_SOURCE_DIR}/src
)

# ===== ===== Compile definition ===== =====
target_compile_definitions(Engine_Options INTERFACE
	ENGINE_VERSION="1.0.0"
	GLFW_INCLUDE_NONE
)

# ===== ===== SIMD ===== =====
option(DY_ENABLE_SIMD "Enable dy::Math SIMD code paths when supported by the target CPU." ON)

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
