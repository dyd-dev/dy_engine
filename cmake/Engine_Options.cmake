# ===== ===== Language (legacy) ===== =====
# set(CMAKE_CXX_STANDARD 20)
# set(CMAKE_CXX_STANDARD_REQUIRED ON)

# abstract target for engine options
add_library(Engine_Options INTERFACE)

# ===== ===== Language ===== =====
target_compile_features(Engine_Options INTERFACE cxx_std_20)

# ===== ===== Include ===== =====
target_include_directories(Engine_Options INTERFACE
	${CMAKE_SOURCE_DIR}/src
)

# ===== ===== Compile definition ===== =====
target_compile_definitions(Engine_Options INTERFACE
	ENGINE_VERSION="1.0.0"
)

# ===== ===== Compile options ===== =====
if(MSVC)
	target_compile_options(Engine_Options INTERFACE /W4 /utf-8)
else()
	target_compile_options(Engine_Options INTERFACE -Wall -Wextra)
endif()